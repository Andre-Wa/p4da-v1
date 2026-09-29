/**
 * pda_config.c — configurações do sistema em Lua.
 *
 * O arquivo é um chunk Lua que RETORNA uma tabela:
 *
 *   return { display = { brightness = 80 }, power = { ... }, ... }
 *
 * Isso permite comentários, edição à mão no cartão e, no futuro,
 * valores computados (ex.: brilho diferente por horário).
 */

#include "pda_config.h"
#include "storage_init.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lua.h"
#include "lauxlib.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

static const char *TAG = "pda_config";

static pda_settings_t s_cfg;
static bool s_dirty = false;
static bool s_force_recreate = false;  /* delete+create p/ entradas viciadas */

static void apply_defaults(pda_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->brightness = 80;
    s->dim_after_s = 30;
    s->screen_off_after_s = 120;
    s->deep_sleep_after_s = 600;
    s->wake_on_touch = false;
    s->light_sleep = false;
    strlcpy(s->timezone, "America/Sao_Paulo", sizeof(s->timezone));
    strlcpy(s->ntp_server, "pool.ntp.org", sizeof(s->ntp_server));
    s->onscreen_keyboard_auto = true;
}

/* ------------------------------------------------------------------ */
static int tbl_int(lua_State *L, int idx, const char *name, int def)
{
    lua_getfield(L, idx, name);
    double d = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : (double)def;
    lua_pop(L, 1);
    if (!isfinite(d)) d = def;   /* nan/inf no arquivo não derrubam o boot */
    return (int)d;
}

static bool tbl_bool(lua_State *L, int idx, const char *name, bool def)
{
    lua_getfield(L, idx, name);
    bool v = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static void tbl_str(lua_State *L, int idx, const char *name, char *out, size_t sz, const char *def)
{
    /* strlcpy: nunca dispara -Wformat-truncation e sempre termina em NUL.
     * Cópia do default antes: out e def podem apontar p/ o mesmo buffer. */
    char defcopy[128];
    strlcpy(defcopy, def ? def : "", sizeof(defcopy));
    lua_getfield(L, idx, name);
    if (lua_isstring(L, -1)) {
        strlcpy(out, lua_tostring(L, -1), sz);
    } else {
        strlcpy(out, defcopy, sz);
    }
    lua_pop(L, 1);
}

/* Limites = autoridade dos sliders da tela Config. Valores fora de faixa
 * (ex.: o INT_MAX que o bug de NaN/escala deixou gravado em system.lua)
 * são substituídos pelos defaults e sinalizados p/ o boot curar o arquivo.
 * Retorna true se algo foi saneado. */
static bool clamp_all(pda_settings_t *s)
{
    pda_settings_t before = *s;
    /* loga campo a campo: sem isto, "valores fora de faixa" não dizia
     * QUAL campo vinha insano do arquivo (boot de 2026-09-24). */
    if (s->brightness < 0 || s->brightness > 100) {
        ESP_LOGW(TAG, "sanitize: brightness=%d -> 80", s->brightness);
        s->brightness = 80;
    }
    if (s->dim_after_s < 5 || s->dim_after_s > 600) {
        ESP_LOGW(TAG, "sanitize: dim_after_s=%d -> 30", s->dim_after_s);
        s->dim_after_s = 30;
    }
    if (s->screen_off_after_s < 10 || s->screen_off_after_s > 1800) {
        ESP_LOGW(TAG, "sanitize: screen_off_after_s=%d -> 120", s->screen_off_after_s);
        s->screen_off_after_s = 120;
    }
    if (s->deep_sleep_after_s < 0 || s->deep_sleep_after_s > 7200) {
        ESP_LOGW(TAG, "sanitize: deep_sleep_after_s=%d -> 600", s->deep_sleep_after_s);
        s->deep_sleep_after_s = 600;
    }
    /* ordenação (ajuste silencioso, não é "veneno") */
    if (s->screen_off_after_s < s->dim_after_s + 5)
        s->screen_off_after_s = s->dim_after_s + 5;
    if (s->deep_sleep_after_s != 0 && s->deep_sleep_after_s < s->screen_off_after_s + 10)
        s->deep_sleep_after_s = s->screen_off_after_s + 10;
    return memcmp(&before, s, sizeof(*s)) != 0;
}

static esp_err_t parse_file(const char *path, pda_settings_t *out)
{
    lua_State *L = luaL_newstate();
    if (!L) return ESP_ERR_NO_MEM;

    pda_settings_t tmp = *out; /* defaults como ponto de partida */
    esp_err_t err = ESP_OK;

    if (luaL_loadfile(L, path) != LUA_OK) {
        ESP_LOGE(TAG, "erro de sintaxe em %s: %s", path, lua_tostring(L, -1));
        err = ESP_FAIL;
        goto done;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "erro ao executar %s: %s", path, lua_tostring(L, -1));
        err = ESP_FAIL;
        goto done;
    }
    if (!lua_istable(L, -1)) {
        ESP_LOGE(TAG, "%s não retorna uma tabela", path);
        err = ESP_FAIL;
        goto done;
    }

    /* display */
    lua_getfield(L, -1, "display");
    if (lua_istable(L, -1)) {
        tmp.brightness = tbl_int(L, -1, "brightness", tmp.brightness);
    }
    lua_pop(L, 1);

    /* power */
    lua_getfield(L, -1, "power");
    if (lua_istable(L, -1)) {
        tmp.dim_after_s = tbl_int(L, -1, "dim_after_s", tmp.dim_after_s);
        tmp.screen_off_after_s = tbl_int(L, -1, "screen_off_after_s", tmp.screen_off_after_s);
        tmp.deep_sleep_after_s = tbl_int(L, -1, "deep_sleep_after_s", tmp.deep_sleep_after_s);
        tmp.wake_on_touch = tbl_bool(L, -1, "wake_on_touch", tmp.wake_on_touch);
        tmp.light_sleep = tbl_bool(L, -1, "light_sleep", tmp.light_sleep);
    }
    lua_pop(L, 1);

    /* locale */
    lua_getfield(L, -1, "locale");
    if (lua_istable(L, -1)) {
        tbl_str(L, -1, "timezone", tmp.timezone, sizeof(tmp.timezone), tmp.timezone);
        tbl_str(L, -1, "ntp_server", tmp.ntp_server, sizeof(tmp.ntp_server), tmp.ntp_server);
    }
    lua_pop(L, 1);

    /* ui */
    lua_getfield(L, -1, "ui");
    if (lua_istable(L, -1)) {
        tmp.onscreen_keyboard_auto = tbl_bool(L, -1, "onscreen_keyboard_auto", tmp.onscreen_keyboard_auto);
    }
    lua_pop(L, 1);

    *out = tmp;

done:
    lua_close(L);
    return err;
}

