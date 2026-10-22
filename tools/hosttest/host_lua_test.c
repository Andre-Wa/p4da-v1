/* host_lua_test.c — LUA-BIND (v5.16.2, logger de campo BAT.1b).
 *
 * Linka o lua_runtime.c REAL ao Lua 5.4 do host (lua55_shim.h casa o
 * lua_newstate de 3 args do Lua 5.5 do alvo) com stubs: bateria dirigida
 * por cenário (o passo avança a cada read()), storage = stdio real em
 * /tmp/luabind, relógio que avança +2 s por leitura (o sleep é no-op).
 *
 * As seções D–F rodam o script REAL
 * sdcard-template/pda/scripts/log_bateria.lua de ponta a ponta e afirmam o
 * CSV que nasce: header único, linha da janela, colunas mv/pct/estado
 * iguais ao cenário, NÃO para no "carregando" inicial (antes de ver a
 * bateria), para quando o carregador volta, continua arquivo existente sem
 * duplicar header e falha graciosamente quando não há onde gravar.
 * Disciplina REGRESSOES_BUILD.md: lógica pura nasce com teste de host —
 * aqui nasce testada até a integração C↔Lua↔arquivo.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lua_runtime.h"

#define ROOT   "/tmp/luabind"
#define SCRIPT "sdcard-template/pda/scripts/log_bateria.lua"
#define CSV    ROOT "/logs/bateria.csv"
#define HEADER "t_min;uptime_s;hora;mv;pct;estado"

static int fails;
#define CHECK(cond, fmt, ...)                                   \
    do {                                                        \
        if (!(cond)) {                                          \
            fails++;                                            \
            printf("FAIL: " fmt "\n", ##__VA_ARGS__);           \
        }                                                       \
    } while (0)

/* ---------------- strlcpy (glibc < 2.38 não tem; mesmo shim do
 * host_cfg_test.c — binários separados, sem colisão) ---------------- */
