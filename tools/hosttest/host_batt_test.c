/* host_batt_test.c — teste de host da máquina de estados BAT.2
 * (main/battery.c compilado com -DBATTERY_UNIT_TEST).
 *
 * Injeta perfis de tensão de bateria (mV já compensados do divisor) em
 * battery_ingest() e verifica: % estável na bateria, detecção de carga
 * por salto, CONGELAMENTO do % ao desconectar ("assentando"), retorno
 * ao carregar de novo e snap honesto p/ curva quando a tensão assenta.
 * Cenários espelham o log de campo de 2026-10-12 (v5.12.0): 4079 mV na
 * bateria, 4170-4174 mV em carga, 4135 mV assentando no standby.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "battery.h"
#include "nvs.h"

void battery_ingest(int bmv);        /* não-static sob BATTERY_UNIT_TEST */
void battery_learn(int bmv);         /* BAT.1a: idem */
void battery_calib_load(void);       /* BAT.1a: idem */
int curve_pct(int mv);               /* BAT.1a: idem */

/* ---- stub NVS mínimo (RAM) p/ battery.c no host ----
 * Só as chaves i32 "vmin"/"vmax" importam; o resto é no-op. O binário de
 * pda_config tem o SEU próprio stub em host_cfg_test.c (binários
 * separados no run.sh — sem colisão de símbolos). */
