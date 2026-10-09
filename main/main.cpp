/**
 * main.cpp — PDA (milestone M2)
 *
 * Bring-up: storage -> config(Lua) -> display -> touch -> Slint -> lua VM
 * -> power mgmt. Toda I/O de disco roda em threads; a UI nunca bloqueia.
 * Scripts Lua rodam serializados por mutex (uma VM, uma execução por vez).
 *
 * M2: editor com cursor (teclado + toque + teclado virtual), abertura de
 * qualquer arquivo texto pelo gerenciador, save de volta no mesmo caminho.
 */

#include "slint-esp.h"
#include "app_ui.h"   // gerado a partir de ui/app_ui.slint
#include "md_render.h"  // parser markdown de bloco+inline (compartilhado c/ harness)

#include "board_config.h"
#include "display_init.h"
#include "touch_init.h"
#include "usb_hid_keyboard.h"
#include "storage_init.h"
#include "pda_config.h"
#include "lua_runtime.h"
#include "power_mgmt.h"
#include "wifi_net.h"
#include "audio_uac.h"
#include "nvs_flash.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pthread.h"
#include "esp_heap_caps.h"

#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <functional>
#include <algorithm>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <strings.h>
#include <utility>
#include <dirent.h>

static const char *TAG = "main";

static AppWindow *g_ui = nullptr;   /* ponteiro do component (handle vive em app_main) */
static std::shared_ptr<slint::VectorModel<slint::SharedString>> g_script_log;
static std::mutex g_lua_mtx;
static std::string g_fm_dir;
static bool g_hid_seen = false;
static std::string s_session_app = "launcher";
static std::string s_session_note = "";
/* M4.15.3: sessão parseada no bring-up, aplicada no event loop. */
static std::string s_rest_app, s_rest_note, s_rest_path;
static bool s_rest_pending = false;

/* Geometria do editor — ESPELHA os tokens ESTÁTICOS do theme.slint
 * (line-h 28, osk-h 193, status-h 34, btn-h-sm 39, body 18px => pitch
 * mono 11px — o renderer grava o avanço na grade de pixels: round(0.6em);
 * ver ED_CHAR_W e docs/UI.md "grade de pixel").
 * Modelo 100% estático: escala de UI se muda com tools/scale_type.py +
 * rebuild (o renderer pré-rasteriza fontes; e tokens mutáveis em runtime
 * já nos custaram um bootloop por NaN). Se mudar token lá, mude aqui.
 * line-h 28 (M4.7): a line-box do PDA Mono é 1.3188em — h1 23px dá 30.3px,
 * MAIOR que a fileira de 28px. Text com height menor que a line-box
 * renderiza ZERO linhas no renderer 1.12 (título em branco, 2x), então o
 * MdRow do editor.slint dá folga vertical centrada p/ fsize >= 22px. */
static const float ED_LINE_H = 28.f;
static const float ED_OSK_H = 193.f;
static const float ED_STATUS_H = 34.f;
static const float ED_BTN_SM = 39.f;
/* M4.13: o renderer Slint 1.12 grava o avanço do glifo na grade de pixels,
 * então o pitch real é round(0.600em × font-size) e não 0.600em exato.
 * Medido no harness offscreen (sonda "fontpitch"): 18px -> 11px, 20px -> 12px,
 * 23px -> 14px, todos com min == max == média (pitch constante, sem drift).
 * Com 10.8f o overlay do cursor-bloco derivava ~0.2px/col e chegava a 9px
 * (quase uma célula) no fim da linha — visível só no cursor de bloco porque
 * "|" e "_" são glifos embutidos no texto e acompanham o avanço real.
 * Mantém em sincronia com `cell` em main/ui/screens/editor.slint. */
static const float ED_CHAR_W = 11.0f;

static void activity(void) { power_mgmt_activity(); }

/* forward decls (ordem de definição vs uso) */
static void refresh_notes_list(void);
static void refresh_scripts_list(void);
static void refresh_music_list(void);
static void push_mus_ui(void);
static void list_dir_async(const std::string dir);
static void nav_goto(AppState st);
static void nav_back(void);
static size_t utf8_next(const std::string &t, size_t pos);
static size_t utf8_count(const std::string &t, size_t end);

/** std::thread com pilha explícita: o default do IDF (~3K) estoura a VM Lua
 *  e é apertado p/ readdir+FATFS. Chamado NA task que cria a thread. */
/* Threads de I/O e de scripts como tasks FreeRTOS com pilha em PSRAM:
 * a RAM interna ficou escassa com o ESP-Hosted e o std::thread falhava
 * com "pthread: Failed to create task!" -> exceção -> abort (2026-09-24).
 * Semântica de detach: a task se auto-apaga ao terminar. */
struct ThreadCtx {
    std::function<void()> fn;
};

static void thread_trampoline(void *arg)
{
    ThreadCtx *c = static_cast<ThreadCtx *>(arg);
    c->fn();
    delete c;
    vTaskDelete(NULL);
}

template <typename F>
static void spawn_thread(const char *name, size_t stack, F &&fn)
{
    ThreadCtx *ctx = new (std::nothrow) ThreadCtx { std::forward<F>(fn) };
    if (!ctx) {
        ESP_LOGE(TAG, "spawn_thread(%s): sem memória p/ contexto", name);
        return;
    }
    BaseType_t ok = xTaskCreateWithCaps(thread_trampoline, name, (uint32_t)stack,
                                        ctx, 5, NULL, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "spawn_thread(%s): xTaskCreateWithCaps falhou", name);
        delete ctx;
    }
}

static void log_line(const char *line, void *ctx)
{
    (void)ctx;
    std::string s(line);
    slint::invoke_from_event_loop([s]() {
        if (g_script_log) {
            g_script_log->push_back(slint::SharedString(s));
            while (g_script_log->row_count() > 200) g_script_log->erase(0);
        }
    });
}

static void toast(const char *msg, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "toast: %s", msg);
    char buf[300];
    snprintf(buf, sizeof(buf), "[toast] %s", msg);
    log_line(buf, NULL);
}

static void apply_brightness_from_settings(void)
{
    if (power_mgmt_state() == PDA_PWR_ACTIVE) {
        board_display_backlight_set((uint8_t)pda_settings()->brightness);
    }
}

static void settings_changed_by_lua(void)
{
    slint::invoke_from_event_loop([]() { apply_brightness_from_settings(); });
}

/* ---------------- redes (M4b) ---------------- */
static std::vector<wifi_net_ap_t> g_aps;
static std::string g_wifi_pick_ssid;

static void on_scan_done(const wifi_net_ap_t *aps, int count, void *ctx)
{
    (void)ctx;
    g_aps.clear();
    for (int i = 0; i < count; i++) g_aps.push_back(aps[i]);
    auto ssids = std::make_shared<slint::VectorModel<slint::SharedString>>();
    auto infos = std::make_shared<slint::VectorModel<slint::SharedString>>();
    auto locked = std::make_shared<slint::VectorModel<bool>>();
    for (int i = 0; i < count; i++) {
        ssids->push_back(slint::SharedString(aps[i].ssid));
        char b[24];
        snprintf(b, sizeof(b), "%d dBm", aps[i].rssi);
        infos->push_back(slint::SharedString(b));
        locked->push_back(!aps[i].open);
    }
    if (count == 0) {
        ssids->push_back(slint::SharedString("(nenhuma rede encontrada)"));
        infos->push_back(slint::SharedString(""));
        locked->push_back(false);
    }
    slint::invoke_from_event_loop([ssids, infos, locked]() {
        g_ui->set_net_ssids(ssids);
        g_ui->set_net_infos(infos);
        g_ui->set_net_locked(locked);
        g_ui->set_net_current(slint::SharedString(
            wifi_net_connected() ? wifi_net_ssid() : "(offline)"));
    });
}

static void net_start_scan(void)
{
    /* M5.3.2: rádio desligado → aviso em vez de scan que falha. */
    if (!wifi_net_enabled()) {
        log_line("[wifi] rádio desligado — ligue em Config > Redes", NULL);
        g_ui->set_net_current(slint::SharedString("(rádio desligado)"));
        return;
    }
    /* Longe da rede salva, o loop de reconexão a cada 2 s atrapalha o
     * scan (rádio ocupado); pausa durante a varredura da tela Redes. */
    wifi_net_set_autoreconnect(false);
    if (wifi_net_scan(on_scan_done, NULL) != ESP_OK) {
        log_line("[erro] scan não iniciado", NULL);
        wifi_net_set_autoreconnect(true);
    }
}

/* ---------------- relógio da status bar --------------------------- */
extern "C" void wifi_net_on_event_ui(bool connected, int rssi)
{
    char buf[40];
    if (connected) snprintf(buf, sizeof(buf), "wifi %d dBm", rssi);
    else snprintf(buf, sizeof(buf), "wifi --");
    std::string s(buf);
    slint::invoke_from_event_loop([s]() {
        g_ui->set_status_wifi(slint::SharedString(s));
        g_ui->set_cfg_wifi_info(slint::SharedString(
            wifi_net_connected() ? wifi_net_ssid() : "offline"));
    });
}

static void update_clock(void)
{
    char buf[32];
    const char *hhmm = wifi_net_time_hhmm();
    if (hhmm) {
        snprintf(buf, sizeof(buf), "%s", hhmm);
    } else {
        int64_t up_s = esp_timer_get_time() / 1000000LL;
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 (int)(up_s / 3600), (int)((up_s / 60) % 60), (int)(up_s % 60));
    }
    g_ui->set_status_clock(slint::SharedString(buf));

    const char *pwr = "ATIVO";
    switch (power_mgmt_state()) {
    case PDA_PWR_DIM: pwr = "DIM"; break;
    case PDA_PWR_STANDBY: pwr = "STANDBY"; break;
    default: pwr = "ATIVO"; break;
    }
    g_ui->set_status_pwr(slint::SharedString(pwr));
}

/* ================================================================== */
/* EDITOR                                                              */
/* ================================================================== */
struct EditorState {
    std::string path;       /* caminho completo; "" = ainda sem arquivo */
    std::string text;
    size_t cursor = 0;
    bool dirty = false;
    bool is_new = false;
    int osk_override = -1;     /* -1 auto, 0 força off, 1 força on */
    bool reading = false;      /* modo leitura (markdown renderizado) */
    bool osk_shift = false;
    bool osk_mode = false;     /* false = abc, true = 123/símbolos */
};
static EditorState g_ed;

static std::string notes_dir(void)
{
    char buf[160];
    pda_path(buf, sizeof(buf), "notes");
    return std::string(buf);
}