/* ------------------------------------------------------------------ */
/* Apaga TODAS as entradas com este nome (o FAT pode acumular duplicatas
 * viciadas; storage_delete_file remove só a primeira que ele resolve). */
static void purge_path(const char *path)
{
    for (int i = 0; i < 8 && storage_file_exists(path); i++) {
        storage_delete_file(path);
    }
}

/* ---- sombra NVS: fonte de recuperação à prova de power-loss ---- */
#define NVS_NS "pdacfg"
static void nvs_shadow_save(const pda_settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "br", s->brightness);
    nvs_set_i32(h, "dim", s->dim_after_s);
    nvs_set_i32(h, "off", s->screen_off_after_s);
    nvs_set_i32(h, "deep", s->deep_sleep_after_s);
    nvs_set_u8(h, "wake", s->wake_on_touch ? 1 : 0);
    nvs_set_u8(h, "osk", s->onscreen_keyboard_auto ? 1 : 0);
    nvs_set_u8(h, "ls", s->light_sleep ? 1 : 0);
    nvs_set_str(h, "tz", s->timezone);
    nvs_set_str(h, "ntp", s->ntp_server);
    nvs_commit(h);
    nvs_close(h);
}

static bool nvs_shadow_load(pda_settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    int32_t v;
    uint8_t b;
    size_t sz;
    bool ok = true;
    if (nvs_get_i32(h, "br", &v) == ESP_OK) s->brightness = v; else ok = false;
    if (nvs_get_i32(h, "dim", &v) == ESP_OK) s->dim_after_s = v; else ok = false;
    if (nvs_get_i32(h, "off", &v) == ESP_OK) s->screen_off_after_s = v; else ok = false;
    if (nvs_get_i32(h, "deep", &v) == ESP_OK) s->deep_sleep_after_s = v; else ok = false;
    if (nvs_get_u8(h, "wake", &b) == ESP_OK) s->wake_on_touch = b != 0;
    if (nvs_get_u8(h, "osk", &b) == ESP_OK) s->onscreen_keyboard_auto = b != 0;
    if (nvs_get_u8(h, "ls", &b) == ESP_OK) s->light_sleep = b != 0;
    sz = sizeof(s->timezone);
    nvs_get_str(h, "tz", s->timezone, &sz);
    sz = sizeof(s->ntp_server);
    nvs_get_str(h, "ntp", s->ntp_server, &sz);
    nvs_close(h);
    return ok;
}