static int32_t s_nvs_vmin, s_nvs_vmax;
static bool s_nvs_have_min, s_nvs_have_max;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h)
{ (void)ns; (void)m; *h = 1; return ESP_OK; }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v)
{
    (void)h;
    if (!strcmp(k, "vmin")) { s_nvs_vmin = v; s_nvs_have_min = true; }
    else if (!strcmp(k, "vmax")) { s_nvs_vmax = v; s_nvs_have_max = true; }
    return ESP_OK;
}
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v)
{
    (void)h;
    if (!strcmp(k, "vmin") && s_nvs_have_min) { *v = s_nvs_vmin; return ESP_OK; }
    if (!strcmp(k, "vmax") && s_nvs_have_max) { *v = s_nvs_vmax; return ESP_OK; }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)
{ (void)h; (void)k; (void)v; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{ (void)h; (void)k; (void)v; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{ (void)h; (void)k; (void)v; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *len)
{ (void)h; (void)k; (void)v; (void)len; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static int fails;
#define CHECK(cond, fmt, ...)                                   \
    do {                                                        \
        if (!(cond)) {                                          \
            fails++;                                            \
            printf("FAIL: " fmt "\n", ##__VA_ARGS__);           \
        }                                                       \
    } while (0)

int main(void)
{
    /* A — bateria em repouso: % estável pela curva (~4082 mV -> ~90%) */
    for (int i = 0; i < 20; i++) battery_ingest(2430 * 168 / 100);
    CHECK(battery_percent() >= 88 && battery_percent() <= 91,
          "A: pct na bateria = %d (esperado ~90)", battery_percent());
    CHECK(strcmp(battery_state_str(), "bateria") == 0,
          "A: estado = %s (esperado bateria)", battery_state_str());

    /* B — conectou carregador: salto +90 mV num poll -> carregando */
    battery_ingest(4172);
    CHECK(strcmp(battery_state_str(), "carregando") == 0,
          "B: estado pós-salto = %s (esperado carregando)", battery_state_str());
    for (int i = 0; i < 5; i++) battery_ingest(4174);
    CHECK(battery_percent() >= 95,
          "B: pct em carga = %d (esperado >= 95)", battery_percent());

    /* C — desconectou: % CONGELADO enquanto assenta (sanfona morta) */
    battery_ingest(4084);
    CHECK(strcmp(battery_state_str(), "assentando") == 0,
          "C: estado pós-unplug = %s (esperado assentando)", battery_state_str());
    const int held = battery_percent();
    for (int i = 0; i < 10; i++) battery_ingest(4135 - i * 2);
    CHECK(battery_percent() == held,
          "C: pct não congelado no assentamento: %d != %d",
          battery_percent(), held);

    /* D — recolocou o carregador no meio do assentamento */
    battery_ingest(4174);
    CHECK(strcmp(battery_state_str(), "carregando") == 0,
          "D: estado no replug = %s (esperado carregando)", battery_state_str());

    /* E — desconectou de novo e deixou assentar (sag até o nível pré-carga) */
    battery_ingest(4080);
    CHECK(strcmp(battery_state_str(), "assentando") == 0,
          "E: estado no 2º unplug = %s (esperado assentando)", battery_state_str());
    for (int i = 0; i < 200 && strcmp(battery_state_str(), "assentando") == 0; i++)
        battery_ingest(4078 - (i % 2));
    CHECK(strcmp(battery_state_str(), "bateria") == 0,
          "E: estado pós-settle = %s (esperado bateria)", battery_state_str());
    CHECK(battery_percent() >= 85 && battery_percent() <= 92,
          "E: pct pós-settle = %d (esperado 85-92)", battery_percent());

    /* F — deriva lenta NA bateria não pode mudar de estado */
    for (int i = 0; i < 30; i++) battery_ingest(4075 - i / 3);
    CHECK(strcmp(battery_state_str(), "bateria") == 0,
          "F: deriva lenta virou %s (esperado bateria)", battery_state_str());

    /* ================= BAT.1a (v5.14.0): âncoras aprendidas =============
     * Cenários espelham o campo de 2026-10-12: célula 14500 que em carga
     * lê 4156 mV e relaxa p/ ~3.9 V no multímetro ao despluguar. */

    /* G — âncoras de fábrica = domínio da curva (mapeamento identidade) */
    CHECK(battery_anchor_min() == 3300 && battery_anchor_max() == 4200,
          "G: âncoras de fábrica = %d-%d (esperado 3300-4200)",
          battery_anchor_min(), battery_anchor_max());
    CHECK(curve_pct(4200) == 100 && curve_pct(3300) == 0,
          "G: extremos da curva = %d/%d (esperado 100/0)",
          curve_pct(4200), curve_pct(3300));
    CHECK(curve_pct(3750) == 45,
          "G: curva(3750) = %d (esperado 45, meio de 3800->53 / 3700->38)",
          curve_pct(3750));

    /* K1 — gate de pct: longe do vazio (89%) NÃO aprende vmin, mesmo com
     * leitura baixa (sag transitório de falante/tela não envenena âncora) */
    battery_learn(3000);
    CHECK(battery_anchor_min() == 3300,
          "K1: vmin moveu p/ %d com pct alto (esperado 3300)",
          battery_anchor_min());

    /* H — perto do vazio aprende vmin (simétrico: célula que corta em
     * 3.45 V SOBE a âncora; senão o PDA "morre aos 15%") */
    for (int i = 0; i < 12; i++) battery_ingest(3500);   /* ema -> ~3518 */
    CHECK(battery_percent() >= 0 && battery_percent() <= 30,
          "H: pct = %d (esperado <= 30 p/ aprender vmin)", battery_percent());
    battery_learn(3450);
    CHECK(battery_anchor_min() == 3450,
          "H: vmin = %d (esperado 3450)", battery_anchor_min());
    CHECK(curve_pct(3450) == 0 && curve_pct(4200) == 100,
          "H: extremos remapeados = %d/%d (esperado 0/100)",
          curve_pct(3450), curve_pct(4200));
    CHECK(curve_pct(3825) == 45,
          "H: curva(3825) c/ ânc 3450-4200 = %d (esperado 45 = std 3750)",
          curve_pct(3825));
    battery_learn(3380);                                 /* desce também */
    CHECK(battery_anchor_min() == 3380,
          "H: vmin = %d (esperado 3380)", battery_anchor_min());

    /* K2 — guardas de vmin: absoluto (2500 mV = leitura inválida) e
     * histerese (5 mV não move; evita escritas NVS em cascata) */
    battery_learn(2500);
    CHECK(battery_anchor_min() == 3380,
          "K2: vmin = %d após learn(2500) (esperado 3380, absoluto)",
          battery_anchor_min());
    battery_learn(3375);
    CHECK(battery_anchor_min() == 3380,
          "K2: vmin = %d após learn(3375) (esperado 3380, histerese)",
          battery_anchor_min());

    /* I — em carga aprende vmax (só acima do atual+15: meia-carga não
     * move; topo CC-CV sim) */
    battery_ingest(4260);                                /* salto -> carregando */
    CHECK(strcmp(battery_state_str(), "carregando") == 0,
          "I: estado = %s (esperado carregando)", battery_state_str());
    battery_learn(4260);
    CHECK(battery_anchor_max() == 4260,
          "I: vmax = %d (esperado 4260)", battery_anchor_max());
    CHECK(curve_pct(4260) == 100,
          "I: curva(4260) = %d (esperado 100)", curve_pct(4260));
    CHECK(curve_pct(3820) == 45,
          "I: curva(3820) c/ ânc 3380-4260 = %d (esperado 45 = std 3750)",
          curve_pct(3820));

    /* K3/K4 — guardas em carga: absoluto (4500 inválido) e vmin intocado
     * (learn() em CHARGING nunca move vmin) */
    battery_learn(4500);
    CHECK(battery_anchor_max() == 4260,
          "K3: vmax = %d após learn(4500) (esperado 4260, absoluto)",
          battery_anchor_max());
    battery_learn(3200);
    CHECK(battery_anchor_min() == 3380,
          "K4: vmin = %d após learn(3200) em carga (esperado 3380)",
          battery_anchor_min());

    /* J — persistência: reload do "NVS" devolve o par aprendido; reset
     * restaura (e persiste) o padrão de fábrica */
    battery_calib_load();
    CHECK(battery_anchor_min() == 3380 && battery_anchor_max() == 4260,
          "J: reload NVS = %d-%d (esperado 3380-4260)",
          battery_anchor_min(), battery_anchor_max());
    battery_reset_calibration();
    CHECK(battery_anchor_min() == 3300 && battery_anchor_max() == 4200,
          "J: reset = %d-%d (esperado 3300-4200)",
          battery_anchor_min(), battery_anchor_max());
    battery_calib_load();
    CHECK(battery_anchor_min() == 3300 && battery_anchor_max() == 4200,
          "J: reset persistido = %d-%d (esperado 3300-4200)",
          battery_anchor_min(), battery_anchor_max());

    /* K5 — vão mínimo entre âncoras (600 mV): candidato a vmin que deixa
     * o intervalo estreito demais é rejeitado; um válido passa */
    for (int i = 0; i < 14; i++) battery_ingest(3500);   /* -> bateria, ~14% */
    CHECK(strcmp(battery_state_str(), "bateria") == 0 && battery_percent() <= 30,
          "K5: pré-condição (estado=%s pct=%d)", battery_state_str(),
          battery_percent());
    battery_learn(3650);                                 /* vão 550 < 600 */
    CHECK(battery_anchor_min() == 3300,
          "K5: vmin = %d após learn(3650) (esperado 3300, vão < 600)",
          battery_anchor_min());
    battery_learn(3250);                                 /* vão 950 ok */
    CHECK(battery_anchor_min() == 3250,
          "K5: vmin = %d após learn(3250) (esperado 3250)",
          battery_anchor_min());

    if (fails) { printf("BATT TEST: %d FALHA(S)\n", fails); return 1; }
    printf("BATT TEST: OK (BAT.2 estados + BAT.1a âncoras)\n");
    return 0;
}