static std::string ed_title(void)
{
    if (g_ed.path.empty()) return std::string("(novo)");
    std::string root = pda_root();
    if (g_ed.path.rfind(root + "/", 0) == 0) return g_ed.path.substr(root.size() + 1);
    return g_ed.path;
}

static bool ed_osk_should_show(void)
{
    if (g_ed.reading) return false;
    if (g_ed.osk_override >= 0) return g_ed.osk_override == 1;
    return pda_settings()->onscreen_keyboard_auto && !usb_hid_keyboard_connected();
}

/* ---- vista virtualizada do editor (M4.6) ----
 * C++ dono da posição; UI recebe fatia visível + estilos por linha.
 * Modos: edição (linhas cruas + cursor) e leitura (markdown renderizado). */
static std::vector<std::string> g_ed_disp;    /* modo edição (cursor embutido) */
static std::vector<std::string> g_md_text;    /* modo leitura */
static std::vector<int> g_md_style;
static std::shared_ptr<slint::VectorModel<slint::SharedString>> g_ed_model;
static std::shared_ptr<slint::VectorModel<int>> g_ed_style_model;
/* runs de markdown inline por linha da fatia visível (modelo de modelos) */
static std::shared_ptr<slint::VectorModel<std::shared_ptr<slint::Model<MdRun>>>> g_ed_runs_model;
static float s_ed_top = 0.f;
static float s_ed_left = 0.f;
static bool s_cursor_on = true;
static int s_cur_line = 0;   /* posição do cursor p/ overlay do block (M4.7) */
static int s_cur_col = 0;    /* coluna em BYTES (scroll horizontal) */
static int s_cur_cell = 0;   /* coluna em CÉLULAS/codepoints (overlay .slint) */
static esp_timer_handle_t s_blink_t = nullptr;   /* criado em app_main */

/* M4.12 — pulldown de ajustes rápidos: o slider de brilho aplica o
 * backlight AO VIVO (on_qs_brightness) mas só grava o system.lua quando o
 * painel fecha (on_qs_state). Sem esse debounce cada pixel de arrasto vira
 * uma escrita no cartão. */
static bool s_qs_dirty = false;

/* Rearma a fase do piscar: o cursor acende já e a próxima alternância só
 * vem um ciclo inteiro depois. Chamado a cada atividade de cursor (digita,
 * seta, toque) para ele não sumir logo após um toque de tecla. */
static void cursor_rearm(void)
{
    s_cursor_on = true;
    if (s_blink_t != nullptr) {
        esp_timer_stop(s_blink_t);   /* não rodando -> retorna erro, inofensivo */
        esp_timer_start_periodic(s_blink_t, 530 * 1000);
    }
}

/* 0=bar 1=under 2=block (M4.7: block é overlay Rectangle no .slint —
 * "█" não existe em nenhuma fonte embutida). */
static int cursor_kind(void)
{
    const char *st = pda_settings()->cursor_style;
    if (!strcmp(st, "under")) return 1;
    if (!strcmp(st, "block")) return 2;
    return 0;
}

static const char *cursor_glyph(void)
{
    return cursor_kind() == 1 ? "_" : "|";
}

/* Markdown de bloco+inline (M4.10): o parser mora em md_render.h e é o
 * MESMO que o harness offscreen renderiza (evidência sem hardware).
 * Estilos: 0=mono edição 1=h1 2=h2 8=h3 3=bullet 4=citação 5=código
 * 6=hr (SÓ "---"/"***") 7=parágrafo 9=linha em branco (sem régua).
 * Tolerante: aceita "#" sem espaço e até 3 níveis; BOM/CRLF saneados no
 * ed_load. Inline: **negrito**, __negrito__ e `código` (runs por linha). */
static std::vector<std::vector<MdRunLite>> g_md_runs;

static void md_render_doc(const std::string &text)
{
    std::vector<MdLineOut> doc;
    md_render(text, doc);
    g_md_text.clear(); g_md_style.clear(); g_md_runs.clear();
    g_md_text.reserve(doc.size());
    g_md_style.reserve(doc.size());
    g_md_runs.reserve(doc.size());
    for (auto &l : doc) {
        g_md_text.push_back(l.text);
        g_md_style.push_back(l.sty);
        g_md_runs.push_back(std::move(l.runs));
    }
}

static int ed_area_h(void)
{
    return 480 - (int)ED_STATUS_H - ((int)ED_BTN_SM + 14) - 16 -
           (ed_osk_should_show() ? (int)ED_OSK_H + 6 : 0);
}

static int ed_area_w(void) { return 800 - 2 * 9 - 2; }

static void ed_refresh_view(void)
{
    const std::vector<std::string> &src = g_ed.reading ? g_md_text : g_ed_disp;
    const std::vector<int> &srcs = g_md_style;

    float content_h = (float)src.size() * ED_LINE_H;
    float maxtop = content_h - (float)ed_area_h();
    if (maxtop < 0) maxtop = 0;
    if (s_ed_top < 0) s_ed_top = 0;
    if (s_ed_top > maxtop) s_ed_top = maxtop;
    float maxleft = g_ui->get_ed_content_w() - (float)ed_area_w();
    if (maxleft < 0) maxleft = 0;
    if (s_ed_left < 0) s_ed_left = 0;
    if (s_ed_left > maxleft) s_ed_left = maxleft;

    int start = (int)(s_ed_top / ED_LINE_H);
    int rows = ed_area_h() / (int)ED_LINE_H + 2;
    if (!g_ed_model) {
        g_ed_model = std::make_shared<slint::VectorModel<slint::SharedString>>();
        g_ui->set_ed_lines(g_ed_model);
    }
    if (!g_ed_style_model) {
        g_ed_style_model = std::make_shared<slint::VectorModel<int>>();
        g_ui->set_ed_line_style(g_ed_style_model);
    }
    if (!g_ed_runs_model) {
        g_ed_runs_model =
            std::make_shared<slint::VectorModel<std::shared_ptr<slint::Model<MdRun>>>>();
        g_ui->set_ed_runs(g_ed_runs_model);
    }
    std::vector<slint::SharedString> tv;
    std::vector<int> sv;
    std::vector<std::shared_ptr<slint::Model<MdRun>>> rv;
    for (int i = start; i < (int)src.size() && i < start + rows; i++) {
        tv.push_back(slint::SharedString(src[i]));
        sv.push_back(g_ed.reading ? srcs[i] : 0);
        auto rm = std::make_shared<slint::VectorModel<MdRun>>();
        if (g_ed.reading && i < (int)g_md_runs.size()) {
            for (const auto &r : g_md_runs[i]) {
                MdRun m;
                m.text = slint::SharedString(r.text);
                m.col = r.col;
                m.kind = r.kind;
                rm->push_back(m);
            }
        }
        rv.push_back(rm);
    }
    if (tv.empty()) { tv.push_back(slint::SharedString("")); sv.push_back(0); }
    size_t nslice = tv.size();
    g_ed_model->set_vector(std::move(tv));
    g_ed_style_model->set_vector(std::move(sv));
    g_ed_runs_model->set_vector(std::move(rv));
    g_ui->set_ed_offset(s_ed_top - (float)start * ED_LINE_H);
    g_ui->set_ed_offset_x(s_ed_left);
    /* overlay do cursor-bloco (M4.7): linha dentro da fatia + coluna */
    int crow = -1;
    if (!g_ed.reading && cursor_kind() == 2 && s_cursor_on) {
        int r = s_cur_line - start;
        if (r >= 0 && r < (int)nslice) crow = r;
    }
    g_ui->set_ed_cursor_row(crow);
    g_ui->set_ed_cursor_col(s_cur_cell);
    g_ui->set_ed_cursor_on(s_cursor_on);
    g_ui->set_ed_cursor_kind(cursor_kind());
}

static void ed_ensure_cursor_visible(int cursor_line, int cursor_col)
{
    float cy = (float)cursor_line * ED_LINE_H;
    if (cy < s_ed_top) s_ed_top = cy;
    else if (cy + ED_LINE_H > s_ed_top + (float)ed_area_h())
        s_ed_top = cy + ED_LINE_H - (float)ed_area_h();
    float cx = (float)cursor_col * ED_CHAR_W + 6;
    if (cx < s_ed_left) s_ed_left = cx;
    else if (cx + ED_CHAR_W > s_ed_left + (float)ed_area_w())
        s_ed_left = cx + ED_CHAR_W - (float)ed_area_w();
}

static void ed_push_ui(bool scroll_to_cursor)
{
    /* atividade de cursor -> pisca recomeça com ele aceso */
    if (scroll_to_cursor && !g_ed.reading) cursor_rearm();

    std::vector<std::string> lines;
    int cursor_line = 0, cursor_col = 0;
    {
        size_t i = 0, line_idx = 0, line_start = 0;
        for (; i <= g_ed.text.size(); i++) {
            if (i == g_ed.text.size() || g_ed.text[i] == '\n') {
                lines.push_back(g_ed.text.substr(line_start, i - line_start));
                if (g_ed.cursor >= line_start && g_ed.cursor <= i) {
                    cursor_line = (int)line_idx;
                    cursor_col = (int)(g_ed.cursor - line_start);
                }
                line_idx++;
                line_start = i + 1;
            }
        }
    }
    if (lines.empty()) lines.push_back("");

    /* modo edição: cursor visível (pisca via s_cursor_on).
     * M4.7: o cursor OCUPA O SLOT do caractere em vez de inserir uma
     * célula extra — "exemplo" com cursor na col 3 vira "exe_plo", não
     * "exe_mplo". O caractere sob o cursor some enquanto ele está aceso
     * (bar/under: glifo no lugar; block: espaço + Rectangle no .slint).
     * No fim da linha o glifo é acrescentado após o último char. */
    g_ed_disp = lines;
    if (!g_ed.reading) {
        if ((size_t)cursor_col > g_ed_disp[cursor_line].size())
            cursor_col = (int)g_ed_disp[cursor_line].size();
        s_cur_line = cursor_line;
        s_cur_col = cursor_col;
        s_cur_cell = (int)utf8_count(lines[cursor_line], (size_t)cursor_col);
        if (s_cursor_on) {
            std::string &L = g_ed_disp[cursor_line];
            size_t cc = (size_t)cursor_col;
            size_t nx = (cc < L.size()) ? utf8_next(L, cc) : cc;
            if (cursor_kind() == 2) {
                L.replace(cc, nx - cc, " ");
            } else if (cc < L.size()) {
                L.replace(cc, nx - cc, cursor_glyph());
            } else {
                L += cursor_glyph();
            }
        }
    } else {
        s_cur_line = cursor_line;
        s_cur_col = cursor_col;
        s_cur_cell = (int)utf8_count(lines[cursor_line], (size_t)cursor_col);
    }
    md_render_doc(g_ed.text);

    size_t maxw = 0;
    const std::vector<std::string> &wsrc = g_ed.reading ? g_md_text : g_ed_disp;
    for (auto &d : wsrc) if (d.size() > maxw) maxw = d.size();

    static float s_last_w = -1.f;
    static std::string s_last_title;
    static int s_last_flags = -1;
    float want_w = (float)(maxw * ED_CHAR_W + 60);
    if (fabsf(want_w - s_last_w) > 0.5f) { g_ui->set_ed_content_w(want_w); s_last_w = want_w; }
    std::string title = ed_title();
    if (title != s_last_title) { g_ui->set_ed_title(slint::SharedString(title)); s_last_title = title; }
    bool can_del = g_ed.path.rfind(notes_dir() + "/", 0) == 0;
    bool osk = ed_osk_should_show();
    int flags = (g_ed.dirty ? 1 : 0) | (can_del ? 2 : 0) | (osk ? 4 : 0) | (g_ed.reading ? 8 : 0);
    if (flags != s_last_flags) {
        g_ui->set_ed_dirty(g_ed.dirty);
        g_ui->set_ed_can_delete(can_del);
        g_ui->set_ed_show_osk(osk);
        g_ui->set_ed_reading(g_ed.reading);
        s_last_flags = flags;
    }

    if (scroll_to_cursor && !g_ed.reading) ed_ensure_cursor_visible(cursor_line, cursor_col);
    ed_refresh_view();
}

