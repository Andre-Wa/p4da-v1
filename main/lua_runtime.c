/**
 * lua_runtime.c — VM Lua persistente + API `pda.*` para scripts do usuário.
 *
 * Segurança: scripts rodam sob lua_pcall com hook de contagem de
 * instruções (mata loop infinito) e allocator com teto de memória
 * (mata runaway alloc). Erro de script vira mensagem na tela/console,
 * nunca panic do firmware.
 */

#include "lua_runtime.h"
#include "pda_config.h"
#include "storage_init.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "lua_runtime";

static lua_State *s_L = NULL;
static size_t s_mem_used = 0;
static pda_lua_log_fn s_log_fn = NULL;
static void *s_log_ctx = NULL;
static pda_lua_toast_fn s_toast_fn = NULL;
static void *s_toast_ctx = NULL;
static void (*s_settings_changed_fn)(void) = NULL;

void lua_runtime_set_settings_changed_cb(void (*cb)(void));

/* ---------------- allocator com teto ------------------------------ */
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    if (nsize == 0) {
        free(ptr);
        s_mem_used -= osize;
        return NULL;
    }
    size_t next = s_mem_used - osize + nsize;
    if (next > PDA_LUA_MEM_LIMIT_BYTES) return NULL;
    void *np = realloc(ptr, nsize);
    if (np) s_mem_used = next;
    return np;
}

/* ---------------- hook anti-loop-infinito ------------------------- */
static void l_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    luaL_error(L, "limite de %d instrucoes excedido (loop infinito?)",
               PDA_LUA_INSTRUCTION_LIMIT);
}

/* ---------------- helpers ----------------------------------------- */
void lua_runtime_emit_log(const char *line)
{
    if (s_log_fn) s_log_fn(line, s_log_ctx);
}

/** Resolve caminho de script: absoluto (/sdcard, /internal) ou relativo
 *  à raiz ativa. Rejeita ".." para não escapar das raízes. */
static int resolve_path(lua_State *L, const char *in, char *out, size_t sz)
{
    if (!in || strstr(in, "..")) {
        return luaL_error(L, "caminho invalido (.. nao permitido): %s", in ? in : "(nil)");
    }
    if (strncmp(in, "/sdcard/", 8) == 0 || strncmp(in, "/internal/", 10) == 0) {
        if (strlen(in) >= sz)
            return luaL_error(L, "caminho longo demais");
        strlcpy(out, in, sz);
        return 0;
    }
    if (pda_path(out, sz, in) != ESP_OK)
        return luaL_error(L, "caminho longo demais");
    return 0;
}

/* ---------------- pda.log / toast / uptime ------------------------ */
static int l_log(lua_State *L)
{
    int n = lua_gettop(L);
    char line[256];
    size_t pos = 0;
    for (int i = 1; i <= n; i++) {
        const char *s = luaL_tolstring(L, i, NULL);
        if (s) {
            int w = snprintf(line + pos, sizeof(line) - pos, "%s%s",
                             (i > 1) ? " " : "", s);
            if (w > 0) pos += (size_t)w;
            if (pos >= sizeof(line) - 1) { pos = sizeof(line) - 1; break; }
        }
        lua_pop(L, 1);
    }
    line[pos < sizeof(line) ? pos : sizeof(line) - 1] = '\0';
    ESP_LOGI(TAG, "[script] %s", line);
    lua_runtime_emit_log(line);
    return 0;
}

static int l_toast(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    if (s_toast_fn) s_toast_fn(msg, s_toast_ctx);
    return 0;
}

static int l_uptime(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000000LL));
    return 1;
}

static int l_millis(lua_State *L)
{
    /* lua_Integer é 32-bit neste port (LUA_32BITS): envolve ~24,8 dias */
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000LL));
    return 1;
}

static int l_version(lua_State *L)
{
    lua_pushstring(L, "p4da 0.1.0");
    return 1;
}

static int l_root(lua_State *L)
{
    lua_pushstring(L, pda_root());
    return 1;
}

static int l_sd_mounted(lua_State *L)
{
    lua_pushboolean(L, storage_sd_mounted());
    return 1;
}

/* ---------------- pda.settings.* ---------------------------------- */
static bool key_is_bool(const char *k)
{
    return !strcmp(k, "power.wake_on_touch") || !strcmp(k, "ui.onscreen_keyboard_auto");
}

