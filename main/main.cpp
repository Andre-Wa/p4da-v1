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

#include "board_config.h"
#include "display_init.h"
#include "touch_init.h"
#include "usb_hid_keyboard.h"
#include "storage_init.h"
#include "pda_config.h"
#include "lua_runtime.h"
#include "power_mgmt.h"
#include "wifi_net.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pthread.h"

#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

/* Geometria do editor — ESPELHA os tokens ESTÁTICOS do theme.slint
 * (line-h 25, osk-h 193, status-h 34, btn-h-sm 39, body 18px => ~11px/char).
 * Modelo 100% estático: escala de UI se muda com tools/scale_type.py +
 * rebuild (o renderer pré-rasteriza fontes; e tokens mutáveis em runtime
 * já nos custaram um bootloop por NaN). Se mudar token lá, mude aqui. */
static const float ED_LINE_H = 25.f;
static const float ED_OSK_H = 193.f;
static const float ED_STATUS_H = 34.f;
static const float ED_BTN_SM = 39.f;
static const int ED_CHAR_W = 11;

static void activity(void) { power_mgmt_activity(); }

/* forward decls (ordem de definição vs uso) */
static void refresh_notes_list(void);
static void refresh_scripts_list(void);
static void list_dir_async(const std::string dir);
static void nav_goto(AppState st);
static void nav_back(void);

/** std::thread com pilha explícita: o default do IDF (~3K) estoura a VM Lua
 *  e é apertado p/ readdir+FATFS. Chamado NA task que cria a thread. */
template <typename F>
static std::thread spawn_thread(const char *name, size_t stack, F &&fn)
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = name;
    cfg.stack_size = stack;
    cfg.prio = 5;
    esp_pthread_set_cfg(&cfg);
    return std::thread(std::forward<F>(fn));
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

