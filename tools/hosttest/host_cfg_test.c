/* host_cfg_test.c — testa o pda_config.c REAL no host (x86 + Lua 5.4).
 *
 * Compilar (ver tools/hosttest/run.sh):
 *   gcc -std=gnu11 -Wall -Wextra -I tools/hosttest/stubs -I main \
 *       -I /usr/include/lua5.4 main/pda_config.c tools/hosttest/host_cfg_test.c \
 *       -llua5.4 -lm -o /tmp/cfgtest
 *
 * Cenários (espelham o mistério do boot 2026-09-30):
 *   T1 cartão vazio  -> cria defaults; 2º init parseia "intacto"
 *   T2 valores do usuário -> save + "reboot" -> round-trip exato (30/30/120/0)
 *   T3 arquivo com INT_MAX literal -> sanitize + recuperação (espelho/NVS)
 *   T4 arquivo com sintaxe inválida -> parse=erro + recuperação
 *   T5 mídia instável (2ª leitura difere) -> log "MÍDIA INSTÁVEL"
 *   T6 tbl_int integer-first: 42.7 -> 42, 42.0 -> 42, "55" -> 55
 *   T7 ui.cursor round-trip pelo arquivo (bug M4.11b)
 *   T8 sombra NVS devolve brightness E cursor com as duas raízes podres
 *
 * Os stubs de storage usam os caminhos REAIS ("/sdcard/...", "/internal/..."):
 * run.sh cria symlinks p/ /tmp/cfgtest, então stat/opendir do censo também
 * funcionam de verdade.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pda_config.h"
#include "storage_init.h"
#include "nvs.h"

/* ---------------- strlcpy (glibc < 2.38 não tem) ---------------- */
#ifndef HAVE_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t sz)
{
    size_t n = strlen(src);
    if (sz) {
        size_t c = n < sz - 1 ? n : sz - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}
#endif

/* ---------------- stub storage (arquivos reais em /sdcard, /internal) ---- */
int hosttest_flaky_reads = 0;
static int s_read_calls = 0;

static void mkdir_p(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    }
}

static void parent_dir(const char *path, char *out, size_t sz)
{
    snprintf(out, sz, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash && slash != out) *slash = 0;
}

bool storage_sd_mounted(void) { return true; }

esp_err_t pda_path(char *out, size_t out_sz, const char *rel)
{
    snprintf(out, out_sz, "/sdcard/pda/%s", rel);
    return ESP_OK;
}

esp_err_t storage_read_file_alloc(const char *path, char **out_data, size_t *out_len)
{
    s_read_calls++;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = 0;
    /* modo instável: leituras PARES voltam corrompidas (conteúdo diferente) */
    if (hosttest_flaky_reads && (s_read_calls % 2) == 0) {
        buf = realloc(buf, rd + 2);
        buf[rd] = 'X';
        buf[rd + 1] = 0;
        rd++;
    }
    *out_data = buf;
    *out_len = rd;
    return ESP_OK;
}

esp_err_t storage_write_text_file(const char *path, const char *data, size_t len)
{
    char dir[256];
    parent_dir(path, dir, sizeof(dir));
    mkdir_p(dir);
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    fwrite(data, 1, len, f);
    fclose(f);
    return ESP_OK;
}

esp_err_t storage_delete_file(const char *path)
{
    return remove(path) == 0 ? ESP_OK : ESP_FAIL;
}

bool storage_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

esp_err_t storage_copy_file(const char *src, const char *dst)
{
    char *data = NULL;
    size_t len = 0;
    if (storage_read_file_alloc(src, &data, &len) != ESP_OK) return ESP_FAIL;
    esp_err_t e = storage_write_text_file(dst, data, len);
    free(data);
    return e;
}

/* ---------------- stub NVS (memória, sobrevive entre "boots" do teste) --- */
static struct {
    int has_br, has_dim, has_off, has_deep, has_wake, has_osk, has_ls, has_tz, has_ntp, has_cursor;
    int32_t br, dim, off, deep;
    uint8_t wake, osk, ls;
    char tz[64], ntp[64], cursor[32];
} s_nvs;

esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h)
{
    (void)m;
    if (strcmp(ns, "pdacfg") != 0) return ESP_ERR_NOT_FOUND;
    *h = 1;
    return ESP_OK;
}
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v)
{
    (void)h;
    if (!strcmp(k, "br"))   { s_nvs.br = v;   s_nvs.has_br = 1; }
    if (!strcmp(k, "dim"))  { s_nvs.dim = v;  s_nvs.has_dim = 1; }
    if (!strcmp(k, "off"))  { s_nvs.off = v;  s_nvs.has_off = 1; }
    if (!strcmp(k, "deep")) { s_nvs.deep = v; s_nvs.has_deep = 1; }
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)
{
    (void)h;
    if (!strcmp(k, "wake")) { s_nvs.wake = v; s_nvs.has_wake = 1; }
    if (!strcmp(k, "osk"))  { s_nvs.osk = v;  s_nvs.has_osk = 1; }
    if (!strcmp(k, "ls"))   { s_nvs.ls = v;   s_nvs.has_ls = 1; }
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{
    (void)h;
    if (!strcmp(k, "tz"))  { strlcpy(s_nvs.tz, v, sizeof(s_nvs.tz));   s_nvs.has_tz = 1; }
    if (!strcmp(k, "ntp")) { strlcpy(s_nvs.ntp, v, sizeof(s_nvs.ntp)); s_nvs.has_ntp = 1; }
    if (!strcmp(k, "cursor")) { strlcpy(s_nvs.cursor, v, sizeof(s_nvs.cursor)); s_nvs.has_cursor = 1; }
    return ESP_OK;
}
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v)
{
    (void)h;
    if (!strcmp(k, "br") && s_nvs.has_br)     { *v = s_nvs.br;   return ESP_OK; }
    if (!strcmp(k, "dim") && s_nvs.has_dim)   { *v = s_nvs.dim;  return ESP_OK; }
    if (!strcmp(k, "off") && s_nvs.has_off)   { *v = s_nvs.off;  return ESP_OK; }
    if (!strcmp(k, "deep") && s_nvs.has_deep) { *v = s_nvs.deep; return ESP_OK; }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{
    (void)h;
    if (!strcmp(k, "wake") && s_nvs.has_wake) { *v = s_nvs.wake; return ESP_OK; }
    if (!strcmp(k, "osk") && s_nvs.has_osk)   { *v = s_nvs.osk;  return ESP_OK; }
    if (!strcmp(k, "ls") && s_nvs.has_ls)     { *v = s_nvs.ls;   return ESP_OK; }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *len)
{
    (void)h;
    const char *src = NULL;
    if (!strcmp(k, "tz") && s_nvs.has_tz)   src = s_nvs.tz;
    if (!strcmp(k, "ntp") && s_nvs.has_ntp) src = s_nvs.ntp;
    if (!strcmp(k, "cursor") && s_nvs.has_cursor) src = s_nvs.cursor;
    if (!src) return ESP_ERR_NOT_FOUND;
    strlcpy(v, src, *len);
    *len = strlen(src) + 1;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

/* ---------------- harness de asserções ---------------- */
static int s_fail = 0;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) s_fail++;
}

static void write_raw(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fputs(content, f);
    fclose(f);
}

