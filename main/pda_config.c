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
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>      /* censo de entradas duplicadas (opendir/readdir) */
#include <errno.h>

static const char *TAG = "pda_config";

/* ---- guarda de ABI do espressif/lua (ver main/CMakeLists.txt) ----
 * A VM do componente é compilada com LUA_32BITS=1 (define PRIVATE lá):
 * lua_Number=float, lua_Integer=int32. Se o main compilar sem o mesmo
 * define, lua_tonumber cruza a fronteira float->double e devolve lixo —
 * foi exatamente assim que todo campo numérico do system.lua virou
 * INT_MAX nos boots de 2026-09-24..30 (parse "ok", cartão inocente).
 * Estes asserts falham o BUILD (não o boot) se a premissa mudar. */
#ifdef ESP_PLATFORM
_Static_assert(sizeof(lua_Number) == 4,
    "ABI espressif/lua: lua_Number tem que ter 4 bytes. Falta LUA_32BITS=1 "
    "no target_compile_definitions do main/CMakeLists.txt?");
_Static_assert(sizeof(lua_Integer) == 4,
    "ABI espressif/lua: lua_Integer tem que ter 4 bytes. Falta LUA_32BITS=1 "
    "no target_compile_definitions do main/CMakeLists.txt?");
#endif

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
    s->boot_btn_standby = false;   /* M4.14.4 */
    s->light_sleep = false;
    strlcpy(s->timezone, "America/Sao_Paulo", sizeof(s->timezone));
    strlcpy(s->ntp_server, "pool.ntp.org", sizeof(s->ntp_server));
    s->onscreen_keyboard_auto = true;
    strlcpy(s->cursor_style, "bar", sizeof(s->cursor_style));
    strlcpy(s->accent, "cyan", sizeof(s->accent));
    s->wifi_enabled = true;
}

/* ------------------------------------------------------------------ */
static int tbl_int(lua_State *L, int idx, const char *name, int def)
{
    lua_getfield(L, idx, name);
    int out = def;
    int conv = 0;
    /* Integer-first: lua_tointegerx converte inteiros E floats sem parte
     * fracionária sem nunca cruzar lua_Number pela fronteira C. É defesa
     * em profundidade contra o bug de ABI do espressif/lua (LUA_32BITS
     * PRIVATE — ver main/CMakeLists.txt): mesmo que o define suma, os
     * campos inteiros do system.lua continuam lendo certo. Só valores com
     * fração de verdade (ex.: "brightness = 42.7" editado à mão) caem no
     * lua_tonumber. */
    lua_Integer v = lua_tointegerx(L, -1, &conv);
    if (conv) {
        out = (int)v;
    } else if (lua_isnumber(L, -1)) {
        double d = lua_tonumber(L, -1);
        if (isfinite(d)) out = (int)d;   /* nan/inf não derrubam o boot */
    }
    lua_pop(L, 1);
    return out;
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

/* FNV-1a 32 bits: checksum barato p/ comparar duas leituras do mesmo
 * arquivo no mesmo boot (detecção de mídia mentirosa/instável). */
static uint32_t fnv1a(const char *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)data[i];
        h *= 16777619u;
    }
    return h;
}

/* Executa o chunk Lua já em memória e extrai as seções. Separado do
 * parse_file para o auto-teste poder rodá-lo sobre um buffer gerado na
 * hora (sem tocar no cartão). */