/* ---------------- relógio da status bar --------------------------- */
void wifi_net_on_event_ui(bool connected, int rssi)
{
    char buf[40];
    if (connected) snprintf(buf, sizeof(buf), "wifi %d dBm", rssi);
    else snprintf(buf, sizeof(buf), "wifi --");
    std::string s(buf);
    slint::invoke_from_event_loop([s]() {
        g_ui->set_status_wifi(slint::SharedString(s));
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
    if (g_ed.osk_override >= 0) return g_ed.osk_override == 1;
    return pda_settings()->onscreen_keyboard_auto && !usb_hid_keyboard_connected();
}

/* linhas de exibição = linhas reais com "|" injetado na coluna do cursor */
static void ed_push_ui(bool scroll_to_cursor)
{
    auto model = std::make_shared<slint::VectorModel<slint::SharedString>>();
    std::vector<std::string> lines;
    int cursor_line = 0, cursor_col = 0;
    {
        size_t i = 0, line_idx = 0;
        size_t line_start = 0;
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

    size_t maxw = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        size_t w = lines[i].size() + ((int)i == cursor_line ? 1 : 0);
        if (w > maxw) maxw = w;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        if ((int)i == cursor_line) {
            std::string l = lines[i];
            if ((size_t)cursor_col > l.size()) cursor_col = (int)l.size();
            l.insert(l.begin() + cursor_col, '|');
            model->push_back(slint::SharedString(l));
        } else {
            model->push_back(slint::SharedString(lines[i]));
        }
    }

    g_ui->set_ed_lines(model);
    g_ui->set_ed_line_count((int)lines.size());
    g_ui->set_ed_content_w((float)(maxw * ED_CHAR_W + 60));
    g_ui->set_ed_title(slint::SharedString(ed_title()));
    g_ui->set_ed_dirty(g_ed.dirty);
    g_ui->set_ed_can_delete(g_ed.path.rfind(notes_dir() + "/", 0) == 0);
    g_ui->set_ed_show_osk(ed_osk_should_show());

    if (scroll_to_cursor) {
        int content_h = (int)(lines.size() * ED_LINE_H) + 40;
        int area_h = (int)(480 - ED_STATUS_H - (ED_BTN_SM + 14) - 16 -
                     (ed_osk_should_show() ? ED_OSK_H + 6 : 0));
        int target = (int)(cursor_line * ED_LINE_H) - area_h / 2;
        if (target < 0) target = 0;
        int maxy = content_h - area_h;
        if (target > maxy) target = maxy > 0 ? maxy : 0;
        g_ui->set_ed_scroll_y((float)target);
    }
}

static void ed_insert_str(const char *s, size_t n)
{
    if (g_ed.cursor > g_ed.text.size()) g_ed.cursor = g_ed.text.size();
    g_ed.text.insert(g_ed.cursor, s, n);
    g_ed.cursor += n;
    g_ed.dirty = true;
    ed_push_ui(true);
}

static void ed_backspace(void)
{
    if (g_ed.cursor == 0 || g_ed.text.empty()) return;
    g_ed.cursor--;
    g_ed.text.erase(g_ed.cursor, 1);
    g_ed.dirty = true;
    ed_push_ui(true);
}

static void ed_delete_key(void)
{
    if (g_ed.cursor >= g_ed.text.size()) return;
    g_ed.text.erase(g_ed.cursor, 1);
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
        g_ed.cursor = spans[target].first + c;
        (void)ls; (void)idx;
        ed_push_ui(true);
        return;
    }
    if (dcol > 0) {
        if (g_ed.cursor < g_ed.text.size()) g_ed.cursor++;
    } else if (dcol < 0) {
        if (g_ed.cursor > 0) g_ed.cursor--;
    }
    ed_push_ui(true);
}

static void ed_load(const std::string &path, const std::string &content, bool is_new)
{
    g_ed.path = path;
    g_ed.text = content;
    g_ed.cursor = 0;
    g_ed.dirty = false;
    g_ed.is_new = is_new;
    g_ed.osk_override = -1;
    ed_push_ui(false);
    g_ui->set_ed_scroll_y(0.f);
    nav_goto(AppState::Editor);
    s_session_app = "noteedit";
    s_session_note = is_new ? std::string("") : ed_title();
}

static void ed_open_path_async(const std::string path)
{
    spawn_thread("io_edit", 8192, [path]() {
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
        slint::invoke_from_event_loop([path, content]() {
            ed_load(path, content, false);
        });
    }).detach();
}

static void ed_save(void)
{
    if (g_ed.path.empty()) return;
    if (storage_write_text_file(g_ed.path.c_str(), g_ed.text.data(), g_ed.text.size()) == ESP_OK) {
        g_ed.dirty = false;
        g_ed.is_new = false;
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
        { "1 2 3 4 5 6 7 8 9 0", "! @ # $ % ^ & * ( )", "- = [ ] \\ _ + { } |" },
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
    }).detach();
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
    g_ui->set_fm_prompt_text(slint::SharedString(g_fm.prompt_text));
    g_ui->set_fm_prompt_needs_text(g_fm.prompt_needs_text);
}

static std::string base_name(const std::string &p)
{
    size_t i = p.find_last_of('/');
    return (i == std::string::npos) ? p : p.substr(i + 1);
}

static void fm_open_entry(const std::string name)
{
    if (name == "(vazio)" || name == "(não montado)") return;
    std::string full = g_fm_dir + "/" + name;
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
        ed_open_path_async(full);
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

static void refresh_notes_list(void)
{
    spawn_thread("io_notes", 8192, []() {
        auto model = std::make_shared<slint::VectorModel<slint::SharedString>>();
        DIR *dir = opendir(notes_dir().c_str());
        int count = 0;
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                std::string name(ent->d_name);
                if (name == "." || name == "..") continue;
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0)
                    name = name.substr(0, name.size() - 4);
                model->push_back(slint::SharedString(name));
                count++;
            }
            closedir(dir);
        }
        if (count == 0)
            model->push_back(slint::SharedString("(nenhuma nota — toque em + Nova nota)"));
        slint::invoke_from_event_loop([model]() { g_ui->set_notes_list(model); });
    }).detach();
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
    }).detach();
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
    }).detach();
}

