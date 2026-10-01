/* md_render.h — parser markdown de BLOCO + INLINE do modo leitura.
 *
 * Sem nenhuma dependência de ESP/Slint de propósito: o firmware (main.cpp)
 * e o harness offscreen (tools/offscreen_render_harness.cpp) incluem este
 * header, então o PNG de evidência renderiza EXATAMENTE o parser que roda
 * na placa. Mudou o parser -> mudou o render.
 *
 * Estilos de linha (mesmo contrato do .slint, ver editor.slint):
 *   0 = linha crua do modo edição
 *   1 = h1   2 = h2   8 = h3
 *   3 = bullet   4 = citação   5 = código (cerca ou conteúdo)
 *   6 = separador horizontal ("---"/"***")  — SÓ isto vira régua
 *   7 = parágrafo   9 = linha em branco (espaço entre blocos, sem régua)
 *
 * Markdown inline suportado (M4.10): **negrito**, __negrito__ e `código`.
 * Marcador não fechado vira texto literal. Itálico de verdade precisaria de
 * uma face oblíqua (não embutimos; ver docs/UI_WISHLIST.md).
 * Listas aninhadas (M4.11): "  - item" recuado (2 espaços = 1 nível, teto
 * 3) renderiza como bullet deslocado; dentro de cercas ``` não conta.
 */
#pragma once

#include <string>
#include <vector>

/* Um trecho já segmentado da linha. `col` é a coluna em CÉLULAS monoespaçadas
 * (avanço 0.600em por contrato do pipeline de fontes), para o .slint
 * posicionar cada trecho sem HorizontalLayout (que encolheria os Texts e
 * brigaria com o scroll horizontal virtualizado do editor). */
struct MdRunLite {
    std::string text;
    int col = 0;
    int kind = 0;   /* 0 normal 1 negrito 2 código 3 mudo 4 marcador */
};

struct MdLineOut {
    std::string text;
    int sty = 7;
    std::vector<MdRunLite> runs;
};

/* codepoints UTF-8 em s[0..bytes) */
static inline int md_utf8_count(const std::string &s, size_t bytes)
{
    int n = 0;
    for (size_t i = 0; i < bytes && i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80) n++;
    }
    return n;
}

static inline void md_push_words(const std::string &text, int col,
                           int kind, std::vector<MdRunLite> &runs)
{
    /* Quebra o run em PALAVRAS: nenhum Text desenhado contém espaço.
     * Medido em pixel no renderer 1.12: dentro de um Text longo o glifo
     * de espaço pré-rasterizado às vezes avança 2 células (o resto da
     * linha desliza +1 e come o vão do run seguinte). Como a fonte é
     * mono (avanço 0.600em) e `col` é exato, os vãos viram células em
     * branco entre runs de palavra — sem depender do glifo de espaço. */
    int c = col;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ') { c++; i++; continue; }
        size_t j = i;
        while (j < text.size() && text[j] != ' ') j++;
        MdRunLite r;
        r.text = text.substr(i, j - i);
        r.col = c;
        r.kind = kind;
        runs.push_back(r);
        c += md_utf8_count(r.text, r.text.size());
        i = j;
    }
};


/* Segmenta `s` em runs de markdown inline, emitindo em `runs` a partir da
 * coluna `col0` (células). Retorna a coluna final. As colunas contam SÓ o
 * texto exibido: os marcadores (**, __, `) ocupam bytes no arquivo mas
 * zero células na tela (bug da 1ª versão: contava os marcadores e as
 * colunas deslizavam +2 a cada marcador). */
