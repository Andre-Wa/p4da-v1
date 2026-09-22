/**
 * main.cpp — PDA (milestone M1)
 *
 * Bring-up: storage -> config(Lua) -> display -> touch -> Slint -> lua VM
 * -> power mgmt. Toda I/O de disco roda em threads; a UI nunca bloqueia.
 * Scripts Lua rodam serializados por mutex (uma VM, uma execução por vez).
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

#include "esp_log.h"
#include "esp_timer.h"

#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <dirent.h>

static const char *TAG = "main";

static const AppWindow *g_ui = nullptr;  /* handle vive em app_main() */
static std::shared_ptr<slint::VectorModel<slint::SharedString>> g_script_log;
static std::mutex g_lua_mtx;
static std::string g_fm_dir;          /* diretório atual do gerenciador */
static bool g_hid_seen = false;
static std::string s_session_app = "launcher";  /* cache p/ session_save() */
static std::string s_session_note = "";

/* ------------------------------------------------------------------ */
static void activity(void) { power_mgmt_activity(); }

static void log_line(const char *line, void *ctx)
{
    (void)ctx;
    std::string s(line);
    slint::invoke_from_event_loop([s]() {
        if (g_script_log) {
            g_script_log->push_back(slint::SharedString(s));
            /* janela deslizante p/ não crescer sem limite */
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
static void update_clock(void)
{
    int64_t up_s = esp_timer_get_time() / 1000000LL;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             (int)(up_s / 3600), (int)((up_s / 60) % 60), (int)(up_s % 60));
    g_ui->set_status_clock(slint::SharedString(buf));

    const char *pwr = "ATIVO";
    switch (power_mgmt_state()) {
    case PDA_PWR_DIM: pwr = "DIM"; break;
    case PDA_PWR_STANDBY: pwr = "STANDBY"; break;
    default: pwr = "ATIVO"; break;
    }
    g_ui->set_status_pwr(slint::SharedString(pwr));
}

/* ---------------- gerenciador de arquivos ------------------------- */
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
    std::thread([dir]() {
        auto names = std::make_shared<slint::VectorModel<slint::SharedString>>();
        auto isdir = std::make_shared<slint::VectorModel<bool>>();

        /* sempre oferece subir, exceto na raiz do VFS */
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
            for (auto &n : dirs) { names->push_back(slint::SharedString(n)); isdir->push_back(true); }
            for (auto &n : files) { names->push_back(slint::SharedString(n)); isdir->push_back(false); }
            if (dirs.empty() && files.empty()) {
                names->push_back(slint::SharedString("(vazio)"));
                isdir->push_back(false);
            }
        } else {
            names->push_back(slint::SharedString("(não montado)"));
            isdir->push_back(false);
        }

        slint::invoke_from_event_loop([names, isdir, dir]() {
            g_ui->set_file_entries(names);
            g_ui->set_file_is_dir(isdir);
            g_ui->set_file_path(slint::SharedString(dir));
        });
    }).detach();
}

static void fm_open_entry(const std::string name)
{
    if (name == "(vazio)" || name == "(não montado)") return;
    std::string full = g_fm_dir + "/" + name;
    if (storage_is_dir(full.c_str())) {
        g_fm_dir = full;
        list_dir_async(g_fm_dir);
        return;
    }
    if (is_textish(name.c_str())) {
        /* abre no editor de notas (M2 vira editor genérico) */
        char *data = NULL; size_t len = 0;
        if (storage_read_file_alloc(full.c_str(), &data, &len) == ESP_OK) {
            std::string content(data, len);
            free(data);
            slint::invoke_from_event_loop([name, content]() {
                g_ui->set_note_name(slint::SharedString(name));
                g_ui->set_note_buffer(slint::SharedString(content));
                g_ui->set_note_is_new(false);
                g_ui->set_active_app(AppState::NoteEdit);
            });
            s_session_app = "noteedit";
            s_session_note = name;
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "[erro] não abriu %s", full.c_str());
            log_line(buf, NULL);
        }
        return;
    }
    char buf[256];
    int64_t sz = storage_file_size(full.c_str());
    snprintf(buf, sizeof(buf), "[info] %s: %ld bytes (visualização no M2)",
             name.c_str(), (long)sz);
    log_line(buf, NULL);
}