static void ed_insert_str(const char *s, size_t n)
{
    if (g_ed.cursor > g_ed.text.size()) g_ed.cursor = g_ed.text.size();
    g_ed.text.insert(g_ed.cursor, s, n);
    g_ed.cursor += n;
    g_ed.dirty = true;
    ed_push_ui(true);
}

/* ---- UTF-8: o cursor anda por CODEPOINTS, nunca por bytes ----
 * Sem isso, setas/backspace em texto acentuado pousavam no meio de uma
 * sequência multibyte e o Slint (Rust) panicava com UTF-8 inválido
 * (Guru Meditation em 2026-09-24). */
static bool utf8_is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static size_t utf8_prev(const std::string &t, size_t pos)
{
    if (pos == 0) return 0;
    size_t p = pos - 1;
    while (p > 0 && utf8_is_cont(t[p])) p--;
    return p;
}

static size_t utf8_next(const std::string &t, size_t pos)
{
    if (pos >= t.size()) return t.size();
    size_t n = pos + 1;
    while (n < t.size() && utf8_is_cont(t[n])) n++;
    return n;
}

/* codepoints em t[0..end) — p/ converter coluna-bytes em coluna-células
 * (o overlay do cursor-bloco no .slint posiciona por célula mono). */
static size_t utf8_count(const std::string &t, size_t end)
{
    size_t n = 0;
    for (size_t i = 0; i < end && i < t.size(); i++)
        if (!utf8_is_cont(t[i])) n++;
    return n;
}

/* snap p/ trás até um limite de codepoint */
static size_t utf8_snap(const std::string &t, size_t pos)
{
    while (pos > 0 && pos < t.size() && utf8_is_cont(t[pos])) pos--;
    return pos;
}

static void ed_backspace(void)
{
    if (g_ed.cursor == 0 || g_ed.text.empty()) return;
    size_t prev = utf8_prev(g_ed.text, g_ed.cursor);
    g_ed.text.erase(prev, g_ed.cursor - prev);
    g_ed.cursor = prev;
    g_ed.dirty = true;
    ed_push_ui(true);
}

static void ed_delete_key(void)
{
    if (g_ed.cursor >= g_ed.text.size()) return;
    size_t next = utf8_next(g_ed.text, g_ed.cursor);
    g_ed.text.erase(g_ed.cursor, next - g_ed.cursor);
    g_ed.dirty = true;
    ed_push_ui(true);
}

static void ed_move(int dcol, int dline, bool home, bool end)
{
    /* reconstrói linha/coluna atuais */
    size_t line_start = 0;
    int line_idx = 0;
    for (size_t i = 0; i < g_ed.cursor; i++) {
        if (g_ed.text[i] == '\n') { line_start = i + 1; line_idx++; }
    }
    int col = (int)(g_ed.cursor - line_start);

    if (home) { g_ed.cursor = line_start; ed_push_ui(true); return; }
    if (end) {
        size_t i = line_start;
        while (i < g_ed.text.size() && g_ed.text[i] != '\n') i++;
        g_ed.cursor = i;
        ed_push_ui(true);
        return;
    }
    if (dline != 0) {
        /* acha a linha alvo e clamp a coluna nela */
        int target = line_idx + dline;
        size_t ls = 0;
        int idx = 0;
        std::vector<std::pair<size_t, size_t>> spans;   /* start, end(incl newline pos) */
        size_t s0 = 0;
        for (size_t i = 0; i <= g_ed.text.size(); i++) {
            if (i == g_ed.text.size() || g_ed.text[i] == '\n') {
                spans.push_back({ s0, i });
                s0 = i + 1;
            }
        }
        if (spans.empty()) spans.push_back({ 0, 0 });
        if (target < 0) target = 0;
        if (target >= (int)spans.size()) target = (int)spans.size() - 1;
        size_t len = spans[target].second - spans[target].first;
        size_t c = (size_t)col > len ? len : (size_t)col;
        g_ed.cursor = utf8_snap(g_ed.text, spans[target].first + c);
        (void)ls; (void)idx;
        ed_push_ui(true);
        return;
    }
    if (dcol > 0) {
        g_ed.cursor = utf8_next(g_ed.text, g_ed.cursor);
    } else if (dcol < 0) {
        g_ed.cursor = utf8_prev(g_ed.text, g_ed.cursor);
    }
    ed_push_ui(true);
}

static void ed_load(const std::string &path, const std::string &content_in,
                    bool is_new, bool start_reading)
{
    /* M4.7: saneamento de texto vindo do cartão/USB: BOM UTF-8 na 1ª linha
     * cegava o md_render ("# título" virava parágrafo cru) e \r de CRLF
     * vazava para as linhas do .slint. */
    std::string content = content_in;
    if (content.size() >= 3 && (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
        content.erase(0, 3);
    if (content.find('\r') != std::string::npos) {
        std::string norm;
        norm.reserve(content.size());
        for (size_t i = 0; i < content.size(); i++) {
            if (content[i] == '\r') {
                if (i + 1 < content.size() && content[i + 1] == '\n') continue;
                norm.push_back('\n');
            } else {
                norm.push_back(content[i]);
            }
        }
        content.swap(norm);
    }
    g_ed.path = path;
    g_ed.text = content;
    g_ed.cursor = 0;
    g_ed.dirty = false;
    g_ed.is_new = is_new;
    g_ed.reading = start_reading;
    g_ed.osk_override = -1;
    s_ed_top = 0.f;
    s_ed_left = 0.f;
    ed_push_ui(false);
    nav_goto(AppState::Editor);
    s_session_app = "noteedit";
    s_session_note = is_new ? std::string("") : ed_title();
}

static void ed_open_path_async(const std::string path, bool start_reading)
{
    spawn_thread("io_edit", 8192, [path, start_reading]() {
        char *data = NULL; size_t len = 0;
        std::string content;
        if (storage_read_file_alloc(path.c_str(), &data, &len) == ESP_OK) {
            content.assign(data, len);
            free(data);
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "[erro] não abriu %s", path.c_str());
            log_line(buf, NULL);
            return;
        }
        slint::invoke_from_event_loop([path, content, start_reading]() {
            ed_load(path, content, false, start_reading);
        });
    });
}

static void ed_save(void)
{
    if (g_ed.path.empty()) return;
    if (storage_write_text_file(g_ed.path.c_str(), g_ed.text.data(), g_ed.text.size()) == ESP_OK) {
        g_ed.dirty = false;
        g_ed.is_new = false;
        /* Se o usuário editou o próprio system.lua, o buffer dele é a nova
         * fonte da verdade: recarrega em memória (com sanitize) para o
         * firmware não voltar a escrever valores velhos por cima
         * (loop de "sobrescrita no boot" de 2026-09-27). */
        if (g_ed.path.find("/config/system.lua") != std::string::npos) {
            pda_config_reload();
        }
        ESP_LOGI(TAG, "salvo: %s (%u bytes)", g_ed.path.c_str(), (unsigned)g_ed.text.size());
        ed_push_ui(false);
        if (g_ed.path.rfind(notes_dir() + "/", 0) == 0) refresh_notes_list();
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "[erro] falha ao salvar %s", g_ed.path.c_str());
        log_line(buf, NULL);
    }
}

/* ---------------- teclado virtual: modelos de linhas -------------- */
/* [modo][shift][fileira] — modo 0 = abc, modo 1 = 123/símbolos.
 * As camadas de símbolos espelham Lower/Raise do teclado USB
 * (reference/.../keyboard/key_mapping.md). */
static const char *s_osk_rows[2][2][3] = {
    {   /* modo abc */
        { "q w e r t y u i o p", "a s d f g h j k l ç", "z x c v b n m , . ;" },
        { "Q W E R T Y U I O P", "A S D F G H J K L Ç", "Z X C V B N M , . ;" },
    },
    {   /* modo 123/símbolos */
        { "1 2 3 4 5 6 7 8 9 0", "! @ # $ % ^ & * ( )", "` - = [ ] \\ _ + { } |" },
        { "` ~ € £ ¥ ° ¶ • ª º", "< > ? / : ; \" ' ´ ¨", "+ - × ÷ = ≠ ≈ ∞ § ¤" },
    },
};

static void ed_push_osk_rows(void)
{
    const char **src = s_osk_rows[g_ed.osk_mode ? 1 : 0][g_ed.osk_shift ? 1 : 0];
    g_ui->set_ed_osk_mode(g_ed.osk_mode);
    for (int r = 0; r < 3; r++) {
        auto model = std::make_shared<slint::VectorModel<slint::SharedString>>();
        const char *p = src[r];
        std::string cur;
        /* separa por espaço; cada "tecla" pode ter 1 char ou multibyte (ç) */
        size_t i = 0;
        std::string row(p);
        cur.clear();
        while (i <= row.size()) {
            if (i == row.size() || row[i] == ' ') {
                if (!cur.empty()) { model->push_back(slint::SharedString(cur)); cur.clear(); }
                i++;
            } else {
                /* copia um codepoint UTF-8 inteiro */
                unsigned char c = (unsigned char)row[i];
                int extra = (c < 0x80) ? 0 : (c < 0xE0) ? 1 : (c < 0xF0) ? 2 : 3;
                cur.push_back(row[i]);
                for (int k = 0; k < extra && i + 1 < row.size(); k++) { i++; cur.push_back(row[i]); }
                i++;
            }
        }
        if (r == 0) g_ui->set_ed_osk_r1(model);
        else if (r == 1) g_ui->set_ed_osk_r2(model);
        else g_ui->set_ed_osk_r3(model);
    }
    g_ui->set_ed_osk_shift(g_ed.osk_shift);
}