static esp_err_t parse_buffer(const char *name, const char *data, size_t len,
                              pda_settings_t *out)
{
    lua_State *L = luaL_newstate();
    if (!L) return ESP_ERR_NO_MEM;

    pda_settings_t tmp = *out; /* defaults como ponto de partida */
    esp_err_t err = ESP_OK;

    if (luaL_loadbuffer(L, data, len, name) != LUA_OK) {
        ESP_LOGE(TAG, "erro de sintaxe em %s: %s", name, lua_tostring(L, -1));
        err = ESP_FAIL;
        goto done;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "erro ao executar %s: %s", name, lua_tostring(L, -1));
        err = ESP_FAIL;
        goto done;
    }
    if (!lua_istable(L, -1)) {
        ESP_LOGE(TAG, "%s não retorna uma tabela (retornou %s)", name,
                 lua_typename(L, lua_type(L, -1)));
        err = ESP_FAIL;
        goto done;
    }

    /* Seção ausente/não-tabela é logada: se um dia o binário e o formato
     * divergirem (build velho, chave renomeada), o log diz QUAL seção o
     * parse não enxergou em vez de só devolver defaults silenciosamente. */

    /* display */
    lua_getfield(L, -1, "display");
    if (lua_istable(L, -1)) {
        tmp.brightness = tbl_int(L, -1, "brightness", tmp.brightness);
    } else {
        ESP_LOGW(TAG, "%s: seção 'display' ausente (%s)", name,
                 lua_typename(L, lua_type(L, -1)));
    }
    lua_pop(L, 1);

    /* net (M5.3) */
    lua_getfield(L, -1, "net");
    if (lua_istable(L, -1)) {
        tmp.wifi_enabled = tbl_bool(L, -1, "wifi_enabled", tmp.wifi_enabled);
    }
    lua_pop(L, 1);

    /* power */
    lua_getfield(L, -1, "power");
    if (lua_istable(L, -1)) {
        tmp.dim_after_s = tbl_int(L, -1, "dim_after_s", tmp.dim_after_s);
        tmp.screen_off_after_s = tbl_int(L, -1, "screen_off_after_s", tmp.screen_off_after_s);
        tmp.deep_sleep_after_s = tbl_int(L, -1, "deep_sleep_after_s", tmp.deep_sleep_after_s);
        tmp.wake_on_touch = tbl_bool(L, -1, "wake_on_touch", tmp.wake_on_touch);
        tmp.boot_btn_standby = tbl_bool(L, -1, "boot_btn_standby", tmp.boot_btn_standby);
        tmp.light_sleep = tbl_bool(L, -1, "light_sleep", tmp.light_sleep);
    } else {
        ESP_LOGW(TAG, "%s: seção 'power' ausente (%s)", name,
                 lua_typename(L, lua_type(L, -1)));
    }
    lua_pop(L, 1);

    /* locale */
    lua_getfield(L, -1, "locale");
    if (lua_istable(L, -1)) {
        tbl_str(L, -1, "timezone", tmp.timezone, sizeof(tmp.timezone), tmp.timezone);
        tbl_str(L, -1, "ntp_server", tmp.ntp_server, sizeof(tmp.ntp_server), tmp.ntp_server);
    } else {
        ESP_LOGW(TAG, "%s: seção 'locale' ausente (%s)", name,
                 lua_typename(L, lua_type(L, -1)));
    }
    lua_pop(L, 1);

    /* ui */
    lua_getfield(L, -1, "ui");
    if (lua_istable(L, -1)) {
        tmp.onscreen_keyboard_auto = tbl_bool(L, -1, "onscreen_keyboard_auto", tmp.onscreen_keyboard_auto);
        tbl_str(L, -1, "cursor", tmp.cursor_style, sizeof(tmp.cursor_style), tmp.cursor_style);
        tbl_str(L, -1, "accent", tmp.accent, sizeof(tmp.accent), tmp.accent);
        /* sanita M5.0: acento fora da lista vira cyan */
        if (strcmp(tmp.accent,"cyan")&&strcmp(tmp.accent,"violet")&&strcmp(tmp.accent,"green")&&strcmp(tmp.accent,"amber")&&strcmp(tmp.accent,"pink")) strlcpy(tmp.accent,"cyan",sizeof(tmp.accent));
    } else {
        ESP_LOGW(TAG, "%s: seção 'ui' ausente (%s)", name,
                 lua_typename(L, lua_type(L, -1)));
    }
    lua_pop(L, 1);

    *out = tmp;

done:
    lua_close(L);
    return err;
}