static esp_err_t serialize_to(const char *path, const pda_settings_t *s)
{
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "-- Configuracao do PDA (gerada pelo sistema; edite a vontade).\n"
        "-- O chunk precisa RETORNAR uma tabela. Ver docs/LUA.md.\n"
        "return {\n"
        "  display = {\n"
        "    brightness = %d,            -- 0..100\n"
        "  },\n"
        "  power = {\n"
        "    dim_after_s = %d,           -- degrau 1: dimeriza backlight\n"
        "    screen_off_after_s = %d,    -- degrau 2: tela off + light sleep\n"
        "    deep_sleep_after_s = %d,    -- degrau 3: deep sleep (restore do SD)\n"
        "    wake_on_touch = %s,    -- wake por touch (INT GT911)\n"
        "    light_sleep = %s,       -- true: light sleep no standby (exp.)\n"
        "  },\n"
        "  locale = {\n"
        "    timezone = \"%s\",\n"
        "    ntp_server = \"%s\",\n"
        "  },\n"
        "  ui = {\n"
        "    onscreen_keyboard_auto = %s, -- teclado virtual so sem teclado USB\n"
        "  },\n"
        "}\n",
        s->brightness,
        s->dim_after_s, s->screen_off_after_s, s->deep_sleep_after_s,
        s->wake_on_touch ? "true" : "false",
        s->light_sleep ? "true" : "false",
        s->timezone, s->ntp_server,
        s->onscreen_keyboard_auto ? "true" : "false");
    if (n <= 0 || (size_t)n >= sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    return storage_write_text_file(path, buf, (size_t)n);
}

/* Grava e RELÊ para confirmar persistência. Cartões com entrada de
 * diretório/cluster cansada podem "aceitar" a escrita no cache e nunca
 * atualizar o arquivo no meio físico — o boot seguinte relê o conteúdo
 * velho (foi assim que os INT_MAX sobreviveram a várias curas). */
static bool save_persists(const char *path)
{
    char *data = NULL;
    size_t len = 0;
    if (storage_read_file_alloc(path, &data, &len) != ESP_OK) return false;
    lua_State *L = luaL_newstate();
    if (!L) { free(data); return true; }
    bool ok = true;
    if (luaL_loadbuffer(L, data, len, "verify") == LUA_OK &&
        lua_pcall(L, 0, 1, 0) == LUA_OK && lua_istable(L, -1)) {
        lua_getfield(L, -1, "power");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "dim_after_s");
            if (lua_isnumber(L, -1) && (int)lua_tointeger(L, -1) != s_cfg.dim_after_s) ok = false;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    } else {
        ok = false;
    }
    lua_close(L);
    free(data);
    return ok;
}