size_t strlcpy(char *dst, const char *src, size_t sz)
{
    size_t n = strlen(src);
    if (sz) {
        size_t c = n >= sz ? sz - 1 : n;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

/* ---------------- relógio falso: +2 s por leitura ------------------ */
static int64_t s_clk_us = 1000000;
int64_t esp_timer_get_time(void)
{
    int64_t t = s_clk_us;
    s_clk_us += 2000000;
    return t;
}
uint32_t esp_random(void) { return 0x5EED5EEDu; }

/* ---------------- bateria: cenário por passos ----------------------
 * A binding chama battery_mv/percent/state_str em sequência: o passo
 * avança no state_str (última das três) para as três leituras de um
 * pda.battery.read() saírem do MESMO passo. */
typedef struct { int mv; int pct; const char *st; } bstep;
static bstep s_scen[16];
static int s_scen_n, s_scen_i;

static void scen(const bstep *s, int n)
{
    for (int i = 0; i < n; i++) s_scen[i] = s[i];
    s_scen_n = n;
    s_scen_i = 0;
}
int battery_mv(void) { return s_scen[s_scen_i].mv; }
int battery_percent(void) { return s_scen[s_scen_i].pct; }
const char *battery_state_str(void)
{
    const char *s = s_scen[s_scen_i].st;
    if (s_scen_i < s_scen_n - 1) s_scen_i++;
    return s;
}
int battery_anchor_min(void) { return 3600; }
int battery_anchor_max(void) { return 3700; }
bool battery_anchors_manual(void) { return true; }
bool battery_set_anchors(int lo, int hi) { (void)lo; (void)hi; return true; }
void battery_reset_calibration(void) {}

/* ---------------- storage: stdio real em /tmp/luabind -------------- */
static char g_mkdir_last[192];
static int g_mkdir_calls, g_mkdir_fail;

const char *pda_root(void) { return ROOT; }
esp_err_t pda_path(char *out, size_t sz, const char *rel)
{
    int n = snprintf(out, sz, "%s/%s", ROOT, rel);
    return (n > 0 && (size_t)n < sz) ? ESP_OK : ESP_FAIL;
}
esp_err_t storage_ensure_dir(const char *p)
{
    g_mkdir_calls++;
    snprintf(g_mkdir_last, sizeof(g_mkdir_last), "%s", p);
    if (g_mkdir_fail) return ESP_FAIL;
    if (mkdir(p, 0755) != 0 && errno != EEXIST) return ESP_FAIL;
    return ESP_OK;
}
bool storage_sd_mounted(void) { return true; }
bool storage_file_exists(const char *p)
{ struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
bool storage_is_dir(const char *p)
{ struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
int64_t storage_file_size(const char *p)
{ struct stat st; return stat(p, &st) == 0 ? (int64_t)st.st_size : -1; }
esp_err_t storage_read_file_alloc(const char *p, char **out, size_t *len)
{
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_FAIL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1);
    if (!b) { fclose(f); return ESP_FAIL; }
    size_t rd = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[rd] = '\0';
    *out = b;
    *len = rd;
    return ESP_OK;
}
esp_err_t storage_delete_file(const char *p)
{ return remove(p) == 0 ? ESP_OK : ESP_FAIL; }

/* ---------------- pda_config: dummies (script não usa) ------------- */
bool pda_settings_get(const char *k, double *n, bool *b, char *s, size_t sz)
{ (void)k; (void)n; (void)b; (void)s; (void)sz; return false; }
bool pda_settings_set(const char *k, double n, bool ib, bool b, const char *s)
{ (void)k; (void)n; (void)ib; (void)b; (void)s; return false; }
esp_err_t pda_config_save(void) { return ESP_OK; }

/* ---------------- captura do pda.log ------------------------------- */
static char g_log[16384];
static size_t g_logpos;
static void cap_log(const char *line, void *ctx)
{
    (void)ctx;
    int w = snprintf(g_log + g_logpos, sizeof(g_log) - g_logpos, "%s\n", line);
    if (w > 0 && g_logpos + (size_t)w < sizeof(g_log)) g_logpos += (size_t)w;
}
static void cap_reset(void) { g_logpos = 0; g_log[0] = '\0'; }
static int cap_has(const char *s) { return strstr(g_log, s) != NULL; }

/* ---------------- helpers de CSV ----------------------------------- */
typedef struct { int tmin, ut, mv, pct; char hora[24], est[24]; } csv_row;

static int csv_load(const char *path, char lines[][256], int max)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = 0;
    while (n < max && fgets(lines[n], 256, f)) {
        size_t l = strlen(lines[n]);
        while (l && (lines[n][l - 1] == '\n' || lines[n][l - 1] == '\r'))
            lines[n][--l] = '\0';
        if (l) n++;
    }
    fclose(f);
    return n;
}
static int parse_row(const char *l, csv_row *r)
{
    return sscanf(l, "%d;%d;%23[^;];%d;%d;%23s",
                  &r->tmin, &r->ut, r->hora, &r->mv, &r->pct, r->est) == 6;
}
static int hora_ok(const char *h)
{
    if (!strcmp(h, "-")) return 1;              /* sem NTP */
    if (strlen(h) != 8 || h[2] != ':' || h[5] != ':') return 0;
    for (int i = 0; i < 8; i++)
        if (i != 2 && i != 5 && (h[i] < '0' || h[i] > '9')) return 0;
    return 1;
}

int main(void)
{
    if (system("rm -rf " ROOT) != 0 || mkdir(ROOT, 0755) != 0) {
        printf("LUA TEST: prep do %s falhou\n", ROOT);
        return 2;
    }
    CHECK(lua_runtime_init() == ESP_OK, "init da VM Lua falhou");
    lua_runtime_set_log_callback(cap_log, NULL);
    char err[256];
    esp_err_t e;

    /* ---- A: pda.battery.read()/get() ---- */
    {
        static const bstep sa[] = { { -1, -1, "bateria" } };
        scen(sa, 1);
        cap_reset();
        e = lua_runtime_run_string(
            "local mv,pct,st = pda.battery.read()\n"
            "assert(mv == -1, 'mv errado: '..tostring(mv))\n"
            "assert(pct == -1, 'pct errado: '..tostring(pct))\n"
            "assert(st == 'bateria', 'estado errado: '..tostring(st))\n"
            "local lo,hi,man = pda.battery.get()\n"
            "assert(lo == 3600 and hi == 3700 and man == true, 'get errado')\n",
            err, sizeof(err));
        CHECK(e == ESP_OK, "A: battery.read/get falhou: %s", err);
        printf("[lua] A: pda.battery.read() (mv,pct,estado) + get()  OK\n");
    }

    /* ---- B: pda.fs.mkdir (resolve, criação, rejeição de ..) ---- */
    {
        g_mkdir_calls = 0; g_mkdir_last[0] = '\0';
        cap_reset();
        e = lua_runtime_run_string("assert(pda.fs.mkdir('testdir'))",
                                   err, sizeof(err));
        CHECK(e == ESP_OK, "B: mkdir falhou: %s", err);
        CHECK(g_mkdir_calls == 1 && !strcmp(g_mkdir_last, ROOT "/testdir"),
              "B: resolve_path errado: calls=%d last='%s'",
              g_mkdir_calls, g_mkdir_last);
        CHECK(storage_is_dir(ROOT "/testdir"), "B: diretório não criado");
        e = lua_runtime_run_string("pda.fs.mkdir('a/../b')", err, sizeof(err));
        CHECK(e == ESP_FAIL, "B: mkdir com '..' devia ser rejeitado");
        printf("[lua] B: pda.fs.mkdir (raiz+rel, criação real, '..' rejeitado)  OK\n");
    }

    /* ---- C: pda.sleep_ms (clamp silencioso, erro de tipo) ---- */
    printf("[lua] C: (as linhas 'E (lua_runtime)' abaixo são casos de erro INTENCIONAIS)\n");
    {
        cap_reset();
        e = lua_runtime_run_string(
            "pda.sleep_ms(50); pda.sleep_ms(0); pda.sleep_ms(-5); pda.sleep_ms(99999999)",
            err, sizeof(err));
        CHECK(e == ESP_OK, "C: sleep válido falhou: %s", err);
        e = lua_runtime_run_string("pda.sleep_ms('x')", err, sizeof(err));
        CHECK(e == ESP_FAIL, "C: sleep com string devia falhar");
        e = lua_runtime_run_string("pda.sleep_ms()", err, sizeof(err));
        CHECK(e == ESP_FAIL, "C: sleep sem argumento devia falhar");
        printf("[lua] C: pda.sleep_ms (0/clamp/inválidos sem travar)  OK\n");
    }

    /* ---- D: script REAL end-to-end, arquivo novo ----
     * Cenário espelha o teste de campo: parte em carga (o usuário pode
     * tocar no script antes de desconectar), desconecta, desce a janela
     * 3700→3590 e o carregador volta — o script tem que gravar as 9
     * amostras e parar na volta. */
    {
        static const bstep sd[] = {
            { 4180, 100, "carregando" }, { 4176, 100, "carregando" },
            { 3950, 100, "assentando" }, { 3700, 100, "bateria" },
            { 3680,  78, "bateria" },    { 3650,  45, "bateria" },
            { 3610,  10, "bateria" },    { 3590,   0, "bateria" },
            { 4150,   0, "carregando" },
        };
        static const int exp_mv[9]  = { 4180, 4176, 3950, 3700, 3680,
                                        3650, 3610, 3590, 4150 };
        static const int exp_pct[9] = { 100, 100, 100, 100, 78, 45, 10, 0, 0 };
        static const char *exp_st[9] = {
            "carregando", "carregando", "assentando", "bateria", "bateria",
            "bateria", "bateria", "bateria", "carregando",
        };
        scen(sd, 9);
        g_mkdir_calls = 0; g_mkdir_last[0] = '\0';
        cap_reset();
        e = lua_runtime_run_file(SCRIPT, err, sizeof(err));
        CHECK(e == ESP_OK, "D: script falhou: %s", err);
        CHECK(g_mkdir_calls == 1 && !strcmp(g_mkdir_last, ROOT "/logs"),
              "D: mkdir('logs'): calls=%d last='%s'",
              g_mkdir_calls, g_mkdir_last);

        char lines[64][256];
        int nl = csv_load(CSV, lines, 64);
        CHECK(nl == 11, "D: CSV com %d linhas (esperado 11: header + '#' + 9)", nl);
        if (nl == 11) {
            CHECK(!strcmp(lines[0], HEADER), "D: header errado: '%s'", lines[0]);
            const char *pre = "# janela 3600-3700 mV (manual); script iniciou com uptime ";
            CHECK(!strncmp(lines[1], pre, strlen(pre)),
                  "D: linha '#' errada: '%s'", lines[1]);
            int prev_ut = -1, prev_tmin = -1;
            for (int i = 0; i < 9; i++) {
                csv_row r;
                int ok = parse_row(lines[2 + i], &r);
                CHECK(ok, "D: linha %d fora do formato: '%s'", i, lines[2 + i]);
                if (!ok) continue;
                CHECK(r.mv == exp_mv[i] && r.pct == exp_pct[i] &&
                          !strcmp(r.est, exp_st[i]),
                      "D: linha %d = %d/%d/%s (esperado %d/%d/%s)",
                      i, r.mv, r.pct, r.est, exp_mv[i], exp_pct[i], exp_st[i]);
                CHECK(hora_ok(r.hora), "D: coluna hora inválida: '%s'", r.hora);
                CHECK(r.ut > prev_ut, "D: uptime_s não cresce (linha %d)", i);
                CHECK(r.tmin >= prev_tmin, "D: t_min decresce (linha %d)", i);
                prev_ut = r.ut;
                prev_tmin = r.tmin;
            }
        }
        CHECK(cap_has("log_bateria[9]"), "D: console sem a amostra 9");
        CHECK(cap_has("carregador de volta"),
              "D: não anunciou a parada no carregador");
        CHECK(cap_has("fim — 9 amostra(s)"), "D: resumo final errado");
        printf("[lua] D: script REAL — CSV novo, 9 amostras do cenário, "
               "não parou na carga inicial, parou na volta  OK\n");
    }

    /* ---- E: continuação do arquivo existente (sem duplicar header) ---- */
    {
        static const bstep se[] = { { 3585, 0, "bateria" },
                                    { 4100, 100, "carregando" } };
        scen(se, 2);
        cap_reset();
        e = lua_runtime_run_file(SCRIPT, err, sizeof(err));
        CHECK(e == ESP_OK, "E: script falhou: %s", err);
        char lines[64][256];
        int nl = csv_load(CSV, lines, 64);
        CHECK(nl == 13, "E: CSV com %d linhas (esperado 13 = 11 + 2)", nl);
        if (nl == 13) {
            int headers = 0, hashes = 0;
            for (int i = 0; i < nl; i++) {
                if (!strcmp(lines[i], HEADER)) headers++;
                if (lines[i][0] == '#') hashes++;
            }
            CHECK(headers == 1 && hashes == 1,
                  "E: headers=%d hashes=%d (esperado 1/1 — sem duplicar)",
                  headers, hashes);
            csv_row r;
            CHECK(parse_row(lines[11], &r) && r.mv == 3585 &&
                      !strcmp(r.est, "bateria"),
                  "E: 1ª linha nova errada");
            CHECK(parse_row(lines[12], &r) && r.mv == 4100 &&
                      !strcmp(r.est, "carregando"),
                  "E: 2ª linha nova errada");
        }
        CHECK(cap_has("fim — 2 amostra(s)"), "E: resumo errado");
        printf("[lua] E: arquivo existente continua (header único) + parada  OK\n");
    }

    /* ---- F: sem diretório p/ gravar — falha graciosa ---- */
    {
        CHECK(system("rm -rf " ROOT "/logs") == 0, "F: prep falhou");
        g_mkdir_fail = 1;
        static const bstep sf[] = { { 3580, 0, "bateria" } };
        scen(sf, 1);
        cap_reset();
        e = lua_runtime_run_file(SCRIPT, err, sizeof(err));
        g_mkdir_fail = 0;
        CHECK(e == ESP_OK, "F: script devia encerrar graciosamente: %s", err);
        CHECK(!storage_file_exists(CSV), "F: CSV não devia existir");
        CHECK(cap_has("FALHA ao gravar"), "F: não anunciou a falha");
        CHECK(cap_has("fim — 0 amostra(s)"), "F: resumo errado");
        printf("[lua] F: sem onde gravar — encerra sem crash, anuncia no console  OK\n");
    }

    if (fails) { printf("LUA TEST: %d FALHA(S)\n", fails); return 1; }
    printf("LUA TEST: OK (bindings v5.16.2 + log_bateria.lua end-to-end: "
           "criação, cenário, parada no carregador, continuação, falha graciosa)\n");
    return 0;
}