static esp_err_t parse_file(const char *path, pda_settings_t *out)
{
    /* Parse a partir da MEMÓRIA, não do fopen interno do luaL_loadfile.
     * Boot de 2026-09-30: o dump (storage_read_file_alloc) mostrou conteúdo
     * VÁLIDO no cartão ("brightness = 30"), mas o parse via loadfile devolveu
     * INT_MAX em todos os campos com parse=ok — ou seja, o fopen do Lua viu
     * bytes diferentes dos que a auditoria viu (entrada duplicada velha no
     * FAT? leitura instável?). Lendo uma vez só e parseando o buffer, essa
     * divergência deixa de existir: o parse enxerga EXATAMENTE os bytes
     * auditados. (save_persists já usava loadbuffer e nunca falhou em campo.) */
    char *data = NULL;
    size_t len = 0;
    esp_err_t e = storage_read_file_alloc(path, &data, &len);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "leitura de %s falhou (%s)", path, esp_err_to_name(e));
        return e;
    }
    uint32_t c1 = fnv1a(data, len);
    esp_err_t err = parse_buffer(path, data, len, out);

    /* Segunda leitura IMEDIATA: mesmo path, mesmo boot, nenhuma escrita no
     * meio. Checksum diferente = a mídia está mentindo (cartão falso/cansado,
     * setor de diretório instável) — a evidência que faltava p/ separar
     * "mídia instável" de "parse bugado". */
    char *d2 = NULL;
    size_t l2 = 0;
    if (storage_read_file_alloc(path, &d2, &l2) == ESP_OK) {
        uint32_t c2 = fnv1a(d2, l2);
        if (c2 != c1 || l2 != len)
            ESP_LOGE(TAG, "MÍDIA INSTÁVEL: %s mudou entre 2 leituras "
                     "(%u B fnv=%08x -> %u B fnv=%08x) — cartão suspeito",
                     path, (unsigned)len, (unsigned)c1, (unsigned)l2, (unsigned)c2);
        free(d2);
    }
    ESP_LOGI(TAG, "parse %s: %u B fnv=%08x -> br=%d dim=%d off=%d deep=%d",
             path, (unsigned)len, (unsigned)c1,
             out->brightness, out->dim_after_s,
             out->screen_off_after_s, out->deep_sleep_after_s);
    free(data);
    return err;
}

/* ------------------------------------------------------------------ */
/* Censo de diretório: o FAT desta placa JÁ acumulou entradas duplicadas
 * com o MESMO nome (o open resolve a antiga, o create anexa no fim).
 * readdir() do VFS enumera TODAS as entradas do diretório (o esp_vfs_fat
 * passa pelo f_readdir), em ambas as raízes (FAT e LittleFS) — a versão
 * anterior chamava f_opendir() com o path do VFS ("/sdcard/...") e falhava
 * SEMPRE, então o censo nunca rodou (boot de 2026-09-29). Com dirent
 * funcionamos nas duas raízes e sem depender de headers internos do fatfs. */
static void census_dir(const char *dirpath, const char *name)
{
    DIR *d = opendir(dirpath);
    if (!d) {
        ESP_LOGE(TAG, "censo %s: opendir falhou (%s)", dirpath, strerror(errno));
        return;
    }
    int hits = 0, total = 0;
    struct dirent *e;
    char full[224];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
            continue;
        total++;
        if (strcmp(e->d_name, name) != 0) continue;
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", dirpath, name);
        long sz = -1, mt = 0;
        if (stat(full, &st) == 0) { sz = (long)st.st_size; mt = (long)st.st_mtime; }
        ESP_LOGW(TAG, "censo %s/%s: entrada #%d size=%ld mtime=%ld",
                 dirpath, name, hits, sz, mt);
        hits++;
    }
    closedir(d);
    if (hits > 1)
        ESP_LOGE(TAG, "censo %s: %d ENTRADAS com o nome '%s' — duplicadas confirmado",
                 dirpath, hits, name);
    else
        ESP_LOGI(TAG, "censo %s: '%s' tem %d entrada(s) (dir com %d itens)",
                 dirpath, name, hits, total);
}