static bool key_is_str(const char *k)
{
    return !strcmp(k, "locale.timezone") || !strcmp(k, "locale.ntp_server");
}

static int l_settings_get(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    double num = 0; bool b = false; char str[96] = {0};
    if (!pda_settings_get(key, &num, &b, str, sizeof(str))) {
        return luaL_error(L, "chave de config desconhecida: %s", key);
    }
    if (key_is_bool(key)) lua_pushboolean(L, b);
    else if (key_is_str(key)) lua_pushstring(L, str);
    else lua_pushnumber(L, (lua_Number)num);
    return 1;
}

static int l_settings_set(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    double num = 0; bool is_bool = false, bval = false; const char *str = NULL;
    if (lua_isboolean(L, 2)) { is_bool = true; bval = lua_toboolean(L, 2); }
    else if (lua_isnumber(L, 2)) { num = lua_tonumber(L, 2); }
    else if (lua_isstring(L, 2)) { str = lua_tostring(L, 2); }
    else return luaL_error(L, "valor invalido p/ %s", key);

    if (!pda_settings_set(key, num, is_bool, bval, str))
        return luaL_error(L, "chave de config desconhecida: %s", key);
    if (s_settings_changed_fn) s_settings_changed_fn();
    return 0;
}

static int l_settings_save(lua_State *L)
{
    esp_err_t err = pda_config_save();
    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

/* ---------------- pda.fs.* ---------------------------------------- */
static int l_fs_read(lua_State *L)
{
    char path[192];
    resolve_path(L, luaL_checkstring(L, 1), path, sizeof(path));
    char *data = NULL; size_t len = 0;
    if (storage_read_file_alloc(path, &data, &len) != ESP_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "nao abriu %s", path);
        return 2;
    }
    lua_pushlstring(L, data, len);
    free(data);
    return 1;
}

static int fs_write_common(lua_State *L, bool append)
{
    char path[192];
    resolve_path(L, luaL_checkstring(L, 1), path, sizeof(path));
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    FILE *f = fopen(path, append ? "a" : "w");
    if (!f) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "nao abriu %s p/ escrita", path);
        return 2;
    }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    lua_pushboolean(L, w == len);
    return 1;
}

static int l_fs_write(lua_State *L) { return fs_write_common(L, false); }
static int l_fs_append(lua_State *L) { return fs_write_common(L, true); }

static int l_fs_list(lua_State *L)
{
    char path[192];
    resolve_path(L, luaL_checkstring(L, 1), path, sizeof(path));
    DIR *d = opendir(path);
    if (!d) {
        lua_pushnil(L);
        lua_pushfstring(L, "nao abriu diretorio %s", path);
        return 2;
    }
    lua_newtable(L);
    int i = 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        /* 480 >= 191 (path) + 1 ('/') + 255 (d_name) + NUL: o GCC prova que
         * não trunca e o -Wformat-truncation fica quieto. */
        char full[480];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        bool isdir = storage_is_dir(full);
        lua_pushinteger(L, i++);
        lua_pushfstring(L, "%s%s", ent->d_name, isdir ? "/" : "");
        lua_settable(L, -3);
    }
    closedir(d);
    return 1;
}

static int l_fs_exists(lua_State *L)
{
    char path[192];
    resolve_path(L, luaL_checkstring(L, 1), path, sizeof(path));
    lua_pushboolean(L, storage_file_exists(path) || storage_is_dir(path));
    return 1;
}

static int l_fs_size(lua_State *L)
{
    char path[192];
    resolve_path(L, luaL_checkstring(L, 1), path, sizeof(path));
    lua_pushinteger(L, (lua_Integer)storage_file_size(path));
    return 1;
}

static int l_fs_remove(lua_State *L)
{
    char path[192];
    resolve_path(L, luaL_checkstring(L, 1), path, sizeof(path));
    lua_pushboolean(L, storage_delete_file(path) == ESP_OK);
    return 1;
}