/* ================================================================== */
/* SESSÃO (hibernação)                                                 */
/* ================================================================== */
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
    char path[160];
    pda_path(path, sizeof(path), ".state/session.txt");
    char *data = NULL; size_t len = 0;
    if (storage_read_file_alloc(path, &data, &len) != ESP_OK) return;
    std::string s(data, len);
    free(data);

    std::string app, note;
    size_t p = s.find("app=");
    if (p != std::string::npos) app = s.substr(p + 4, s.find('\n', p) - p - 4);
    p = s.find("note=");
    if (p != std::string::npos) note = s.substr(p + 5, s.find('\n', p) - p - 5);

    ESP_LOGI(TAG, "restaurando sessão: app=%s note=%s", app.c_str(), note.c_str());
    if (app == "noteedit" && !note.empty()) {
        std::string full = std::string(pda_root()) + "/" + note;
        ed_open_path_async(full);
    } else if (app == "notes") {
        refresh_notes_list();
        g_ui->set_active_app(AppState::NotesList);
    } else if (app == "files") {
        g_fm_dir = pda_root();
        list_dir_async(g_fm_dir);
        g_ui->set_active_app(AppState::FileManager);
    } else if (app == "scripts") {
        refresh_scripts_list();
        g_ui->set_active_app(AppState::Scripts);
    }
    storage_delete_file(path);
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
    g_ui->set_cfg_osk_auto(s->onscreen_keyboard_auto);

    char buf[96];
    storage_sd_describe(buf, sizeof(buf));
    g_ui->set_cfg_store_info(slint::SharedString(buf));
    g_ui->set_status_store(slint::SharedString(buf));
    g_ui->set_status_store_icon(slint::SharedString(storage_sd_mounted() ? "sd" : "int"));
    g_ui->set_cfg_batt_info(slint::SharedString("nao medivel (IP5306)"));
    g_ui->set_status_batt(slint::SharedString("--"));
}