/* Conteúdo do arquivo quando os valores vêm insanos: é a evidência que
 * separa "o arquivo no meio físico realmente tem 2147483647" (escrita que
 * não persiste / escritor externo) de "o parse é que enlouqueceu". Loga os
 * primeiros bytes como uma linha sanitizada (\n -> '|'). */
static void dump_head(const char *path, size_t maxn)
{
    char *data = NULL;
    size_t len = 0;
    if (storage_read_file_alloc(path, &data, &len) != ESP_OK) {
        ESP_LOGW(TAG, "dump %s: leitura falhou", path);
        return;
    }
    size_t n = len < maxn ? len : maxn;
    char line[161];
    for (size_t chunk = 0; chunk < n; chunk += sizeof(line) - 1) {
        size_t m = n - chunk;
        if (m > sizeof(line) - 1) m = sizeof(line) - 1;
        for (size_t i = 0; i < m; i++) {
            unsigned char c = (unsigned char)data[chunk + i];
            line[i] = (c == '\n' || c == '\r') ? '|' : (c >= 0x20 && c < 0x7f ? (char)c : '?');
        }
        line[m] = 0;
        ESP_LOGW(TAG, "dump %s +%u: %s", path, (unsigned)chunk, line);
    }
    ESP_LOGW(TAG, "dump %s: total %u B", path, (unsigned)len);
    free(data);
}

/* Apaga TODAS as entradas com este nome (o FAT pode acumular duplicatas
 * viciadas; storage_delete_file remove só a primeira que ele resolve). */
static int purge_path(const char *path)
{
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (!storage_file_exists(path)) break;
        esp_err_t e = storage_delete_file(path);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "purge: delete de %s falhou (%s) — entrada viciada PODE sobrar",
                     path, esp_err_to_name(e));
            break;
        }
        n++;
    }
    if (storage_file_exists(path))
        ESP_LOGE(TAG, "purge: %s AINDA existe após %d delete(s)", path, n);
    else if (n > 0)
        ESP_LOGW(TAG, "purge: %s — %d entrada(s) apagada(s)", path, n);
    return n;
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
    nvs_set_u8(h, "bbtn", s->boot_btn_standby ? 1 : 0);   /* M4.14.4 */
    nvs_set_u8(h, "osk", s->onscreen_keyboard_auto ? 1 : 0);
    nvs_set_u8(h, "ls", s->light_sleep ? 1 : 0);
    nvs_set_str(h, "tz", s->timezone);
    nvs_set_str(h, "ntp", s->ntp_server);
    nvs_set_str(h, "cursor", s->cursor_style);   /* M4.11b: sem isto a cura
                                                  * pelo NVS apagava o cursor */
    nvs_set_str(h, "accent", s->accent);
    nvs_set_u8(h, "wifion", s->wifi_enabled ? 1 : 0);   /* M5.3 */
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
    if (nvs_get_u8(h, "bbtn", &b) == ESP_OK) s->boot_btn_standby = b != 0;
    if (nvs_get_u8(h, "osk", &b) == ESP_OK) s->onscreen_keyboard_auto = b != 0;
    if (nvs_get_u8(h, "ls", &b) == ESP_OK) s->light_sleep = b != 0;
    sz = sizeof(s->timezone);
    nvs_get_str(h, "tz", s->timezone, &sz);
    sz = sizeof(s->ntp_server);
    nvs_get_str(h, "ntp", s->ntp_server, &sz);
    /* cursor é opcional na sombra (NVS de firmware antigo não tem a chave):
     * ausente -> mantém o valor corrente em vez de marcar a sombra inválida. */
    sz = sizeof(s->cursor_style);
    nvs_get_str(h, "cursor", s->cursor_style, &sz);
    sz = sizeof(s->accent);
    nvs_get_str(h, "accent", s->accent, &sz);
    if (strcmp(s->accent,"cyan")&&strcmp(s->accent,"violet")&&strcmp(s->accent,"green")&&strcmp(s->accent,"amber")&&strcmp(s->accent,"pink")) strlcpy(s->accent,"cyan",sizeof(s->accent));
    if (nvs_get_u8(h, "wifion", &b) == ESP_OK) s->wifi_enabled = b != 0;
    nvs_close(h);
    return ok;
}