int main(void)
{
    const char *syslua = "/sdcard/pda/config/system.lua";

    /* ---- T1: cartão vazio ---- */
    printf("== T1: cartão vazio -> defaults + 2º boot intacto ==\n");
    pda_config_init();
    const pda_settings_t *s = pda_settings();
    check(s->brightness == 80 && s->dim_after_s == 30 &&
          s->screen_off_after_s == 120 && s->deep_sleep_after_s == 600,
          "defaults criados (80/30/120/600)");
    check(storage_file_exists(syslua), "system.lua criado");
    pda_config_init();   /* "reboot" */
    s = pda_settings();
    check(s->brightness == 80, "2º boot reparseia o arquivo (sem sanitize)");

    /* ---- T2: valores de usuário round-trip (o cenário de campo) ---- */
    printf("== T2: round-trip 30/30/120/0 + wake/ls ==\n");
    pda_settings_set("display.brightness", 30, false, false, NULL);
    pda_settings_set("power.dim_after_s", 30, false, false, NULL);
    pda_settings_set("power.screen_off_after_s", 120, false, false, NULL);
    pda_settings_set("power.deep_sleep_after_s", 0, false, false, NULL);
    pda_settings_set("power.wake_on_touch", 0, true, true, NULL);
    pda_settings_set("power.light_sleep", 0, true, true, NULL);
    pda_config_save();
    pda_config_init();   /* "reboot" */
    s = pda_settings();
    check(s->brightness == 30 && s->dim_after_s == 30 &&
          s->screen_off_after_s == 120 && s->deep_sleep_after_s == 0,
          "arquivo gravado volta exato no boot seguinte (30/30/120/0)");
    check(s->wake_on_touch && s->light_sleep, "bools round-trip");

    /* ---- T3: arquivo com INT_MAX literal (o veneno histórico) ---- */
    printf("== T3: INT_MAX literal -> sanitize + recuperação ==\n");
    write_raw(syslua,
        "return {\n"
        "  display = { brightness = 2147483647, },\n"
        "  power = { dim_after_s = 2147483647, screen_off_after_s = 2147483647,"
        " deep_sleep_after_s = 2147483647, wake_on_touch = true, light_sleep = true, },\n"
        "  locale = { timezone = \"America/Sao_Paulo\", ntp_server = \"pool.ntp.org\", },\n"
        "  ui = { onscreen_keyboard_auto = true, cursor = \"bar\", },\n"
        "}\n");
    pda_config_init();
    s = pda_settings();
    check(s->brightness == 30 && s->dim_after_s == 30 &&
          s->screen_off_after_s == 120 && s->deep_sleep_after_s == 0,
          "INT_MAX no arquivo -> recuperado do espelho/NVS (30/30/120/0)");
    pda_config_init();
    s = pda_settings();
    check(s->brightness == 30, "pós-cura: boot seguinte intacto");

    /* ---- T4: sintaxe inválida ---- */
    printf("== T4: arquivo corrompido (sintaxe) -> recuperação ==\n");
    write_raw(syslua, "return {{{ isto não é lua\n");
    pda_config_init();
    s = pda_settings();
    check(s->brightness == 30, "sintaxe inválida -> espelho/NVS mantém 30");

    /* ---- T5: mídia instável (2ª leitura divergente) ---- */
    printf("== T5: mídia instável -> espera-se log 'MÍDIA INSTÁVEL' acima ==\n");
    pda_config_init();          /* arquivo bom reescrito pela cura do T4 */
    hosttest_flaky_reads = 1;
    pda_config_init();
    hosttest_flaky_reads = 0;
    check(1, "T5 roda sem crash (verificar a linha E no output)");

    /* ---- T6: números com fração/float/string (tbl_int integer-first) ---- */
    printf("== T6: brightness 42.7 / 42.0 / \"55\" -> 42 / 42 / 55 ==\n");
    write_raw(syslua,
        "return {\n"
        "  display = { brightness = 42.7, },\n"
        "  power = { dim_after_s = 5, screen_off_after_s = 120,"
        " deep_sleep_after_s = 0, },\n"
        "}\n");
    pda_config_init();
    s = pda_settings();
    check(s->brightness == 42, "float fracionado 42.7 -> 42 (fallback tonumber)");

    write_raw(syslua,
        "return {\n"
        "  display = { brightness = 42.0, },\n"
        "  power = { dim_after_s = 5, screen_off_after_s = 120,"
        " deep_sleep_after_s = 0, },\n"
        "}\n");
    pda_config_init();
    s = pda_settings();
    check(s->brightness == 42, "float inteiro 42.0 -> 42 (caminho tointegerx)");

    write_raw(syslua,
        "return {\n"
        "  display = { brightness = \"55\", },\n"
        "  power = { dim_after_s = 5, screen_off_after_s = 120,"
        " deep_sleep_after_s = 0, },\n"
        "}\n");
    pda_config_init();
    s = pda_settings();
    check(s->brightness == 55, "string numérica \"55\" -> 55 (coerção mantida)");

    /* ---- T7: cursor round-trip pelo ARQUIVO (bug: não persistia) ---- */
    printf("== T7: ui.cursor=block -> save -> reboot -> block ==\n");
    pda_settings_set("ui.cursor", 0, false, false, "block");
    pda_settings_set("display.brightness", 25, false, false, NULL);
    pda_config_save();
    pda_config_init();
    s = pda_settings();
    check(strcmp(s->cursor_style, "block") == 0,
          "cursor volta 'block' do arquivo no boot seguinte");
    check(s->brightness == 25, "brightness 25 sobrevive junto");

    /* ---- T8: raízes podres -> sombra NVS carrega o cursor ---- */
    printf("== T8: ativo+espelho podres -> sombra NVS traz br E cursor ==\n");
    write_raw(syslua, "return {{{ podre\n");
    write_raw("/internal/pda/config/system.lua", "return }}} podre tambem\n");
    pda_config_init();
    s = pda_settings();
    check(s->brightness == 25, "sombra NVS devolve brightness 25");
    check(strcmp(s->cursor_style, "block") == 0,
          "sombra NVS devolve cursor block (M4.11b: chave 'cursor' no NVS)");

    printf("\n%s (%d falha(s))\n", s_fail ? "FALHOU" : "TUDO PASSOU", s_fail);
    return s_fail ? 1 : 0;
}