/* ---------------- registro da API --------------------------------- */
static const luaL_Reg s_pda_fs[] = {
    { "read", l_fs_read }, { "write", l_fs_write }, { "append", l_fs_append },
    { "list", l_fs_list }, { "exists", l_fs_exists }, { "size", l_fs_size },
    { "remove", l_fs_remove }, { NULL, NULL },
};
static const luaL_Reg s_pda_settings[] = {
    { "get", l_settings_get }, { "set", l_settings_set }, { "save", l_settings_save },
    { NULL, NULL },
};
static const luaL_Reg s_pda[] = {
    { "log", l_log }, { "toast", l_toast }, { "uptime", l_uptime },
    { "millis", l_millis }, { "version", l_version }, { "root", l_root },
    { "sd_mounted", l_sd_mounted }, { NULL, NULL },
};

esp_err_t lua_runtime_init(void)
{
    if (s_L) return ESP_OK;
    /* Lua 5.5: lua_newstate() ganhou 3o param (seed p/ hash de
     * strings). Alimentamos com entropy do RNG do chip. */
    s_L = lua_newstate(l_alloc, NULL, (unsigned)esp_random());
    if (!s_L) {
        ESP_LOGE(TAG, "sem memoria p/ VM Lua");
        return ESP_ERR_NO_MEM;
    }
    luaL_openlibs(s_L);

    lua_newtable(s_L);                       /* pda */
    luaL_setfuncs(s_L, s_pda, 0);
    lua_newtable(s_L); luaL_setfuncs(s_L, s_pda_fs, 0);
    lua_setfield(s_L, -2, "fs");
    lua_newtable(s_L); luaL_setfuncs(s_L, s_pda_settings, 0);
    lua_setfield(s_L, -2, "settings");
    lua_setglobal(s_L, "pda");

    ESP_LOGI(TAG, "VM Lua pronta (Lua %s), teto %d KB",
             LUA_RELEASE, PDA_LUA_MEM_LIMIT_BYTES / 1024);
    return ESP_OK;
}

void lua_runtime_set_log_callback(pda_lua_log_fn fn, void *ctx)
{ s_log_fn = fn; s_log_ctx = ctx; }
void lua_runtime_set_toast_callback(pda_lua_toast_fn fn, void *ctx)
{ s_toast_fn = fn; s_toast_ctx = ctx; }
void lua_runtime_set_settings_changed_cb(void (*cb)(void))
{ s_settings_changed_fn = cb; }

static esp_err_t run_protected(int (*loader)(lua_State *), const char *what,
                               char *err_msg, size_t err_sz)
{
    if (!s_L) return ESP_ERR_INVALID_STATE;
    lua_settop(s_L, 0);

    if (loader(s_L) != LUA_OK) {
        snprintf(err_msg, err_sz, "%s", lua_tostring(s_L, -1) ? lua_tostring(s_L, -1) : "erro de carga");
        lua_settop(s_L, 0);
        return ESP_FAIL;
    }
    lua_sethook(s_L, l_hook, LUA_MASKCOUNT, PDA_LUA_INSTRUCTION_LIMIT);
    int rc = lua_pcall(s_L, 0, 0, 0);
    lua_sethook(s_L, NULL, 0, 0);

    if (rc != LUA_OK) {
        const char *msg = lua_tostring(s_L, -1);
        snprintf(err_msg, err_sz, "%s", msg ? msg : "erro desconhecido");
        ESP_LOGE(TAG, "script %s falhou: %s", what, err_msg);
        char line[320];
        snprintf(line, sizeof(line), "ERRO: %s", err_msg);
        lua_runtime_emit_log(line);
        lua_settop(s_L, 0);
        return ESP_FAIL;
    }
    lua_settop(s_L, 0);
    return ESP_OK;
}

static const char *s_pending_path = NULL;
static int load_pending_file(lua_State *L) { return luaL_loadfile(L, s_pending_path); }
static const char *s_pending_src = NULL;
static int load_pending_string(lua_State *L) { return luaL_loadstring(L, s_pending_src); }

esp_err_t lua_runtime_run_file(const char *path, char *err_msg, size_t err_sz)
{
    s_pending_path = path;
    esp_err_t e = run_protected(load_pending_file, path, err_msg, err_sz);
    s_pending_path = NULL;
    return e;
}

esp_err_t lua_runtime_run_string(const char *src, char *err_msg, size_t err_sz)
{
    s_pending_src = src;
    esp_err_t e = run_protected(load_pending_string, "(string)", err_msg, err_sz);
    s_pending_src = NULL;
    return e;
}