/* Formata as configurações como chunk Lua em memória. Retorna o nº de
 * bytes ou -1 se não couber. Usado por serialize_to (arquivo) e pelo
 * auto-teste do parser (só RAM). */
static int serialize_buf(char *buf, size_t sz, const pda_settings_t *s)
{
    int n = snprintf(buf, sz,
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
        "    boot_btn_standby = %s, -- M4.14.4: BOOT em ACTIVE/DIM pede standby\n"
        "    light_sleep = %s,       -- true: light sleep no standby (exp.)\n"
        "  },\n"
        "  locale = {\n"
        "    timezone = \"%s\",\n"
        "    ntp_server = \"%s\",\n"
        "  },\n"
        "  net = {\n"
        "    wifi_enabled = %s,    -- M5.3: radio on/off pela tela Config\n"
        "  },\n"
        "  ui = {\n"
        "    onscreen_keyboard_auto = %s, -- teclado virtual so sem teclado USB\n"
        "    cursor = \"%s\",             -- cursor do editor: bar | under | block\n"
        "    accent = \"%s\",             -- acento M3 Expressive: cyan|violet|green|amber|pink\n"
        "  },\n"
        "}\n",
        s->brightness,
        s->dim_after_s, s->screen_off_after_s, s->deep_sleep_after_s,
        s->wake_on_touch ? "true" : "false",
        s->boot_btn_standby ? "true" : "false",
        s->light_sleep ? "true" : "false",
        s->timezone, s->ntp_server,
        s->wifi_enabled ? "true" : "false",
        s->onscreen_keyboard_auto ? "true" : "false",
        s->cursor_style, s->accent);
    if (n <= 0 || (size_t)n >= sz) return -1;
    return n;
}

static esp_err_t serialize_to(const char *path, const pda_settings_t *s)
{
    char buf[1536];   /* M5.3: seção net entrou; 1024 ficava no limite */
    int n = serialize_buf(buf, sizeof(buf), s);
    if (n < 0) return ESP_ERR_INVALID_SIZE;
    return storage_write_text_file(path, buf, (size_t)n);
}

/* Auto-teste do parser: serializa as configurações atuais em RAM e as
 * re-parseia. Prova que o BINÁRIO EM EXECUÇÃO lê o formato que ele mesmo
 * grava — separa "parse quebrado/antigo no build" de "conteúdo insano na
 * mídia" (mistério do boot 2026-09-30: dump mostrou conteúdo válido com
 * brightness = 30, mas todos os campos voltaram INT_MAX com parse=ok). */