/* ---------------- notas -------------------------------------------- */
static std::string notes_dir(void)
{
    char buf[160];
    pda_path(buf, sizeof(buf), "notes");
    return std::string(buf);
}

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
    std::thread([]() {
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

/* ---------------- scripts lua -------------------------------------- */
static void refresh_scripts_list(void)
{
    std::thread([]() {
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
    std::thread([name]() {
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

/* ---------------- sessão (hibernação) ----------------------------- */
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
        std::string full = notes_dir() + "/" + note + ".txt";
        char *nd = NULL; size_t nl = 0;
        if (storage_read_file_alloc(full.c_str(), &nd, &nl) == ESP_OK) {
            g_ui->set_note_name(slint::SharedString(note));
            g_ui->set_note_buffer(slint::SharedString(std::string(nd, nl)));
            g_ui->set_note_is_new(false);
            g_ui->set_active_app(AppState::NoteEdit);
            free(nd);
        }
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

/* ---------------- config UI --------------------------------------- */
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
    g_ui->set_cfg_batt_info(slint::SharedString("nao medivel (IP5306)"));
    g_ui->set_status_batt(slint::SharedString("--"));
}

/* ================================================================== */
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== PDA M1 — bring-up ===");

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
        .panel_type = SlintDisplayPanelType::MipiDsiDpi,
    });

    auto ui = AppWindow::create();
    g_ui = ui.operator->();
    g_script_log = std::make_shared<slint::VectorModel<slint::SharedString>>();
    ui->set_script_log(g_script_log);
    g_fm_dir = pda_root();

    push_settings_to_ui();

    /* ---------- status bar ---------- */
    ui->on_tick([]() { update_clock(); });

    /* ---------- launcher ---------- */
    ui->on_open_app_dispatch([](slint::SharedString name) {
        activity();
        std::string n(name.data());
        if (n == "Arquivos") {
            g_fm_dir = pda_root();
            list_dir_async(g_fm_dir);
            g_ui->set_active_app(AppState::FileManager);
            s_session_app = "files";
        } else if (n == "Notas") {
            refresh_notes_list();
            g_ui->set_active_app(AppState::NotesList);
            s_session_app = "notes";
        } else if (n == "Scripts") {
            refresh_scripts_list();
            g_ui->set_active_app(AppState::Scripts);
            s_session_app = "scripts";
        } else if (n == "Config") {
            push_settings_to_ui();
            g_ui->set_active_app(AppState::Settings);
            s_session_app = "settings";
        }
    });

    /* ---------- arquivos ---------- */
    ui->on_request_dir([](slint::SharedString arg) {
        activity();
        std::string a(arg.data());
        if (!a.empty() && a[0] == '/') {          /* refresh: caminho completo */
            g_fm_dir = a;
            list_dir_async(g_fm_dir);
        } else {                                   /* toque numa entrada */
            fm_open_entry(a);
        }
    });
    ui->on_dir_up([]() {
        activity();
        std::string d = g_fm_dir;
        size_t slash = d.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
            std::string parent = d.substr(0, slash);
            if (parent.empty()) parent = "/";
            /* não subir acima das raízes montadas */
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

    /* ---------- notas ---------- */
    ui->on_request_notes_list([]() { activity(); refresh_notes_list(); });
    ui->on_open_note([](slint::SharedString name) {
        activity();
        std::string n(name.data());
        if (n.rfind("(nenhuma", 0) == 0) return;
        std::string full = notes_dir() + "/" + n + ".txt";
        char *data = NULL; size_t len = 0;
        if (storage_read_file_alloc(full.c_str(), &data, &len) == ESP_OK) {
            g_ui->set_note_name(name);
            g_ui->set_note_buffer(slint::SharedString(std::string(data, len)));
            g_ui->set_note_is_new(false);
            g_ui->set_active_app(AppState::NoteEdit);
            free(data);
            s_session_app = "noteedit";
            s_session_note = n;
        }
    });
    ui->on_new_note([]() {
        activity();
        std::string name = generate_new_note_name();
        g_ui->set_note_name(slint::SharedString(name));
        g_ui->set_note_buffer(slint::SharedString(""));
        g_ui->set_note_is_new(true);
        g_ui->set_active_app(AppState::NoteEdit);
        s_session_app = "noteedit";
        s_session_note = name;
    });
    ui->on_save_note([]() {
        activity();
        std::string name(g_ui->get_note_name().data());
        std::string content(g_ui->get_note_buffer().data());
        std::string full = notes_dir() + "/" + name + ".txt";
        if (storage_write_text_file(full.c_str(), content.data(), content.size()) == ESP_OK) {
            g_ui->set_note_is_new(false);
            ESP_LOGI(TAG, "nota salva: %s (%u bytes)", full.c_str(), (unsigned)content.size());
        }
    });
    ui->on_delete_note([]() {
        activity();
        std::string name(g_ui->get_note_name().data());
        std::string full = notes_dir() + "/" + name + ".txt";
        storage_delete_file(full.c_str());
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
        if (g_script_log) {
            g_script_log->clear();
        }
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
        /* sessão é salva pelo hook dentro de power_mgmt_hibernate() */
        power_mgmt_hibernate();
    });

    /* ---------- teclado USB ---------- */
    usb_hid_keyboard_init([](uint8_t ascii, uint8_t keycode, uint8_t /*mod*/) {
        char buf[16];
        if (ascii == '\n') snprintf(buf, sizeof(buf), "Enter");
        else if (ascii == '\b') snprintf(buf, sizeof(buf), "Backspace");
        else if (ascii == '\t') snprintf(buf, sizeof(buf), "Tab");
        else if (ascii == ' ') snprintf(buf, sizeof(buf), "Espaco");
        else if (ascii != 0) snprintf(buf, sizeof(buf), "%c", ascii);
        else snprintf(buf, sizeof(buf), "0x%02X", keycode);
        std::string label(buf);

        slint::invoke_from_event_loop([label, ascii]() {
            activity();
            if (!g_hid_seen) {
                g_hid_seen = true;
                g_ui->set_has_keyboard(true);
            }
            if (g_ui->get_active_app() == AppState::NoteEdit && ascii != 0) {
                std::string text(g_ui->get_note_buffer().data());
                if (ascii == '\b') {
                    if (!text.empty()) text.pop_back();
                } else if (ascii != '\t') {
                    text += (char)ascii;
                }
                g_ui->set_note_buffer(slint::SharedString(text));
            }
        });
    });

    /* ---------- power ---------- */
    power_mgmt_set_standby_cb([](bool entering, void *) {
        if (!entering) {
            /* acordou: reaplica brilho/config (RAM já estava viva) */
            slint::invoke_from_event_loop([]() { update_clock(); });
        }
    }, NULL);
    power_mgmt_set_hibernate_save_cb(session_save, NULL);
    power_mgmt_init();

    session_restore_if_needed();
    update_clock();

    ESP_LOGI(TAG, "bring-up completo, entrando no loop do Slint");
    ui->run();
}