static inline int md_inline(const std::string &s, size_t from,
                            std::vector<MdRunLite> &runs, int col0, int base_kind)
{
    std::string lit;          /* texto literal pendente */
    int cur = col0;           /* coluna (células) do próximo caractere exibido */
    int lit_col = col0;
    bool lit_started = false;
    size_t p = from;

    auto flush = [&]() {
        if (!lit.empty()) {
            md_push_words(lit, lit_col, base_kind, runs);
            cur += md_utf8_count(lit, lit.size());
            lit.clear();
        }
        lit_started = false;
    };

    while (p < s.size()) {
        bool two_b = (s.compare(p, 2, "**") == 0);
        bool two_u = (s.compare(p, 2, "__") == 0);
        bool one_t = (s[p] == '`');
        if (two_b || two_u || one_t) {
            size_t open_len = one_t ? 1 : 2;
            size_t close = s.find(one_t ? "`" : (two_b ? "**" : "__"), p + open_len);
            if (close != std::string::npos && close > p + open_len) {
                flush();
                std::string inner = s.substr(p + open_len, close - p - open_len);
                md_push_words(inner, cur, one_t ? 2 : 1, runs);
                cur += md_utf8_count(inner, inner.size());
                p = close + open_len;
                lit_col = cur;
                continue;
            }
            /* marcador órfão: literal */
        }
        if (!lit_started) { lit_col = cur; lit_started = true; }
        lit.push_back(s[p]);
        p++;
    }
    flush();
    return cur;
}

/* Converte o documento inteiro em linhas de leitura. */
static inline void md_render(const std::string &text, std::vector<MdLineOut> &out)
{
    out.clear();
    bool in_code = false;
    size_t ls = 0;
    for (size_t p = 0; p <= text.size(); p++) {
        if (p == text.size() || text[p] == '\n') {
            std::string ln = text.substr(ls, p - ls);
            ls = p + 1;
            MdLineOut o;
            o.text = ln;
            if (ln.rfind("```", 0) == 0) {
                in_code = !in_code;
                /* a cerca em si vira linha em branco verde (respiro do bloco);
                 * o info-string ("lua") não polui a leitura */
                o.text = "";
                o.sty = 5;
                out.push_back(o);
                continue;
            }
            if (in_code) {
                o.sty = 5;
                md_push_words(ln, 0, 0, o.runs);
                out.push_back(o);
                continue;
            }
            size_t h = 0;
            while (h < ln.size() && ln[h] == '#') h++;
            if (h >= 1 && h <= 3 && (h == ln.size() || ln[h] != '#')) {
                size_t body = (h < ln.size() && ln[h] == ' ') ? h + 1 : h;
                o.text = ln.substr(body);
                o.sty = h == 1 ? 1 : (h == 2 ? 2 : 8);
                md_inline(ln, body, o.runs, 0, 0);
                out.push_back(o);
                continue;
            }
            if (ln == "---" || ln == "***") { o.text = ""; o.sty = 6; out.push_back(o); continue; }
            if (ln.rfind("> ", 0) == 0) {
                o.text = ln.substr(2);
                o.sty = 4;
                MdRunLite bar; bar.text = "|"; bar.col = 0; bar.kind = 3;
                o.runs.push_back(bar);
                md_inline(ln, 2, o.runs, 2, 0);
                out.push_back(o);
                continue;
            }
            /* Bullet com recuo opcional (listas aninhadas, M4.11):
             * cada 2 espaços = 1 nível, teto em 3 níveis (6 células) p/ não
             * empurrar o texto p/ fora da tela de 480 px. Dentro de cerca
             * de código este trecho não roda (o ramo in_code vem antes),
             * então "  - x" indentado DENTRO de ``` continua código. */
            {
                size_t sp = 0;
                while (sp < ln.size() && ln[sp] == ' ') sp++;
                bool dash = sp + 2 <= ln.size() && ln[sp] == '-' && ln[sp + 1] == ' ';
                bool star = sp + 2 <= ln.size() && ln[sp] == '*' && ln[sp + 1] == ' ';
                if (dash || star) {
                    int lvl = (int)(sp / 2);
                    if (lvl > 3) lvl = 3;
                    int col = lvl * 2;
                    o.text = std::string(sp, ' ') + "• " + ln.substr(sp + 2);
                    o.sty = 3;
                    MdRunLite mk; mk.text = "•"; mk.col = col; mk.kind = 4;
                    o.runs.push_back(mk);
                    md_inline(ln, sp + 2, o.runs, col + 2, 0);
                    out.push_back(o);
                    continue;
                }
            }
            if (ln.empty()) { o.text = ""; o.sty = 9; out.push_back(o); continue; }
            o.sty = 7;
            md_inline(ln, 0, o.runs, 0, 0);
            out.push_back(o);
        }
    }
    if (out.empty()) { MdLineOut o; o.text = ""; o.sty = 7; out.push_back(o); }
}