static bool parser_selftest(const pda_settings_t *s)
{
    char buf[1536];   /* M5.3: mesmo teto do serialize_to */
    int n = serialize_buf(buf, sizeof(buf), s);
    if (n < 0) return false;
    pda_settings_t rt = *s;
    rt.brightness = -12345;          /* perturba os valores: só podem voltar */
    rt.dim_after_s = -12345;         /* do buffer se o parse funcionar de */
    rt.screen_off_after_s = -12345;  /* verdade (defaults diferentes) */
    rt.deep_sleep_after_s = -12345;
    if (parse_buffer("selftest", buf, (size_t)n, &rt) != ESP_OK) {
        ESP_LOGE(TAG, "selftest: parse_buffer devolveu erro (sintaxe/tabela?)");
        return false;
    }
    bool ok = rt.brightness == s->brightness &&
              rt.dim_after_s == s->dim_after_s &&
              rt.screen_off_after_s == s->screen_off_after_s &&
              rt.deep_sleep_after_s == s->deep_sleep_after_s &&
              rt.wake_on_touch == s->wake_on_touch &&
              rt.light_sleep == s->light_sleep &&
              rt.onscreen_keyboard_auto == s->onscreen_keyboard_auto &&
              strcmp(rt.timezone, s->timezone) == 0 &&
              strcmp(rt.ntp_server, s->ntp_server) == 0 &&
              strcmp(rt.cursor_style, s->cursor_style) == 0 &&
              strcmp(rt.accent, s->accent) == 0 &&
              rt.wifi_enabled == s->wifi_enabled;
    if (!ok) {
        /* M4.11b: dizia só "FALHOU" — no boot de 2026-09-30 isso escondeu
         * que TODOS os numéricos voltavam INT_MAX (ABI LUA_32BITS). Loga os
         * dois lados: -12345 intacto = parse não leu; INT_MAX = ABI/float. */
        ESP_LOGE(TAG, "selftest DIVERGIU: veio br=%d dim=%d off=%d deep=%d "
                 "wake=%d ls=%d osk=%d cursor=%s acc=%s | esperado br=%d dim=%d "
                 "off=%d deep=%d wake=%d ls=%d osk=%d cursor=%s acc=%s",
                 rt.brightness, rt.dim_after_s, rt.screen_off_after_s,
                 rt.deep_sleep_after_s, rt.wake_on_touch, rt.light_sleep,
                 rt.onscreen_keyboard_auto, rt.cursor_style, rt.accent,
                 s->brightness, s->dim_after_s, s->screen_off_after_s,
                 s->deep_sleep_after_s, s->wake_on_touch, s->light_sleep,
                 s->onscreen_keyboard_auto, s->cursor_style, s->accent);
    }
    return ok;
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

    /* Primeiro de tudo: prova que este binário parseia o formato que grava.
     * Se FALHAR aqui, o resto do diagnóstico de mídia é irrelevante. */
    if (parser_selftest(&s_cfg))
        ESP_LOGI(TAG, "parser self-test: OK (binário lê o formato que grava)");
    else
        ESP_LOGE(TAG, "parser self-test: FALHOU — parser quebrado no build?!");

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
    /* Evidência p/ o mistério do system.lua que "volta" insano: quantas
     * entradas o diretório tem e qual a data de cada uma. mtime=1980-01-01
     * = entrada nunca escrita por firmware novo (assinatura de duplicata). */
    census_dir(storage_sd_mounted() ? "/sdcard/pda/config" : "/internal/pda/config",
               "system.lua");
    esp_err_t err = parse_file(path, &s_cfg);
    bool bad = (err != ESP_OK) || clamp_all(&s_cfg);
    if (bad) {
        ESP_LOGW(TAG, "raiz ATIVA %s insana (parse=%s); conteúdo abaixo", path,
                 err == ESP_OK ? "ok" : "erro");
        dump_head(path, 320);
    }
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
        census_dir(storage_sd_mounted() ? "/sdcard/pda/config" : "/internal/pda/config",
                   "system.lua");
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
    if (!strcmp(key, "ui.cursor")) { if (out_str) strlcpy(out_str, s_cfg.cursor_style, str_sz); return true; }
    if (!strcmp(key, "ui.accent")) { if (out_str) strlcpy(out_str, s_cfg.accent, str_sz); return true; }
    if (!strcmp(key, "net.wifi_enabled")) { if (out_bool) *out_bool = s_cfg.wifi_enabled; return true; }
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
    else if (!strcmp(key, "ui.cursor")) { if (!str) return false; strlcpy(s_cfg.cursor_style, str, sizeof(s_cfg.cursor_style)); }
    else if (!strcmp(key, "ui.accent")) { if (!str) return false; strlcpy(s_cfg.accent, str, sizeof(s_cfg.accent)); }
    else if (!strcmp(key, "net.wifi_enabled")) { if (!is_bool) return false; s_cfg.wifi_enabled = bool_val; }
    else if (!strcmp(key, "locale.timezone")) { if (!str) return false; strlcpy(s_cfg.timezone, str, sizeof(s_cfg.timezone)); }
    else if (!strcmp(key, "locale.ntp_server")) { if (!str) return false; strlcpy(s_cfg.ntp_server, str, sizeof(s_cfg.ntp_server)); }
    else return false;
    clamp_all(&s_cfg);
    s_dirty = true;
    return true;
}