static void ed_osk_key(const std::string k)
{
    if (k == "BACKSPACE") { ed_backspace(); return; }
    if (k == "DEL") { ed_delete_key(); return; }
    if (k == "ENTER") { ed_insert_str("\n", 1); return; }
    if (k == "SPACE") { ed_insert_str(" ", 1); return; }
    if (k == "LEFT")  { ed_move(-1, 0, false, false); return; }
    if (k == "RIGHT") { ed_move(1, 0, false, false); return; }
    if (k == "UP")    { ed_move(0, -1, false, false); return; }
    if (k == "DOWN")  { ed_move(0, 1, false, false); return; }
    if (k == "HOME")  { ed_move(0, 0, true, false); return; }
    if (k == "END")   { ed_move(0, 0, false, true); return; }
    if (k == "HIDE")  { g_ed.osk_override = 0; ed_push_ui(false); return; }
    if (k == "MODE")  { g_ed.osk_mode = !g_ed.osk_mode; ed_push_osk_rows(); return; }
    ed_insert_str(k.data(), k.size());
}

/* ================================================================== */
/* GERENCIADOR DE ARQUIVOS                                             */
/* ================================================================== */
/* Categoria de ícone (o codepoint vive estático em icons.slint). */
static const char *icon_kind_for(const char *name, bool isdir)
{
    if (isdir) return "dir";
    size_t n = strlen(name);
    struct { const char *ext; const char *kind; } map[] = {
        { ".lua", "lua" }, { ".sh", "sh" },
        { ".mp3", "audio" }, { ".wav", "audio" }, { ".flac", "audio" }, { ".ogg", "audio" },
        { ".png", "img" }, { ".jpg", "img" }, { ".jpeg", "img" },
        { ".gif", "img" }, { ".bmp", "img" },
        { ".txt", "txt" }, { ".md", "txt" }, { ".csv", "txt" },
        { ".log", "txt" }, { ".ini", "txt" }, { ".json", "txt" },
    };
    for (auto &m : map) {
        size_t e = strlen(m.ext);
        if (n > e && !strcasecmp(name + n - e, m.ext)) return m.kind;
    }
    return "file";
}

static bool is_textish(const char *name)
{
    const char *exts[] = { ".txt", ".lua", ".md", ".csv", ".log", ".ini", ".json" };
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t e = strlen(exts[i]);
        if (n > e && !strcasecmp(name + n - e, exts[i])) return true;
    }
    return false;
}

static void list_dir_async(const std::string dir)
{
    spawn_thread("io_list", 8192, [dir]() {
        auto names = std::make_shared<slint::VectorModel<slint::SharedString>>();
        auto isdir = std::make_shared<slint::VectorModel<bool>>();
        auto icons = std::make_shared<slint::VectorModel<slint::SharedString>>();

        /* O VFS do IDF não enumera "/" (opendir falha): sintetiza a lista
         * de mounts para a raiz virtual. */
        if (dir == "/") {
            if (storage_sd_mounted()) {
                names->push_back(slint::SharedString("sdcard"));
                isdir->push_back(true);
                icons->push_back(slint::SharedString(icon_kind_for("sdcard", true)));
            }
            names->push_back(slint::SharedString("internal"));
            isdir->push_back(true);
            icons->push_back(slint::SharedString(icon_kind_for("internal", true)));
            slint::invoke_from_event_loop([names, isdir, icons, dir]() {
                g_ui->set_file_entries(names);
                g_ui->set_file_is_dir(isdir);
                g_ui->set_file_kind(icons);
                g_ui->set_file_path(slint::SharedString(dir));
            });
            return;
        }

        DIR *d = opendir(dir.c_str());
        if (d) {
            std::vector<std::string> dirs, files;
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                std::string n(ent->d_name);
                if (n == "." || n == "..") continue;
                std::string full = dir + "/" + n;
                if (storage_is_dir(full.c_str())) dirs.push_back(n);
                else files.push_back(n);
            }
            closedir(d);
            for (auto &n : dirs) {
                names->push_back(slint::SharedString(n));
                isdir->push_back(true);
                icons->push_back(slint::SharedString(icon_kind_for(n.c_str(), true)));
            }
            for (auto &n : files) {
                names->push_back(slint::SharedString(n));
                isdir->push_back(false);
                icons->push_back(slint::SharedString(icon_kind_for(n.c_str(), false)));
            }
            if (dirs.empty() && files.empty()) {
                names->push_back(slint::SharedString("(vazio)"));
                isdir->push_back(false);
                icons->push_back(slint::SharedString(""));
            }
        } else {
            names->push_back(slint::SharedString("(não montado)"));
            isdir->push_back(false);
            icons->push_back(slint::SharedString(""));
        }

        slint::invoke_from_event_loop([names, isdir, icons, dir]() {
            g_ui->set_file_entries(names);
            g_ui->set_file_is_dir(isdir);
            g_ui->set_file_kind(icons);
            g_ui->set_file_path(slint::SharedString(dir));
        });
    });
}

struct FmState {
    bool sheet = false;
    std::string sheet_name;
    bool sheet_isdir = false;
    bool picker = false;
    std::string picker_op;      /* "copy" | "move" */
    std::string picker_src;
    std::string picker_label;
    bool prompt = false;
    std::string prompt_title;
    std::string prompt_text;
    size_t prompt_pos = 0;      /* cursor do prompt (offset de byte UTF-8) */
    std::string prompt_action;  /* rename | newdir | delete */
    bool prompt_needs_text = false;
};
static FmState g_fm;
static std::vector<std::string> g_dir_hist;   /* histórico p/ Voltar em Arquivos */
static std::vector<AppState> g_nav_hist;      /* pilha de telas p/ Voltar global */

static void nav_goto(AppState st)
{
    AppState cur = g_ui->get_active_app();
    if (cur != st) g_nav_hist.push_back(cur);
    g_ui->set_active_app(st);
}

static void nav_back(void)
{
    while (!g_nav_hist.empty()) {
        AppState prev = g_nav_hist.back();
        g_nav_hist.pop_back();
        g_ui->set_active_app(prev);
        if (prev == AppState::FileManager) { list_dir_async(g_fm_dir); return; }
        if (prev == AppState::NotesList) { refresh_notes_list(); return; }
        if (prev == AppState::Scripts) { refresh_scripts_list(); return; }
        if (prev != AppState::Editor) return;   /* Editor sem estado p/ restaurar: continua descendo */
    }
    g_ui->set_active_app(AppState::Launcher);
}

static void fm_enter_dir(const std::string &dir)
{
    g_dir_hist.push_back(g_fm_dir);
    g_fm_dir = dir;
    list_dir_async(g_fm_dir);
}

static void push_fm_ui(void)
{
    g_ui->set_fm_sheet(g_fm.sheet);
    g_ui->set_fm_sheet_name(slint::SharedString(g_fm.sheet_name));
    g_ui->set_fm_sheet_isdir(g_fm.sheet_isdir);
    g_ui->set_fm_picker(g_fm.picker);
    g_ui->set_fm_picker_label(slint::SharedString(g_fm.picker_label));
    g_ui->set_fm_prompt(g_fm.prompt);
    g_ui->set_fm_prompt_title(slint::SharedString(g_fm.prompt_title));
    /* campo com cursor: parte o texto no caret (o overlay desenha
     * pré | caret | sufixo — setas do OSK movem o caret, M4.8). */
    size_t pp = g_fm.prompt_pos;
    if (pp > g_fm.prompt_text.size()) pp = g_fm.prompt_text.size();
    g_ui->set_fm_prompt_pre(slint::SharedString(g_fm.prompt_text.substr(0, pp)));
    g_ui->set_fm_prompt_suf(slint::SharedString(g_fm.prompt_text.substr(pp)));
    g_ui->set_fm_prompt_needs_text(g_fm.prompt_needs_text);
}

/* Join de caminho que respeita a raiz "/" (evita "//sdcard"). */
static std::string path_join(const std::string &dir, const std::string &name)
{
    if (dir == "/") return "/" + name;
    return dir + "/" + name;
}

static std::string base_name(const std::string &p)
{
    size_t i = p.find_last_of('/');
    return (i == std::string::npos) ? p : p.substr(i + 1);
}

static void fm_open_entry(const std::string name)
{
    if (name == "(vazio)" || name == "(não montado)") return;
    std::string full = path_join(g_fm_dir, name);
    if (g_fm.picker) {
        if (storage_is_dir(full.c_str())) {
            fm_enter_dir(full);
        } else {
            log_line("[seletor] navegue até um diretório e toque em Selecionar", NULL);
        }
        return;
    }
    if (storage_is_dir(full.c_str())) {
        fm_enter_dir(full);
        return;
    }
    if (is_textish(name.c_str())) {
        bool md = name.size() > 3 && !strcasecmp(name.c_str() + name.size() - 3, ".md");
        ed_open_path_async(full, md);
        return;
    }
    char buf[256];
    int64_t sz = storage_file_size(full.c_str());
    snprintf(buf, sizeof(buf), "[info] %s: %ld bytes (visualização no M3)",
             name.c_str(), (long)sz);
    log_line(buf, NULL);
}

/* ================================================================== */
/* NOTAS                                                               */
/* ================================================================== */
static std::string generate_new_note_name(void)
{
    int max_n = 0;
    DIR *dir = opendir(notes_dir().c_str());
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            std::string name(ent->d_name);
            if (name.rfind("nota", 0) == 0) {
                size_t dot = name.find(".txt");
                if (dot != std::string::npos && dot > 4) {
                    int n = atoi(name.substr(4, dot - 4).c_str());
                    if (n > max_n) max_n = n;
                }
            }
        }
        closedir(dir);
    }
    return "nota" + std::to_string(max_n + 1);
}

/* Nomes REAIS dos arquivos de notas (o display stripa ".txt"). Preenchido
 * na thread de UI junto com o modelo; open_note usa para mapear
 * display -> arquivo (bug pré-M4.8: qualquer nota ganhava ".txt" extra e
 * ".md" nunca abria daqui). */
static std::vector<std::string> s_notes_real;