/* ================================================================== */
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== PDA M2 — bring-up ===");

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
        if (g_fm.prompt || g_fm.sheet) {
            g_fm.prompt = g_fm.sheet = false;
            push_fm_ui();
            return;
        }
        /* picker: Sobe navega normalmente (precisa conseguir sair de
         * subdiretórios para escolher destino!) */
        std::string d = g_fm_dir;
        size_t slash = d.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
            std::string parent = d.substr(0, slash);
            if (parent.empty()) parent = "/";
            if (parent == "/sdcard" || parent == "/internal" || parent == "/") {
                g_fm_dir = pda_root();
            } else {
                g_fm_dir = parent;
            }
        } else {
            g_fm_dir = pda_root();
        }
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
            g_fm.prompt_action = "rename";
            push_fm_ui();
            return;
        }
        if (act == "copiar" || act == "mover") {
            g_fm.sheet = false;
            g_fm.picker = true;
            g_fm.picker_op = (act == "copiar") ? "copy" : "move";
            g_fm.picker_src = g_fm_dir + "/" + g_fm.sheet_name;
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
        g_fm.prompt_action = "newfile";
        push_fm_ui();
    });
    ui->on_fm_newdir([]() {
        activity();
        g_fm.prompt = true;
        g_fm.prompt_needs_text = true;
        g_fm.prompt_title = "Nome do novo diretório:";
        g_fm.prompt_text = "";
        g_fm.prompt_action = "newdir";
        push_fm_ui();
    });
    ui->on_fm_picker_select([]() {
        activity();
        if (!g_fm.picker) return;
        std::string dest = g_fm_dir + "/" + base_name(g_fm.picker_src);
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
        std::string target = g_fm_dir + "/" + g_fm.sheet_name;
        char msg[320];
        if (g_fm.prompt_action == "rename") {
            std::string nt = g_fm.prompt_text;
            if (nt.empty() || nt == "." || nt == ".." || nt.find('/') != std::string::npos) {
                log_line("[erro] nome inválido", NULL);
            } else {
                std::string dest = g_fm_dir + "/" + nt;
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
                storage_ensure_dir((g_fm_dir + "/" + nt).c_str());
                log_line("[ok] diretório criado", NULL);
            }
        } else if (g_fm.prompt_action == "newfile") {
            std::string nt = g_fm.prompt_text;
            if (nt.empty() || nt.find('/') != std::string::npos) {
                log_line("[erro] nome inválido", NULL);
            } else {
                std::string path = g_fm_dir + "/" + nt;
                if (storage_file_exists(path.c_str())) {
                    log_line("[erro] arquivo já existe", NULL);
                } else if (storage_write_text_file(path.c_str(), "", 0) == ESP_OK) {
                    log_line("[ok] arquivo criado", NULL);
                    g_fm.prompt = false;
                    push_fm_ui();
                    ed_open_path_async(path);   /* já abre p/ editar */
                    return;
                }
            }
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
    ui->on_app_back([]() {
        activity();
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
        ed_open_path_async(notes_dir() + "/" + n + ".txt");
    });
    ui->on_new_note([]() {
        activity();
        std::string name = generate_new_note_name();
        std::string path = notes_dir() + "/" + name + ".txt";
        ed_load(path, "", true);
    });

    /* ---------- editor ---------- */
    ui->on_ed_line_tapped([](int line) {
        activity();
        /* cursor no FIM da linha tocada (coluna via setas/OSK) */
        size_t line_start = 0;
        int idx = 0;
        for (size_t i = 0; i <= g_ed.text.size(); i++) {
            if (i == g_ed.text.size() || g_ed.text[i] == '\n') {
                if (idx == line) { g_ed.cursor = i; break; }
                idx++;
                line_start = i + 1;
                (void)line_start;
            }
        }
        ed_push_ui(true);
    });
    ui->on_ed_osk_key([](slint::SharedString k) {
        activity();
        std::string key(k.data());
        if (g_fm.prompt && g_fm.prompt_needs_text) {
            if (key == "BACKSPACE") {
                if (!g_fm.prompt_text.empty()) g_fm.prompt_text.pop_back();
            } else if (key == "ENTER") {
                g_ui->invoke_fm_prompt_ok();
                return;
            } else if (key == "HIDE" || key == "MODE" || key == "LEFT" ||
                       key == "RIGHT" || key == "UP" || key == "DOWN") {
                if (key == "MODE") { g_ed.osk_mode = !g_ed.osk_mode; ed_push_osk_rows(); }
                return;
            } else {
                g_fm.prompt_text += key;
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
        s.onscreen_keyboard_auto = g_ui->get_cfg_osk_auto();
        pda_settings_update(&s);
        pda_config_save();
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
    ui->on_cfg_hibernate([]() {
        power_mgmt_hibernate();
    });

    /* ---------- teclado USB ---------- */
    usb_hid_keyboard_init([](uint8_t ascii, uint8_t keycode, uint8_t /*mod*/) {
        slint::invoke_from_event_loop([ascii, keycode]() {
            activity();
            if (g_ui->get_active_app() != AppState::Editor) return;
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
        if (entering) {
            usb_hid_keyboard_prepare_sleep();
            return;
        }
        storage_remount_sd();
        usb_hid_keyboard_resume();
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
    if (wifi_net_init() == ESP_OK) {
        g_ui->set_status_wifi(slint::SharedString("wifi ..."));
    } else {
        g_ui->set_status_wifi(slint::SharedString("wifi off"));
    }

    session_restore_if_needed();
    update_clock();

    ESP_LOGI(TAG, "bring-up completo, entrando no loop do Slint");
    ui->run();
}
