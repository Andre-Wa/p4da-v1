// Harness: renderiza o EditorScreen offscreen com o software renderer do
// Slint 1.12.1 (mesmo backend do ESP) para validar estilos/cursor em pixel.
#include <slint-platform.h>
#include <memory>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>

class MyWindowAdapter;
static MyWindowAdapter *g_win = nullptr;

class MyWindowAdapter : public slint::platform::WindowAdapter {
public:
    slint::platform::SoftwareRenderer r { slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer };
    slint::platform::AbstractRenderer &renderer() override { return r; }
    slint::PhysicalSize size() override { return slint::PhysicalSize(slint::Size<uint32_t>{800, 480}); }
};

class MyPlatform : public slint::platform::Platform {
public:
    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override {
        auto w = std::make_unique<MyWindowAdapter>();
        g_win = w.get();
        return w;
    }
};

#include "gen.h"
#include "md_render.h"

static void save_ppm(const char *path, const std::vector<slint::Rgb8Pixel> &buf, int w, int h)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (auto &p : buf) { unsigned char c[3] = {p.r, p.g, p.b}; fwrite(c, 1, 3, f); }
    fclose(f);
}

int main(int argc, char **argv)
{
    const int W = 800, H = 480;
    slint::platform::set_platform(std::make_unique<MyPlatform>());
    auto ui = AppWindow::create();
    ui->window().show();

    std::string mode = argc > 1 ? argv[1] : "reading";
    std::string out_mode = mode;   /* nome do arquivo ANTES dos remaps */
    bool scroll = false;
    if (mode == "reading2") { mode = "reading"; scroll = true; }
    // M4.12: painel pulldown aberto / meio arrastado. Renderizados via
    // qs-dragging=true + qs-drag fixo: durante o drag a duração da animação
    // é 0ms, então o frame único do harness mostra o estado exato.
    bool panel = false;
    float panel_h = 0.f;
    if (mode == "panel")     { panel = true; panel_h = 192.f; mode = "reading"; }
    if (mode == "paneldrag") { panel = true; panel_h = 110.f; mode = "reading"; }

    ui->set_active_app(AppState::Editor);
    ui->set_ed_title(slint::SharedString("teste.md"));
    ui->set_ed_content_w(780.f);
    ui->set_ed_offset(scroll ? 28.f * 15 : 0.f);
    ui->set_ed_offset_x(0.f);
    ui->set_ed_dirty(false);
    ui->set_ed_can_delete(true);
    ui->set_ed_show_osk(false);

    std::vector<slint::SharedString> lines;
    std::vector<int> styles;
    if (mode == "reading") {
        ui->set_ed_reading(true);
        // Documento REAL de teste (o que o usuário edita no cartão), passando
        // pelo MESMO parser do firmware (main/md_render.h) — o PNG é a
        // evidência do pipeline inteiro, não de uma cópia dele.
        static const char *doc =
            "# Índice do PDA\n"
            "\n"
            "Nota de teste do renderizador markdown (M4.10). Abra pelo app **Notas** ou\n"
            "pelo **Arquivos** — `.md` abre direto em modo **Ler**; o botão **Editar**\n"
            "mostra o texto cru com cursor.\n"
            "\n"
            "## Seção 2 (h2)\n"
            "\n"
            "Texto normal com acentos: ação, coração, três pássaros voando à mercê\n"
            "do vento — e números 0123456789.\n"
            "\n"
            "### Seção 3 (h3)\n"
            "\n"
            "- item de lista 1\n"
            "- item de lista 2\n"
            "* item com asterisco\n"
            "- [ ] item não checado\n"
            "- [x] item checado\n"
            "\n"
            "> Citação em bloco,\n"
            "> linha continuada vira parágrafo normal (subset de bloco).\n"
            "\n"
            "```lua\n"
            "-- cerca de código: mono + verde\n"
            "pda.log(\"olá do bloco code\")\n"
            "```\n"
            "\n"
            "---\n"
            "\n"
            "Parágrafo após o separador horizontal. **Negrito inline** agora é\n"
            "suportado, assim como `código inline` e a crase sozinha: `.\n"
            "\n"
            "#título-sem-espaço também vira h1 (parser tolerante)\n";
        std::vector<MdLineOut> docout;
        md_render(doc, docout);
        auto runmodels =
            std::make_shared<slint::VectorModel<std::shared_ptr<slint::Model<MdRun>>>>();
        for (auto &l : docout) {
            lines.push_back(slint::SharedString(l.text));
            styles.push_back(l.sty);
            auto rm = std::make_shared<slint::VectorModel<MdRun>>();
            for (auto &r : l.runs) {
                MdRun m;
                m.text = slint::SharedString(r.text);
                m.col = r.col;
                m.kind = r.kind;
                rm->push_back(m);
            }
            runmodels->push_back(rm);
        }
        ui->set_ed_runs(runmodels);
    } else if (mode == "networks") {
        /* M5.0b (A5): tela Redes com amostra p/ auditar layout Slint 1.12 */
        ui->set_active_app(AppState::Networks);
        auto mk = [](std::vector<std::string> v) {
            auto m = std::make_shared<slint::VectorModel<slint::SharedString>>();
            for (auto &x : v) m->push_back(slint::SharedString(x));
            return m;
        };
        auto mb = [](std::vector<bool> v) {
            auto m = std::make_shared<slint::VectorModel<bool>>();
            for (bool x : v) m->push_back(x);
            return m;
        };
        ui->set_net_ssids(mk({"Redstone F3", "Vizinhas-2G", "CafeAberto", "ssid_grande_paguemos_novamente"}));
        ui->set_net_infos(mk({"-42 dBm", "-71 dBm", "-88 dBm", "-55 dBm"}));
        ui->set_net_locked(mb({false, true, true, false}));
        ui->set_net_current(slint::SharedString(""));
        lines = { "", "", "" };
        styles = { 9, 9, 9 };
    } else if (mode == "prompt") {
        ui->set_active_app(AppState::FileManager);
        ui->set_fm_prompt(true);
        ui->set_fm_prompt_needs_text(true);
        ui->set_fm_prompt_title(slint::SharedString("Renomear \"_indice.md\" para:"));
        ui->set_fm_prompt_pre(slint::SharedString("_indi"));
        ui->set_fm_prompt_suf(slint::SharedString("ce.md"));
        auto mk = [](std::vector<std::string> v) {
            auto m = std::make_shared<slint::VectorModel<slint::SharedString>>();
            for (auto &s : v) m->push_back(slint::SharedString(s));
            return m;
        };
        ui->set_ed_osk_r1(mk({"q","w","e","r","t","y","u","i","o","p"}));
        ui->set_ed_osk_r2(mk({"a","s","d","f","g","h","j","k","l","\u00e7"}));
        ui->set_ed_osk_r3(mk({"z","x","c","v","b","n","m"}));
        lines = {"", "", ""};
        styles = {6, 6, 6};
    } else if (mode == "fontpitch") {
        // M4.13 sonda de pitch: 4 linhas de 24 'M' idênticos, um único run
        // cada, nos estilos do modo leitura -> mede o avanço REAL do
        // renderer por (face,tamanho): sty1 h1 bold23, sty2 h2 bold20,
        // sty8 h3 bold18, sty7 parágrafo base18.
        ui->set_ed_reading(true);
        std::string ms(24, 'M');
        lines = { slint::SharedString(ms), slint::SharedString(ms),
                  slint::SharedString(ms), slint::SharedString(ms),
                  slint::SharedString("") };
        styles = { 1, 2, 8, 7, 9 };
        auto runmodels =
            std::make_shared<slint::VectorModel<std::shared_ptr<slint::Model<MdRun>>>>();
        for (int i = 0; i < 5; i++) {
            auto rm = std::make_shared<slint::VectorModel<MdRun>>();
            if (i < 4) {
                MdRun m;
                m.text = slint::SharedString(ms);
                m.col = 0;
                m.kind = 0;
                rm->push_back(m);
            }
            runmodels->push_back(rm);
        }
        ui->set_ed_runs(runmodels);
    } else if (mode == "coltest") {
        ui->set_ed_reading(false);
        // M4.13 sonda de drift: linha 0 = 43 'M' + espaço com cursor BLOCK
        // (overlay vetorial em x = 6 + col*cell); linha 1 = 43 'M' + "|"
        // (glifo in-flow na MESMA coluna 43 — referência do renderer).
        // Se o overlay e o "|" desalinharem, o pitch real do glifo != cell.
        std::string ms(43, 'M');
        lines = { slint::SharedString(ms + " "), slint::SharedString(ms + "|"),
                  slint::SharedString(""), slint::SharedString("") };
        styles = { 0, 0, 0, 0 };
        ui->set_ed_cursor_row(0);
        ui->set_ed_cursor_col(43);
        ui->set_ed_cursor_on(true);
        ui->set_ed_cursor_kind(2);
    } else {
        ui->set_ed_reading(false);
        // modo edição: linha 0 com cursor UNDER embutido (slot ocupado),
        // linha 1 com cursor BLOCK (espaço + Rectangle overlay)
        lines = { "exe_mplo", "ex emplo", "linha 2", "" };
        styles = { 0, 0, 0, 0 };
        ui->set_ed_cursor_row(1);
        ui->set_ed_cursor_col(3);
        ui->set_ed_cursor_on(true);
        ui->set_ed_cursor_kind(2);
    }
    auto lm = std::make_shared<slint::VectorModel<slint::SharedString>>(lines);
    auto sm = std::make_shared<slint::VectorModel<int>>(styles);
    ui->set_ed_lines(lm);
    ui->set_ed_line_style(sm);
    ui->set_status_clock(slint::SharedString("12:34"));

    if (panel) {
        ui->set_cfg_brightness(62.f);
        ui->set_cfg_wifi_info(slint::SharedString("Redstone"));
        ui->set_status_wifi(slint::SharedString("wifi -54 dBm"));
        ui->set_status_batt(slint::SharedString("87%"));
        ui->set_status_store(slint::SharedString("SD ok"));
        ui->set_status_store_icon(slint::SharedString("sd"));
        ui->set_status_pwr(slint::SharedString("ATIVO"));
        ui->set_qs_dragging(true);
        ui->set_qs_drag(panel_h);
    }

    std::vector<slint::Rgb8Pixel> buf(W * H);
    g_win->r.render(std::span<slint::Rgb8Pixel>(buf.data(), buf.size()), W);
    std::string out = std::string("/tmp/harness/render_") +
                      (scroll ? "reading2" : out_mode) + ".ppm";
    save_ppm(out.c_str(), buf, W, H);
    printf("saved %s\n", out.c_str());
    return 0;
}