static void refresh_notes_list(void)
{
    spawn_thread("io_notes", 8192, []() {
        auto model = std::make_shared<slint::VectorModel<slint::SharedString>>();
        auto pairs = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
        DIR *dir = opendir(notes_dir().c_str());
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                std::string name(ent->d_name);
                if (name == "." || name == "..") continue;
                std::string disp = name;
                if (disp.size() > 4 && disp.compare(disp.size() - 4, 4, ".txt") == 0)
                    disp = disp.substr(0, disp.size() - 4);
                pairs->emplace_back(disp, name);
            }
            closedir(dir);
        }
        std::sort(pairs->begin(), pairs->end());
        for (auto &pr : *pairs) model->push_back(slint::SharedString(pr.first));
        if (pairs->empty())
            model->push_back(slint::SharedString("(nenhuma nota — toque em + Nova nota)"));
        slint::invoke_from_event_loop([model, pairs]() {
            s_notes_real.clear();
            for (auto &pr : *pairs) s_notes_real.push_back(pr.second);
            g_ui->set_notes_list(model);
        });
    });
}

/* ================================================================== */
/* SCRIPTS LUA                                                         */
/* ================================================================== */
static void refresh_scripts_list(void)
{
    spawn_thread("io_scripts", 8192, []() {
        auto model = std::make_shared<slint::VectorModel<slint::SharedString>>();
        char dir[160];
        pda_path(dir, sizeof(dir), "scripts");
        DIR *d = opendir(dir);
        int count = 0;
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                std::string n(ent->d_name);
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".lua") == 0) {
                    model->push_back(slint::SharedString(n));
                    count++;
                }
            }
            closedir(d);
        }
        if (count == 0)
            model->push_back(slint::SharedString("(sem .lua em scripts/)"));
        slint::invoke_from_event_loop([model]() { g_ui->set_scripts_list(model); });
    });
}

static void run_script_async(const std::string name)
{
    spawn_thread("lua_script", 32768, [name]() {
        if (!g_lua_mtx.try_lock()) {
            log_line("[aviso] já existe script rodando", NULL);
            return;
        }
        std::lock_guard<std::mutex> lk(g_lua_mtx, std::adopt_lock);
        char path[200];
        snprintf(path, sizeof(path), "%s/scripts/%s", pda_root(), name.c_str());
        char msg[256];
        snprintf(msg, sizeof(msg), ">>> executando %s", name.c_str());
        log_line(msg, NULL);
        esp_err_t err = lua_runtime_run_file(path, msg, sizeof(msg));
        if (err == ESP_OK) {
            snprintf(msg, sizeof(msg), "<<< %s ok", name.c_str());
        } else {
            snprintf(msg, sizeof(msg), "<<< %s falhou (ver console)", name.c_str());
        }
        log_line(msg, NULL);
    });
}

/* ================================================================== */
/* SESSÃO (hibernação)                                                 */
/* ================================================================== */
/* ================================================================== */
/* M5a — MÚSICA (WAV do cartão -> UAC USB)                             */
/* ================================================================== */
static std::vector<std::string> g_mus_names;
static int g_mus_cur = -1;

static void push_mus_ui(void)
{
    if (!g_ui) return;
    auto m = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (auto &n : g_mus_names) m->push_back(slint::SharedString(n));
    g_ui->set_mus_tracks(m);
    g_ui->set_mus_dev(slint::SharedString(
        audio_uac_present() ? audio_uac_dev_name() : "(sem device UAC)"));
    g_ui->set_mus_state(slint::SharedString(audio_uac_state()));
    g_ui->set_mus_track(slint::SharedString(audio_uac_track()));
}

static void refresh_music_list(void)
{
    spawn_thread("io_music_ls", 8192, []() {
        std::vector<std::string> out;
        std::string dir = std::string(pda_root()) + "/music";
        DIR *d = opendir(dir.c_str());
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                std::string n = e->d_name;
                if (n.size() > 4 && !strcasecmp(n.c_str() + n.size() - 4, ".wav"))
                    out.push_back(n);
            }
            closedir(d);
        }
        std::sort(out.begin(), out.end());
        slint::invoke_from_event_loop([out]() {
            g_mus_names = out;
            push_mus_ui();
        });
    });
}

static void music_play_idx(int i, bool fresh)
{
    if (i < 0 || (size_t)i >= g_mus_names.size()) return;
    g_mus_cur = i;
    std::string full = std::string(pda_root()) + "/music/" + g_mus_names[i];
    /* M5a.2.4: fresh = tap na lista/navegação (do zero); fresh=false =
     * play/pause do fone com player parado (retoma ponto de unplug). */
    spawn_thread("io_music", 8192, [full, fresh]() {
        if (fresh) audio_uac_play_fresh(full.c_str());
        else audio_uac_play(full.c_str());
    });
}

static void session_save(void *ctx)
{
    (void)ctx;
    char path[160];
    pda_path(path, sizeof(path), ".state/session.txt");
    char buf[320];
    snprintf(buf, sizeof(buf), "app=%s\nnote=%s\n",
             s_session_app.c_str(), s_session_note.c_str());
    storage_write_text_file(path, buf, strlen(buf));
    ESP_LOGI(TAG, "sessão salva: %s", buf);
}

static void session_restore_if_needed(void)
{
    if (!power_mgmt_woke_from_hibernate()) return;
    ESP_LOGI(TAG, "boot pós-hibernação detectado (flag NVS/reason)");
    char path[160];
    pda_path(path, sizeof(path), ".state/session.txt");
    char *data = NULL; size_t len = 0;
    if (storage_read_file_alloc(path, &data, &len) != ESP_OK) {
        ESP_LOGW(TAG, "pós-hibernação sem session.txt legível — nada a restaurar");
        return;
    }
    std::string s(data, len);
    free(data);

    std::string app, note;
    size_t p = s.find("app=");
    if (p != std::string::npos) app = s.substr(p + 4, s.find('\n', p) - p - 4);
    p = s.find("note=");
    if (p != std::string::npos) note = s.substr(p + 5, s.find('\n', p) - p - 5);

    /* M4.15.3: só PARSEIA aqui. Aplicar no bring-up crashava: o ramo
     * noteedit chama slint::invoke_from_event_loop ANTES da task ui_loop
     * existir → assert xTaskToNotify==NULL em ISR-context (log do
     * usuário 2026-10-05). A aplicação roda no event loop (abaixo). */
    ESP_LOGI(TAG, "sessão lida: app=%s note=%s (aplica no event loop)",
             app.c_str(), note.c_str());
    s_rest_app = app;
    s_rest_note = note;
    s_rest_path = path;
    s_rest_pending = true;
}

static void session_restore_apply(void)
{
    if (!s_rest_pending) return;
    s_rest_pending = false;
    ESP_LOGI(TAG, "restaurando sessão: app=%s note=%s",
             s_rest_app.c_str(), s_rest_note.c_str());
    if (s_rest_app == "noteedit" && !s_rest_note.empty()) {
        std::string full = std::string(pda_root()) + "/" + s_rest_note;
        ed_open_path_async(full, true);
    } else if (s_rest_app == "notes") {
        refresh_notes_list();
        g_ui->set_active_app(AppState::NotesList);
    } else if (s_rest_app == "files") {
        g_fm_dir = pda_root();
        list_dir_async(g_fm_dir);
        g_ui->set_active_app(AppState::FileManager);
    } else if (s_rest_app == "scripts") {
        refresh_scripts_list();
        g_ui->set_active_app(AppState::Scripts);
    } else if (s_rest_app == "settings") {
        g_ui->set_active_app(AppState::Settings);   /* ramo novo (M4.15.3) */
    } else if (s_rest_app == "music") {
        refresh_music_list();
        g_ui->set_active_app(AppState::Music);
    }
    storage_delete_file(s_rest_path.c_str());
}

/* ================================================================== */
/* CONFIG UI                                                           */
/* ================================================================== */
static void push_settings_to_ui(void)
{
    const pda_settings_t *s = pda_settings();
    g_ui->set_cfg_brightness((float)s->brightness);
    g_ui->set_cfg_dim_s((float)s->dim_after_s);
    g_ui->set_cfg_off_s((float)s->screen_off_after_s);
    g_ui->set_cfg_deep_s((float)s->deep_sleep_after_s);
    g_ui->set_cfg_wake_touch(s->wake_on_touch);
    g_ui->set_cfg_wifi_on(s->wifi_enabled);
    g_ui->set_cfg_accent(slint::SharedString(s->accent));
    g_ui->set_cfg_boot_standby(s->boot_btn_standby);
    g_ui->set_cfg_osk_auto(s->onscreen_keyboard_auto);
    g_ui->set_cfg_light_sleep(s->light_sleep);

    char buf[96];
    storage_sd_describe(buf, sizeof(buf));
    g_ui->set_cfg_store_info(slint::SharedString(buf));
    g_ui->set_status_store(slint::SharedString(buf));
    g_ui->set_status_store_icon(slint::SharedString(storage_sd_mounted() ? "sd" : "int"));
    g_ui->set_cfg_batt_info(slint::SharedString("nao medivel (IP5306)"));
    g_ui->set_cfg_wifi_info(slint::SharedString(
        wifi_net_connected() ? wifi_net_ssid() : "offline"));
    g_ui->set_status_batt(slint::SharedString("--"));
}

/* ================================================================== */
/* Heap-allocated de propósito: ComponentHandle não tem ctor default, e o
 * handle precisa viver para sempre (dono da referência do component). */
static slint::ComponentHandle<AppWindow> *s_ui_run_handle = nullptr;
static esp_lcd_touch_handle_t s_touch_handle = nullptr;   /* M4.15: GT911 sleep no standby */

