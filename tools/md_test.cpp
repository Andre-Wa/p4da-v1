/* md_test.cpp — teste de host do md_render.h (o MESMO header que vai na
 * placa e no harness offscreen). Compilar:
 *   g++ -std=c++17 -Wall -Wextra -I main tools/md_test.cpp -o /tmp/mdtest
 * Verifica: estilos de linha, colunas dos runs (células mono), listas
 * aninhadas (M4.11) e regressões da M4.10 (h1, bold inline, crase, hr).
 */
#include "md_render.h"

#include <cstdio>
#include <cstdlib>

static int g_fail = 0;

static void check(bool cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

/* procura a linha cujo texto bruto começa com `prefix` */
static const MdLineOut *find(const std::vector<MdLineOut> &v, const char *prefix)
{
    for (auto &l : v)
        if (l.text.rfind(prefix, 0) == 0) return &l;
    return nullptr;
}

static const MdRunLite *run_at(const MdLineOut &l, size_t i)
{
    return i < l.runs.size() ? &l.runs[i] : nullptr;
}

static void dump(const std::vector<MdLineOut> &v)
{
    for (auto &l : v) {
        printf("    sty=%d |%s|\n", l.sty, l.text.c_str());
        for (auto &r : l.runs)
            printf("        col=%2d kind=%d |%s|\n", r.col, r.kind, r.text.c_str());
    }
}

int main()
{
    /* ---- documento de teste (espelha o _indice.md + casos novos) ---- */
    std::string doc =
        "# Título h1\n"
        "## Seção h2\n"
        "### Subseção h3\n"
        "\n"
        "Parágrafo com acentuação: ação, ção, ímpar.\n"
        "\n"
        "- item de lista\n"
        "  - item recuado\n"
        "    - nível 2\n"
        "      - nível 3\n"
        "          - teto: 10 espaços vira nível 3\n"
        "  - [ ] tarefa aninhada\n"
        "- [x] tarefa feita\n"
        "\n"
        "> citação com **negrito**\n"
        "\n"
        "```lua\n"
        "  - isto NÃO é lista (dentro da cerca)\n"
        "local x = 1\n"
        "```\n"
        "\n"
        "---\n"
        "\n"
        "Texto com **negrito** e `código` e ` crase órfã\n"
        "#título-sem-espaço\n"
        "-x hífen sem espaço\n"
        "-\n";

    std::vector<MdLineOut> v;
    md_render(doc, v);
    dump(v);
    printf("\n== asserções ==\n");

    /* estilos básicos (regressão M4.10) */
    check(find(v, "Título h1") && find(v, "Título h1")->sty == 1, "h1 -> sty 1");
    check(find(v, "Seção h2") && find(v, "Seção h2")->sty == 2, "h2 -> sty 2");
    check(find(v, "Subseção h3") && find(v, "Subseção h3")->sty == 8, "h3 -> sty 8");
    check(find(v, "Parágrafo com") && find(v, "Parágrafo com")->sty == 7, "parágrafo -> sty 7");

    /* hr só no "---" de verdade */
    int hrs = 0, blanks = 0;
    for (auto &l : v) { if (l.sty == 6) hrs++; if (l.sty == 9) blanks++; }
    check(hrs == 1, "exatamente 1 régua (---)");
    check(blanks >= 4, "linhas em branco são respiro (sty 9), não régua");

    /* listas aninhadas (M4.11) */
    const MdLineOut *l0 = find(v, "• item de lista");
    const MdLineOut *l1 = find(v, "  • item recuado");
    const MdLineOut *l2 = find(v, "    • nível 2");
    const MdLineOut *l3 = find(v, "      • nível 3");
    const MdLineOut *l4 = find(v, "          • teto: 10 espaços vira nível 3");
    check(l0 && l0->sty == 3, "nível 0: bullet sty 3");
    check(l1 && l1->sty == 3, "nível 1 (2 espaços) reconhecido");
    check(l2 && l2->sty == 3, "nível 2 (4 espaços) reconhecido");
    check(l3 && l3->sty == 3, "nível 3 (6 espaços) reconhecido");
    check(l4 && l4->sty == 3, "10 espaços -> teto no nível 3");

    /* colunas: marcador no col do nível; texto 2 células depois */
    if (l0 && run_at(*l0, 0) && run_at(*l0, 1))
        check(run_at(*l0, 0)->col == 0 && run_at(*l0, 0)->text == "•" &&
              run_at(*l0, 1)->col == 2, "nível 0: • col 0, texto col 2");
    if (l2 && run_at(*l2, 0) && run_at(*l2, 1))
        check(run_at(*l2, 0)->col == 4 && run_at(*l2, 1)->col == 6,
              "nível 2: • col 4, texto col 6");

    /* tarefa aninhada mantém [ ] literal (como no nível 0) */
    check(find(v, "  • [ ] tarefa aninhada") != nullptr, "tarefa aninhada renderiza");
    check(find(v, "• [x] tarefa feita") != nullptr, "tarefa nível 0 renderiza");

    /* cerca de código: linha recuada NÃO vira bullet */
    check(find(v, "  - isto NÃO é lista") && find(v, "  - isto NÃO é lista")->sty == 5,
          "dentro da cerca: recuo continua código (sty 5)");

    /* citação + inline (regressão M4.10) */
    const MdLineOut *q = find(v, "citação com");
    check(q && q->sty == 4, "citação -> sty 4");
    bool qbold = false;
    if (q) for (auto &r : q->runs) if (r.kind == 1 && r.text == "negrito") qbold = true;
    check(qbold, "negrito dentro de citação (kind 1)");

    /* inline na linha final: negrito, código, crase órfã literal */
    const MdLineOut *p = find(v, "Texto com");
    bool hasbold = false, hascode = false, orphan = false;
    if (p) for (auto &r : p->runs) {
        if (r.kind == 1 && r.text == "negrito") hasbold = true;
        if (r.kind == 2 && r.text == "código") hascode = true;
        if (r.text == "`") orphan = true;
    }
    check(hasbold, "**negrito** inline");
    check(hascode, "`código` inline");
    check(orphan, "crase órfã literal");

    /* h1 sem espaço depois do # */
    check(find(v, "título-sem-espaço") && find(v, "título-sem-espaço")->sty == 1,
          "#título-sem-espaço -> h1");

    /* hífen sem espaço não é bullet */
    check(find(v, "-x hífen sem espaço") && find(v, "-x hífen sem espaço")->sty == 7,
          "'-x ...' (sem espaço) -> parágrafo");
    bool lone = false;
    for (auto &l : v) if (l.text == "-") lone = (l.sty == 7);
    check(lone, "'-' sozinho -> parágrafo");

    printf("\n%s (%d falha(s))\n", g_fail ? "FALHOU" : "TUDO PASSOU", g_fail);
    return g_fail ? 1 : 0;
}