esp_err_t pda_config_save(void)
{
    char path[160];
    if (pda_path(path, sizeof(path), "config/system.lua") != ESP_OK)
        return ESP_ERR_INVALID_SIZE;
    if (s_force_recreate) {
        /* PURGA de entradas duplicadas/viciadas no FAT (o open resolve a
         * entrada ANTIGA; create anexa no fim e nunca é lida — mtime 1980
         * no boot seguinte é a assinatura). Apaga até não existir mais. */
        ESP_LOGW(TAG, "recriando system.lua (purga de entradas viciadas)");
        purge_path(path);
        purge_path("/internal/pda/config/system.lua");
        purge_path("/sdcard/pda/config/system.lua");
        s_force_recreate = false;
    }
    esp_err_t err = serialize_to(path, &s_cfg);
    if (err == ESP_OK && !save_persists(path)) {
        ESP_LOGW(TAG, "escrita nao persistiu no cartao; recriando arquivo");
        storage_delete_file(path);
        err = serialize_to(path, &s_cfg);
        if (err == ESP_OK && !save_persists(path)) {
            ESP_LOGE(TAG, "SD nao persiste system.lua mesmo recriado (cartao/FAT?)");
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao salvar %s", path);
        return err;
    }
    /* espelho melhor-esforço na outra raiz montada */
    if (storage_sd_mounted()) {
        serialize_to("/internal/pda/config/system.lua", &s_cfg);
    }
    nvs_shadow_save(&s_cfg);
    s_dirty = false;
    ESP_LOGI(TAG, "config salva em %s (br=%d dim=%d off=%d deep=%d)", path,
             s_cfg.brightness, s_cfg.dim_after_s, s_cfg.screen_off_after_s,
             s_cfg.deep_sleep_after_s);
    return ESP_OK;
}

esp_err_t pda_config_init(void)
{
    apply_defaults(&s_cfg);

    char path[160];
    if (pda_path(path, sizeof(path), "config/system.lua") != ESP_OK)
        return ESP_ERR_INVALID_SIZE;
    const char *alt = storage_sd_mounted()
        ? "/internal/pda/config/system.lua"
        : "/sdcard/pda/config/system.lua";

    if (!storage_file_exists(path)) {
        /* Antes de apelar p/ defaults: promove a cópia da OUTRA raiz
         * (espelho), para nunca "esquecer" configurações por um boot
         * em que uma das raízes estava ausente/vazia. */
        const char *alt = storage_sd_mounted()
            ? "/internal/pda/config/system.lua"
            : "/sdcard/pda/config/system.lua";
        if (storage_file_exists(alt) && storage_copy_file(alt, path) == ESP_OK) {
            ESP_LOGW(TAG, "system.lua ausente em %s — promovido de %s", path, alt);
        } else {
            ESP_LOGI(TAG, "system.lua ausente — criando com defaults em %s", path);
            return pda_config_save();
        }
    }
    bool healed = false;
    esp_err_t err = parse_file(path, &s_cfg);
    bool bad = (err != ESP_OK) || clamp_all(&s_cfg);
    if (bad) {
        /* Cadeia de recuperação: espelho da outra raiz -> sombra NVS ->
         * defaults. O FAT desta placa já apresentou entradas duplicadas
         * viciadas (boot relê conteúdo antigo mesmo após save verificado);
         * NVS (flash interna, power-loss safe) é a última rede. */
        pda_settings_t mirror;
        apply_defaults(&mirror);
        if (parse_file(alt, &mirror) == ESP_OK && !clamp_all(&mirror)) {
            ESP_LOGW(TAG, "system.lua da raiz ativa insano; recuperado do espelho %s", alt);
            s_cfg = mirror;
        } else {
            pda_settings_t sh;
            apply_defaults(&sh);
            if (nvs_shadow_load(&sh) && !clamp_all(&sh)) {
                ESP_LOGW(TAG, "system.lua insano nas duas raizes; recuperado da sombra NVS");
                s_cfg = sh;
            } else {
                ESP_LOGW(TAG, "system.lua insano em tudo — defaults + regravação");
                apply_defaults(&s_cfg);
            }
        }
        s_force_recreate = true;   /* cura => purga + arquivo novo nas raízes */
        pda_config_save();
        healed = true;
    }
    if (err == ESP_OK) {
        if (!healed) {
            ESP_LOGI(TAG, "system.lua intacto — nenhuma regravação no boot");
        }
    {
        struct stat st;
        if (stat(path, &st) == 0)
            ESP_LOGI(TAG, "system.lua ativa: %ld B mtime=%ld", (long)st.st_size, (long)st.st_mtime);
        if (stat(alt, &st) == 0)
            ESP_LOGI(TAG, "system.lua espelho: %ld B mtime=%ld", (long)st.st_size, (long)st.st_mtime);
    }
        ESP_LOGI(TAG, "config carregada: brilho=%d dim=%ds off=%ds deep=%ds",
                 s_cfg.brightness, s_cfg.dim_after_s,
                 s_cfg.screen_off_after_s, s_cfg.deep_sleep_after_s);
    }
    return ESP_OK;
}

const pda_settings_t *pda_settings(void) { return &s_cfg; }
bool pda_config_dirty(void) { return s_dirty; }

void pda_settings_update(const pda_settings_t *s)
{
    s_cfg = *s;
    clamp_all(&s_cfg);
    s_dirty = true;
}


esp_err_t pda_config_reload(void)
{
    char path[160];
    if (pda_path(path, sizeof(path), "config/system.lua") != ESP_OK)
        return ESP_ERR_INVALID_SIZE;
    pda_settings_t tmp = s_cfg;
    if (parse_file(path, &tmp) != ESP_OK) return ESP_FAIL;
    bool healed = clamp_all(&tmp);
    s_cfg = tmp;
    if (healed) {
        ESP_LOGW(TAG, "system.lua editado com valores fora de faixa — saneado e regravado");
        pda_config_save();
    } else {
        ESP_LOGI(TAG, "system.lua recarregado pelo editor");
    }
    return ESP_OK;
}

void pda_config_reset_defaults(void)
{
    apply_defaults(&s_cfg);
    s_dirty = true;
    s_force_recreate = true;   /* "Padrão" + Salvar = arquivos recriados */
}

bool pda_settings_get(const char *key, double *out_num, bool *out_bool,
                      char *out_str, size_t str_sz)
{
    if (!key) return false;
    if (out_num) *out_num = 0;
    if (out_bool) *out_bool = false;
    if (out_str && str_sz) out_str[0] = '\0';

    if (!strcmp(key, "display.brightness")) { if (out_num) *out_num = s_cfg.brightness; return true; }
    if (!strcmp(key, "power.dim_after_s")) { if (out_num) *out_num = s_cfg.dim_after_s; return true; }
    if (!strcmp(key, "power.screen_off_after_s")) { if (out_num) *out_num = s_cfg.screen_off_after_s; return true; }
    if (!strcmp(key, "power.deep_sleep_after_s")) { if (out_num) *out_num = s_cfg.deep_sleep_after_s; return true; }
    if (!strcmp(key, "power.wake_on_touch")) { if (out_bool) *out_bool = s_cfg.wake_on_touch; return true; }
    if (!strcmp(key, "power.light_sleep")) { if (out_bool) *out_bool = s_cfg.light_sleep; return true; }
    if (!strcmp(key, "ui.onscreen_keyboard_auto")) { if (out_bool) *out_bool = s_cfg.onscreen_keyboard_auto; return true; }
    if (!strcmp(key, "locale.timezone")) { if (out_str) strlcpy(out_str, s_cfg.timezone, str_sz); return true; }
    if (!strcmp(key, "locale.ntp_server")) { if (out_str) strlcpy(out_str, s_cfg.ntp_server, str_sz); return true; }
    return false;
}

bool pda_settings_set(const char *key, double num, bool is_bool, bool bool_val, const char *str)
{
    if (!key) return false;
    if (!strcmp(key, "display.brightness")) { s_cfg.brightness = (int)num; }
    else if (!strcmp(key, "power.dim_after_s")) { s_cfg.dim_after_s = (int)num; }
    else if (!strcmp(key, "power.screen_off_after_s")) { s_cfg.screen_off_after_s = (int)num; }
    else if (!strcmp(key, "power.deep_sleep_after_s")) { s_cfg.deep_sleep_after_s = (int)num; }
    else if (!strcmp(key, "power.wake_on_touch")) { s_cfg.wake_on_touch = is_bool ? bool_val : (num != 0); }
    else if (!strcmp(key, "power.light_sleep")) { s_cfg.light_sleep = is_bool ? bool_val : (num != 0); }
    else if (!strcmp(key, "ui.onscreen_keyboard_auto")) { s_cfg.onscreen_keyboard_auto = is_bool ? bool_val : (num != 0); }
    else if (!strcmp(key, "locale.timezone")) { if (!str) return false; strlcpy(s_cfg.timezone, str, sizeof(s_cfg.timezone)); }
    else if (!strcmp(key, "locale.ntp_server")) { if (!str) return false; strlcpy(s_cfg.ntp_server, str, sizeof(s_cfg.ntp_server)); }
    else return false;
    clamp_all(&s_cfg);
    s_dirty = true;
    return true;
}