static void ui_run_task(void *arg)
{
    (void)arg;
    (*s_ui_run_handle)->run();   /* operator-> devolve AppWindow*, que tem run() */
    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== PDA M2 — bring-up ===");

    /* NVS antes de tudo: a sombra de config (pda_config) precisa dele já
     * no boot-heal, antes da task de Wi-Fi subir. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    wifi_net_seed_clock();   /* M5.0b (A3): relógio plausível sem rede */
    board_storage_init();
    pda_config_init();
    lua_runtime_init();
    lua_runtime_set_log_callback(log_line, NULL);
    lua_runtime_set_toast_callback(toast, NULL);
    lua_runtime_set_settings_changed_cb(settings_changed_by_lua);

    esp_lcd_panel_handle_t panel = nullptr;
    ESP_ERROR_CHECK(board_display_init(&panel));
    board_display_backlight_set((uint8_t)pda_settings()->brightness);

    esp_lcd_touch_handle_t touch = nullptr;
    ESP_ERROR_CHECK(board_touch_init(&touch));
    s_touch_handle = touch;

    static std::vector<slint::platform::Rgb565Pixel> framebuffer(
        BOARD_LCD_H_RES_NATIVE * BOARD_LCD_V_RES_NATIVE);

    slint_esp_init(SlintPlatformConfiguration<slint::platform::Rgb565Pixel>{
        .size = slint::PhysicalSize({BOARD_LCD_V_RES_NATIVE, BOARD_LCD_H_RES_NATIVE}),
        .panel_handle = panel,
        .touch_handle = touch,
        .buffer1 = framebuffer,
        .rotation = slint::platform::SoftwareRenderer::RenderingRotation::Rotate90,
        .byte_swap = false,
    });

    auto ui = AppWindow::create();
    g_ui = ui.operator->();
    g_script_log = std::make_shared<slint::VectorModel<slint::SharedString>>();
    ui->set_script_log(g_script_log);
    g_fm_dir = pda_root();

    push_settings_to_ui();
    ed_push_osk_rows();

    /* ---------- status bar ---------- */
    ui->on_tick([]() { update_clock(); });

    /* ---------- cursor piscante (~530 ms) ----------
     * Callback do esp_timer roda em task de sistema: só agenda o toggle no
     * loop de eventos do Slint. Alterna apenas com o Editor ativo e fora do
     * modo leitura; a fase é rearmada por cursor_rearm() (via ed_push_ui). */
    {
        esp_timer_create_args_t bargs = {};
        bargs.callback = [](void *arg) {
            (void)arg;
            slint::invoke_from_event_loop([]() {
                if (g_ui && g_ui->get_active_app() == AppState::Editor &&
                    !g_ed.reading) {
                    s_cursor_on = !s_cursor_on;
                    ed_push_ui(false);
                }
            });
        };
        bargs.arg = nullptr;
        bargs.name = "cursor_blink";
        if (esp_timer_create(&bargs, &s_blink_t) == ESP_OK) {
            esp_timer_start_periodic(s_blink_t, 530 * 1000);
        } else {
            s_blink_t = nullptr;
        }
    }

    /* ---------- launcher ---------- */
    ui->on_open_app_dispatch([](slint::SharedString name) {
        activity();
        std::string n(name.data());
        if (n == "Arquivos") {
            g_fm_dir = pda_root();
            g_dir_hist.clear();
            list_dir_async(g_fm_dir);
            nav_goto(AppState::FileManager);
            s_session_app = "files";
        } else if (n == "Notas") {
            refresh_notes_list();
            nav_goto(AppState::NotesList);
            s_session_app = "notes";
        } else if (n == "Scripts") {
            refresh_scripts_list();
            nav_goto(AppState::Scripts);
            s_session_app = "scripts";
        } else if (n == "Musica") {
            refresh_music_list();
            nav_goto(AppState::Music);
            s_session_app = "music";
        } else if (n == "Redes") {
            nav_goto(AppState::Networks);
            net_start_scan();
            s_session_app = "launcher";
        } else if (n == "Config") {
            push_settings_to_ui();
            nav_goto(AppState::Settings);
            s_session_app = "settings";
        }
    });

    /* ---------- arquivos ---------- */
    ui->on_request_dir([](slint::SharedString arg) {
        activity();
        std::string a(arg.data());
        if (!a.empty() && a[0] == '/') {
            g_fm_dir = a;
            list_dir_async(g_fm_dir);
        } else {
            fm_open_entry(a);
        }
    });
    ui->on_dir_up([]() {
        activity();
        ESP_LOGI(TAG, "fm: Sobe (prompt=%d sheet=%d picker=%d dir=%s)",
                 (int)g_fm.prompt, (int)g_fm.sheet, (int)g_fm.picker, g_fm_dir.c_str());
        if (g_fm.prompt || g_fm.sheet) {
            g_fm.prompt = g_fm.sheet = false;
            push_fm_ui();
            return;
        }
        /* Sobe até "/" (onde o VFS lista os mounts /sdcard e /internal).
         * Sem clamp em pda_root: era isso que prendia o usuário em
         * /sdcard/pda (reporte 2026-09-28; o replace anterior não casou). */
        std::string d = g_fm_dir;
        if (d == "/") return;
        size_t slash = d.find_last_of('/');
        std::string parent = (slash == std::string::npos || slash == 0)
                                 ? std::string("/")
                                 : d.substr(0, slash);
        g_fm_dir = parent;
        g_dir_hist.push_back(d);
        ESP_LOGI(TAG, "fm: subindo p/ %s", g_fm_dir.c_str());
        list_dir_async(g_fm_dir);
    });

    /* ---------- operações de arquivo (M3b) ---------- */
    ui->on_fm_more([](slint::SharedString name, bool isdir) {
        activity();
        g_fm.sheet = true;
        g_fm.sheet_name = std::string(name.data());
        g_fm.sheet_isdir = isdir;
        push_fm_ui();
    });
    ui->on_fm_sheet_action([](slint::SharedString a) {
        activity();
        std::string act(a.data());
        if (act == "cancelar") { g_fm.sheet = false; push_fm_ui(); return; }
        if (act == "abrir") {
            g_fm.sheet = false;
            push_fm_ui();
            fm_open_entry(g_fm.sheet_name);
            return;
        }
        if (act == "renomear") {
            g_fm.sheet = false;
            g_fm.prompt = true;
            g_fm.prompt_needs_text = true;
            g_fm.prompt_title = "Renomear \"" + g_fm.sheet_name + "\" para:";
            g_fm.prompt_text = g_fm.sheet_name;
            g_fm.prompt_pos = g_fm.prompt_text.size();
            g_fm.prompt_action = "rename";
            push_fm_ui();
            return;
        }
        if (act == "copiar" || act == "mover") {
            g_fm.sheet = false;
            g_fm.picker = true;
            g_fm.picker_op = (act == "copiar") ? "copy" : "move";
            g_fm.picker_src = path_join(g_fm_dir, g_fm.sheet_name);
            g_fm.picker_label = (act == "copiar" ? "Copiar \"" : "Mover \"")
                              + g_fm.sheet_name + "\" para:";
            push_fm_ui();
            return;
        }
        if (act == "apagar") {
            g_fm.sheet = false;
            g_fm.prompt = true;
            g_fm.prompt_needs_text = false;
            g_fm.prompt_title = "Apagar \"" + g_fm.sheet_name + "\"? Não dá para desfazer.";
            g_fm.prompt_action = "delete";
            push_fm_ui();
            return;
        }
    });
    ui->on_fm_newfile([]() {
        activity();
        g_fm.prompt = true;
        g_fm.prompt_needs_text = true;
        g_fm.prompt_title = "Nome do novo arquivo (ex.: nota.txt):";
        g_fm.prompt_text = "";
        g_fm.prompt_pos = g_fm.prompt_text.size();
        g_fm.prompt_action = "newfile";
        push_fm_ui();
    });
    ui->on_fm_newdir([]() {
        activity();
        g_fm.prompt = true;
        g_fm.prompt_needs_text = true;
        g_fm.prompt_title = "Nome do novo diretório:";
        g_fm.prompt_text = "";
        g_fm.prompt_pos = g_fm.prompt_text.size();
        g_fm.prompt_action = "newdir";
        push_fm_ui();
    });
    ui->on_fm_picker_select([]() {
        activity();
        if (!g_fm.picker) return;
        std::string dest = path_join(g_fm_dir, base_name(g_fm.picker_src));
        char msg[320];
        esp_err_t err = (g_fm.picker_op == "copy")
            ? storage_copy_file(g_fm.picker_src.c_str(), dest.c_str())
            : storage_move_file(g_fm.picker_src.c_str(), dest.c_str());
        if (err == ESP_OK) {
            snprintf(msg, sizeof(msg), "[ok] %s -> %s",
                     g_fm.picker_op.c_str(), dest.c_str());
        } else {
            snprintf(msg, sizeof(msg), "[erro] %s falhou (%s) — destino existe?",
                     g_fm.picker_op.c_str(), esp_err_to_name(err));
        }
        log_line(msg, NULL);
        g_fm.picker = false;
        push_fm_ui();
        list_dir_async(g_fm_dir);
    });
    ui->on_fm_prompt_ok([]() {
        activity();
        if (!g_fm.prompt) return;
        std::string target = path_join(g_fm_dir, g_fm.sheet_name);
        char msg[320];
        if (g_fm.prompt_action == "rename") {
            std::string nt = g_fm.prompt_text;
            if (nt.empty() || nt == "." || nt == ".." || nt.find('/') != std::string::npos) {
                log_line("[erro] nome inválido", NULL);
            } else {
                std::string dest = path_join(g_fm_dir, nt);
                esp_err_t err = storage_move_file(target.c_str(), dest.c_str());
                snprintf(msg, sizeof(msg), err == ESP_OK ? "[ok] renomeado p/ %s" : "[erro] renomear (%s)",
                         err == ESP_OK ? nt.c_str() : esp_err_to_name(err));
                log_line(msg, NULL);
            }
        } else if (g_fm.prompt_action == "newdir") {
            std::string nt = g_fm.prompt_text;
            if (nt.empty() || nt.find('/') != std::string::npos) {
                log_line("[erro] nome inválido", NULL);
            } else {
                storage_ensure_dir((path_join(g_fm_dir, nt)).c_str());
                log_line("[ok] diretório criado", NULL);
            }
        } else if (g_fm.prompt_action == "overwrite") {
            /* segundo passo: confirmação de substituição */
            std::string path = path_join(g_fm_dir, g_fm.prompt_text);
            if (storage_write_text_file(path.c_str(), "", 0) == ESP_OK) {
                log_line("[ok] arquivo substituído", NULL);
                g_fm.prompt = false;
                push_fm_ui();
                ed_open_path_async(path, false);
                return;
            }
            log_line("[erro] falha ao substituir", NULL);
        } else if (g_fm.prompt_action == "newfile") {
            std::string nt = g_fm.prompt_text;
            if (nt.empty() || nt.find('/') != std::string::npos) {
                log_line("[erro] nome inválido", NULL);
            } else {
                std::string path = path_join(g_fm_dir, nt);
                if (storage_file_exists(path.c_str())) {
                    /* confirma antes de sobrescrever */
                    g_fm.prompt_title = "Já existe \"" + nt + "\". Substituir?";
                    g_fm.prompt_text = nt;
                    g_fm.prompt_pos = g_fm.prompt_text.size();
                    g_fm.prompt_needs_text = false;
                    g_fm.prompt_action = "overwrite";
                    push_fm_ui();
                    return;
                } else if (storage_write_text_file(path.c_str(), "", 0) == ESP_OK) {
                    log_line("[ok] arquivo criado", NULL);
                    g_fm.prompt = false;
                    push_fm_ui();
                    ed_open_path_async(path, false);   /* já abre p/ editar */
                    return;
                }
            }
        } else if (g_fm.prompt_action == "wifipass") {
            char msg[200];
            snprintf(msg, sizeof(msg), "[wifi] conectando em \"%s\"", g_wifi_pick_ssid.c_str());
            log_line(msg, NULL);
            g_ui->set_cfg_wifi_on(true);   /* M5.3.2: switch reflete o rádio */
            wifi_net_connect(g_wifi_pick_ssid.c_str(), g_fm.prompt_text.c_str(), true);
        } else if (g_fm.prompt_action == "delete") {
            esp_err_t err = storage_rm_rf(target.c_str());
            snprintf(msg, sizeof(msg), err == ESP_OK ? "[ok] apagado: %s" : "[erro] apagar (%s)",
                     err == ESP_OK ? g_fm.sheet_name.c_str() : esp_err_to_name(err));
            log_line(msg, NULL);
        }
        g_fm.prompt = false;
        push_fm_ui();
        list_dir_async(g_fm_dir);
    });
    ui->on_fm_prompt_cancel([]() {
        activity();
        g_fm.prompt = false;
        push_fm_ui();
    });
    ui->on_net_rescan([]() {
        activity();
        net_start_scan();
    });
    ui->on_net_pick([](int i) {
        activity();
        wifi_net_set_autoreconnect(true);
        if (i < 0 || (size_t)i >= g_aps.size()) return;
        const wifi_net_ap_t &ap = g_aps[i];
        if (ap.open) {
            char msg[160];
            snprintf(msg, sizeof(msg), "[wifi] conectando em \"%s\" (aberta)", ap.ssid);
            log_line(msg, NULL);
            g_ui->set_cfg_wifi_on(true);   /* M5.3.2: switch reflete o rádio */
            wifi_net_connect(ap.ssid, "", true);
        } else if (const char *sp = wifi_net_saved_pass(ap.ssid)) {
            /* M5.2: rede já salva conecta direto, sem prompt. */
            char msg[160];
            snprintf(msg, sizeof(msg), "[wifi] conectando em \"%s\" (senha salva)", ap.ssid);
            log_line(msg, NULL);
            g_ui->set_cfg_wifi_on(true);   /* M5.3.2: switch reflete o rádio */
            wifi_net_connect(ap.ssid, sp, false);
        } else {
            g_wifi_pick_ssid = ap.ssid;
            g_fm.prompt = true;
            g_fm.prompt_needs_text = true;
            g_fm.prompt_title = "Senha para \"" + g_wifi_pick_ssid + "\":";
            g_fm.prompt_text = "";
            g_fm.prompt_pos = g_fm.prompt_text.size();
            g_fm.prompt_action = "wifipass";
            push_fm_ui();
        }
    });
    ui->on_mus_play([](int i) { activity(); music_play_idx(i, true); push_mus_ui(); });
    ui->on_mus_stop([]() { activity(); audio_uac_stop(); push_mus_ui(); });
    ui->on_mus_next([]() {
        activity();
        if (!g_mus_names.empty())
            music_play_idx((g_mus_cur + 1) % (int)g_mus_names.size(), true);
        push_mus_ui();
    });
    ui->on_mus_refresh([]() { activity(); refresh_music_list(); });
    ui->on_mus_pause([]() {
        activity();
        if (audio_uac_present()) {
            if (audio_uac_paused()) audio_uac_resume(); else audio_uac_pause();
        }
        push_mus_ui();
    });
    /* M5a.2: media keys do consumer control (H3S/teclados c/ mídia). */
    usb_hid_keyboard_set_media_cb([](int act) {
        slint::invoke_from_event_loop([act]() {
            switch (act) {
            case USB_MEDIA_PLAYPAUSE:
                if (audio_uac_playing()) {
                    if (audio_uac_paused()) audio_uac_resume(); else audio_uac_pause();
                } else if (!g_mus_names.empty()) {
                    music_play_idx(g_mus_cur >= 0 ? g_mus_cur : 0, false);
                }
                break;
            case USB_MEDIA_NEXT:
                if (!g_mus_names.empty())
                    music_play_idx((g_mus_cur + 1) % (int)g_mus_names.size(), true);
                break;
            case USB_MEDIA_PREV:
                if (!g_mus_names.empty())
                    music_play_idx((g_mus_cur - 1 + (int)g_mus_names.size()) %
                                   (int)g_mus_names.size(), true);
                break;
            case USB_MEDIA_STOP:
                audio_uac_stop();
                break;
            case USB_MEDIA_VOL_UP:
                audio_uac_volume_step(+10);
                break;
            case USB_MEDIA_VOL_DOWN:
                audio_uac_volume_step(-10);
                break;
            case USB_MEDIA_MUTE:
                audio_uac_mute_toggle();
                break;
            default:
                log_line("[audio] mídia: usage desconhecido", NULL);
                break;
            }
            push_mus_ui();
        });
    });
    ui->on_app_back([]() {
        activity();
        /* saindo da tela Redes sem escolher: retoma reconexão da rede salva */
        if (g_ui->get_active_app() == AppState::Networks) {
            wifi_net_set_autoreconnect(true);
        }
        nav_back();
    });
    ui->on_fm_back([]() {
        activity();
        if (g_fm.prompt) { g_fm.prompt = false; push_fm_ui(); return; }
        if (g_fm.sheet) { g_fm.sheet = false; push_fm_ui(); return; }
        if (g_fm.picker) { g_fm.picker = false; push_fm_ui(); return; }
        if (!g_dir_hist.empty()) {
            g_fm_dir = g_dir_hist.back();
            g_dir_hist.pop_back();
            list_dir_async(g_fm_dir);
            return;
        }
        nav_back();
    });

    /* ---------- notas ---------- */
    ui->on_request_notes_list([]() { activity(); refresh_notes_list(); });
    ui->on_open_note([](slint::SharedString name) {
        activity();
        std::string n(name.data());
        if (n.rfind("(nenhuma", 0) == 0) return;
        /* display -> nome real (a lista stripa ".txt"; ".md"/".lua"/etc.
         * aparecem com o nome cheio). */
        std::string real;
        for (const auto &r : s_notes_real) {
            std::string d = r;
            if (d.size() > 4 && d.compare(d.size() - 4, 4, ".txt") == 0)
                d = d.substr(0, d.size() - 4);
            if (d == n) { real = r; break; }
        }
        if (real.empty()) real = n + ".txt";   /* fallback: nota antiga */
        if (!is_textish(real.c_str())) {
            ESP_LOGW(TAG, "notas: formato não suportado, ignorando %s", real.c_str());
            return;
        }
        size_t nl = real.size();
        bool md = nl > 3 && strcasecmp(real.c_str() + nl - 3, ".md") == 0;
        ed_open_path_async(path_join(notes_dir(), real), md);
    });
    ui->on_new_note([]() {
        activity();
        std::string name = generate_new_note_name();
        std::string path = notes_dir() + "/" + name + ".txt";
        ed_load(path, "", true, false);
    });

    /* ---------- editor ---------- */
    ui->on_ed_drag([](float dx, float dy) {
        activity();
        s_ed_left -= dx;
        s_ed_top -= dy;
        ed_refresh_view();
    });
    ui->on_ed_tap_at([](float x, float y) {
        activity();
        if (g_ed.reading) return;   /* leitura: toque só rola */
        /* fonte mono: linha e coluna direto do ponto tocado */
        int line = (int)((s_ed_top + y) / ED_LINE_H);
        int col = (int)((s_ed_left + x - 6) / ED_CHAR_W);
        if (line < 0) line = 0;
        if (col < 0) col = 0;
        size_t line_start = 0;
        int idx = 0;
        for (size_t i = 0; i <= g_ed.text.size(); i++) {
            if (i == g_ed.text.size() || g_ed.text[i] == '\n') {
                if (idx == line) {
                    size_t len = i - line_start;
                    size_t c = (size_t)col > len ? len : (size_t)col;
                    g_ed.cursor = utf8_snap(g_ed.text, line_start + c);
                    break;
                }
                idx++;
                line_start = i + 1;
            }
        }
        ed_push_ui(false);
    });
    ui->on_ed_osk_key([](slint::SharedString k) {
        activity();
        std::string key(k.data());
        if (g_ed.reading) return;   /* leitura: teclado não edita */
        if (g_fm.prompt && g_fm.prompt_needs_text) {
            /* edição do campo com cursor real (M4.8): antes as teclas
             * simbólicas caíam no else e eram anexadas LITERAIS
             * ("SPACE" no lugar de espaço) e as setas não faziam nada. */
            std::string &t = g_fm.prompt_text;
            if (g_fm.prompt_pos > t.size()) g_fm.prompt_pos = t.size();
            size_t &p = g_fm.prompt_pos;
            if (key == "BACKSPACE") {
                if (p > 0) { size_t q = utf8_prev(t, p); t.erase(q, p - q); p = q; }
            } else if (key == "DEL") {
                if (p < t.size()) { size_t q = utf8_next(t, p); t.erase(p, q - p); }
            } else if (key == "LEFT") {
                p = utf8_prev(t, p);
            } else if (key == "RIGHT") {
                p = utf8_next(t, p);
            } else if (key == "HOME") {
                p = 0;
            } else if (key == "END") {
                p = t.size();
            } else if (key == "ENTER") {
                g_ui->invoke_fm_prompt_ok();
                return;
            } else if (key == "MODE") {
                g_ed.osk_mode = !g_ed.osk_mode;
                ed_push_osk_rows();
                return;
            } else if (key == "HIDE" || key == "UP" || key == "DOWN") {
                return;
            } else if (key == "SPACE") {
                t.insert(p, " ");
                p += 1;
            } else {
                t.insert(p, key);
                p += key.size();
            }
            push_fm_ui();
            return;
        }
        ed_osk_key(key);
    });
    ui->on_ed_osk_shift_set([](bool s) {
        activity();
        g_ed.osk_shift = s;
        ed_push_osk_rows();
    });
    ui->on_ed_toggle_mode([]() {
        activity();
        g_ed.reading = !g_ed.reading;
        ed_push_ui(true);   /* volta p/ edição: rearma o piscar e mostra o cursor */
    });
    ui->on_ed_toggle_osk([]() {
        activity();
        g_ed.osk_override = ed_osk_should_show() ? 0 : 1;
        ed_push_ui(false);
    });
    ui->on_ed_save([]() { activity(); ed_save(); });
    ui->on_ed_delete([]() {
        activity();
        if (g_ed.path.rfind(notes_dir() + "/", 0) != 0) return;  /* segurança */
        storage_delete_file(g_ed.path.c_str());
        g_ed.path.clear();
        g_ui->set_active_app(AppState::NotesList);
        refresh_notes_list();
    });

    /* ---------- scripts ---------- */
    ui->on_request_scripts([]() { activity(); refresh_scripts_list(); });
    ui->on_run_script([](slint::SharedString name) {
        activity();
        std::string n(name.data());
        if (n.rfind("(sem", 0) == 0) return;
        run_script_async(n);
    });
    ui->on_clear_script_log([]() {
        activity();
        if (g_script_log) g_script_log->clear();
    });

    /* ---------- config ---------- */
    ui->on_cfg_save([]() {
        activity();
        pda_settings_t s = *pda_settings();
        s.brightness = (int)g_ui->get_cfg_brightness();
        s.dim_after_s = (int)g_ui->get_cfg_dim_s();
        s.screen_off_after_s = (int)g_ui->get_cfg_off_s();
        s.deep_sleep_after_s = (int)g_ui->get_cfg_deep_s();
        s.wake_on_touch = g_ui->get_cfg_wake_touch();
        s.boot_btn_standby = g_ui->get_cfg_boot_standby();
        s.onscreen_keyboard_auto = g_ui->get_cfg_osk_auto();
        s.light_sleep = g_ui->get_cfg_light_sleep();
        ESP_LOGI(TAG, "cfg-save UI: br=%f dim=%f off=%f deep=%f",
                 (double)g_ui->get_cfg_brightness(), (double)g_ui->get_cfg_dim_s(),
                 (double)g_ui->get_cfg_off_s(), (double)g_ui->get_cfg_deep_s());
        pda_settings_update(&s);
        pda_config_save();
        /* M5.0b (A2): clamp_all pode ajustar (deep >= off+10 etc.);
         * devolve os valores REAIS aos sliders p/ UI não mentir. */
        {
            const pda_settings_t *cs = pda_settings();
            g_ui->set_cfg_brightness((float)cs->brightness);
            g_ui->set_cfg_dim_s((float)cs->dim_after_s);
            g_ui->set_cfg_off_s((float)cs->screen_off_after_s);
            g_ui->set_cfg_deep_s((float)cs->deep_sleep_after_s);
        }
        s_qs_dirty = false;   /* o save do painel de Config já persistiu o brilho */
        apply_brightness_from_settings();
        ed_push_ui(false);   /* osk_auto pode ter mudado */
        log_line("[config] salva em system.lua", NULL);
    });
    ui->on_cfg_reset([]() {
        activity();
        pda_config_reset_defaults();
        push_settings_to_ui();
    });
    ui->on_cfg_sleep_now([]() {
        activity();
        power_mgmt_request_standby();
    });
    ui->on_cfg_set_cursor([](slint::SharedString st) {
        activity();
        pda_settings_set("ui.cursor", 0, false, false, std::string(st.data()).c_str());
        /* M4.11b: persiste NA HORA. Antes só ia para a RAM (s_dirty) e
         * dependia do próximo "Salvar"; se o boot seguinte caísse na cura
         * espelho/NVS, o cursor voltava ao default. */
        pda_config_save();
        s_qs_dirty = false;   /* save completo: brilho pendente do pulldown foi junto */
        ESP_LOGI(TAG, "[config] cursor salvo: %s", pda_settings()->cursor_style);
        slint::invoke_from_event_loop([]() {
            if (g_ui->get_active_app() == AppState::Editor) ed_push_ui(false);
        });
    });
    /* M5a.1 (v5.4.3): o boot-wire do UAC tinha sido perdido por um
     * replace sem assert numa rodada anterior (âncora errada: a chamada
     * do HID leva lambdas). Sem isto o driver NUNCA instalava — log da
     * 13ª rodada não tinha uma linha de audio_uac. */
    audio_uac_set_event_cb([](void *) {
        slint::invoke_from_event_loop([] { push_mus_ui(); });
    }, NULL);
    audio_uac_init();
    ui->on_cfg_set_accent([ui](slint::SharedString a) {
        activity();
        pda_settings_set("ui.accent", 0, false, false, std::string(a.data()).c_str());
        pda_config_save();
        ui->set_cfg_accent(slint::SharedString(pda_settings()->accent));
        ESP_LOGI(TAG, "[config] acento salvo: %s", pda_settings()->accent);
    });
    ui->on_cfg_set_wifi_on([](bool on) {
        activity();
        pda_settings_set("net.wifi_enabled", 0, true, on, NULL);
        pda_config_save();
        wifi_net_set_enabled(on);
        ESP_LOGI(TAG, "[config] wifi %s pela chave", on ? "ligado" : "desligado");
    });
    ui->on_cfg_open_networks([]() {
        activity();
        nav_goto(AppState::Networks);
        net_start_scan();
    });
    ui->on_cfg_hibernate([]() {
        power_mgmt_hibernate();
    });

    /* ---------- quick settings pulldown (M4.12) ---------- */
    ui->on_qs_brightness([](float v) {
        activity();
        pda_settings_t s = *pda_settings();
        if (s.brightness == (int)v) return;
        s.brightness = (int)v;
        pda_settings_update(&s);          /* RAM apenas — I/O só no fechamento */
        apply_brightness_from_settings(); /* backlight ao vivo */
        s_qs_dirty = true;
    });
    ui->on_qs_state([](bool open) {
        activity();
        if (!open && s_qs_dirty) {
            s_qs_dirty = false;
            pda_config_save();
            ESP_LOGI(TAG, "[quick] brilho persistido: %d", pda_settings()->brightness);
        }
    });

    /* ---------- teclado USB ---------- */
    usb_hid_keyboard_init([](uint8_t ascii, uint8_t keycode, uint8_t /*mod*/) {
        slint::invoke_from_event_loop([ascii, keycode]() {
            activity();
            if (g_ui->get_active_app() != AppState::Editor) return;
            if (g_ed.reading) return;
            switch (keycode) {
            case 0x4F: ed_move(1, 0, false, false); return;    // Right
            case 0x50: ed_move(-1, 0, false, false); return;   // Left
            case 0x51: ed_move(0, 1, false, false); return;    // Down
            case 0x52: ed_move(0, -1, false, false); return;   // Up
            case 0x4A: ed_move(0, 0, true, false); return;     // Home
            case 0x4D: ed_move(0, 0, false, true); return;     // End
            case 0x4C: ed_delete_key(); return;                // Delete
            default: break;
            }
            if (ascii == 0) return;
            if (ascii == '\b') ed_backspace();
            else if (ascii == '\t') ed_insert_str("  ", 2);
            else ed_insert_str((const char *)&ascii, 1);
        });
    },
    [](bool connected) {
        ESP_LOGI(TAG, "teclado USB: %s", connected ? "conectado" : "ausente");
        slint::invoke_from_event_loop([connected]() {
            activity();
            g_hid_seen = g_hid_seen || connected;
            g_ui->set_has_keyboard(connected);
            if (g_ui->get_active_app() == AppState::Editor) ed_push_ui(false);
        });
    });

    /* ---------- power ---------- */
    power_mgmt_set_standby_cb([](bool entering, void *) {
        /* Teardown/re-init de periféricos SOMENTE no modo light sleep
         * (hoje atrás do kill-switch). No standby robusto a CPU fica idle
         * e SD/USB/Wi-Fi permanecem vivos — remontar aqui era o que
         * derrubava USB e SD sem necessidade (log de 2026-09-25). */
        const bool ls = power_mgmt_light_sleep_active();
        if (entering) {
            wifi_net_pause();   /* M4.14: standby não reconecta nem polla NTP */
            /* M4.15: sem wake por toque armado, o GT911 dorme no standby
             * (~3,5 mA economizados); o proxy do touch já cega read/get_xy
             * então não há I2C nem pontos fantasmas. Com a chave ON o chip
             * fica acordado (é ele que dá o wake). */
            if (s_touch_handle && !power_mgmt_touch_wake_armed()) {
                if (esp_lcd_touch_enter_sleep(s_touch_handle) == ESP_OK) {
                    ESP_LOGI(TAG, "GT911 em sleep (standby sem wake por toque)");
                }
            }
            if (ls) usb_hid_keyboard_prepare_sleep();
            return;
        }
        if (s_touch_handle) esp_lcd_touch_exit_sleep(s_touch_handle);
        wifi_net_resume();      /* M4.14: reconecta/NTP volta ao acordar */
        if (ls) {
            storage_remount_sd();
            usb_hid_keyboard_resume();
        }
        slint::invoke_from_event_loop([]() {
            update_clock();
            char buf[96];
            storage_sd_describe(buf, sizeof(buf));
            g_ui->set_status_store(slint::SharedString(buf));
            g_ui->set_cfg_store_info(slint::SharedString(buf));
            g_ui->set_status_store_icon(slint::SharedString(storage_sd_mounted() ? "sd" : "int"));
            g_ui->set_has_keyboard(usb_hid_keyboard_connected());
        });
    }, NULL);
    power_mgmt_set_hibernate_save_cb(session_save, NULL);
    power_mgmt_init();

    /* Wi-Fi/NTP (M4): sem config/wifi.lua retorna NOT_FOUND e segue offline */
    wifi_net_set_enabled_boot(pda_settings()->wifi_enabled);   /* M5.3 */
    if (wifi_net_init() == ESP_OK) {
        g_ui->set_status_wifi(slint::SharedString("wifi ..."));
    } else {
        g_ui->set_status_wifi(slint::SharedString("wifi off"));
    }

    session_restore_if_needed();
    update_clock();

    /* O event loop do Slint roda em task própria com pilha em PSRAM:
     * o layout precisa de ~32 KiB de pilha e a RAM interna ficou curta
     * depois que o ESP-Hosted passou a alocar buffers SDIO/DMA antes da
     * task main (assert em app_startup.c:86 no boot de 2026-09-24). */
    s_ui_run_handle = new slint::ComponentHandle<AppWindow>(ui);
    BaseType_t ok = xTaskCreateWithCaps(ui_run_task, "ui_loop", 32768, NULL, 5,
                                        NULL, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "pilha PSRAM p/ UI falhou — fallback interno");
        ok = xTaskCreate(ui_run_task, "ui_loop", 32768, NULL, 5, NULL);
    }
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_FAIL);
    /* M4.15.3: a task ui_loop existe (handle válido p/ notify); o lambda
     * roda quando o event loop começar — seguro p/ invoke/ed_load. */
    if (s_rest_pending) {
        slint::invoke_from_event_loop([]() { session_restore_apply(); });
    }
    ESP_LOGI(TAG, "bring-up completo, entrando no loop do Slint");
    /* app_main retorna: a task main se encerra e libera os 16 KiB internos */
}
