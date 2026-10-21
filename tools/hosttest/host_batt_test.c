/* host_batt_test.c — teste de host da máquina de estados BAT.2 + âncoras
 * BAT.1a + estabilidade BAT.1c + âncoras manuais BAT.1d (main/battery.c
 * compilado com -DBATTERY_UNIT_TEST).
 *
 * Injeta perfis de tensão de bateria (mV já compensados do divisor) em
 * battery_ingest() e verifica: % estável na bateria, detecção de carga
 * por salto COM CONFIRMAÇÃO (BAT.1c), congelamento do % ao desconectar
 * ("assentando"), retorno ao carregar de novo, snap honesto p/ curva,
 * rejeição de transiente de carga (sag+recuperação não vira "carregando"),
 * passo do % (queda/suave ritmada + banda morta), religação imediata do
 * % no reset de calibração e (P–T) a janela manual da BAT.1d: mapeamento
 * 3600-3700 do teste do usuário, learn congelado, round-trip NVS da flag
 * "man", reset que devolve o modo aprendido e validação da entrada.
 * Cenários espelham os logs de campo (v5.12 2026-10-12: 4079 mV bateria,
 * 4170-4174 mV carga; v5.14 14ª rodada: sag 2815 mV, dança ±50 mV com
 * brilho/Wi-Fi, âncoras aprendidas 2815-4200).
 *
 * NOTA BAT.1c: o % exibido anda a PASSO (queda ≤1 pt/3 polls, subida ≤1
 * pt/6 polls com banda morta de 2). Perfis sintéticos que pulam centenas
 * de mV de uma vez exigem MUITOS polls p/ o passo alcançar a curva — os
 * loops abaixo são dimensionados p/ isso; na descarga real a tensão cai
 * ~100x mais devagar que o passo e ele nunca é o limitante.
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
int battery_slow_mv(void);           /* BAT.1c: EMA lenta (só host) */

/* ---- stub NVS mínimo (RAM) p/ battery.c no host ----
 * Chaves i32 "vmin"/"vmax" + u8 "man" (BAT.1d); o resto é no-op. O
 * binário de pda_config tem o SEU próprio stub em host_cfg_test.c
 * (binários separados no run.sh — sem colisão de símbolos). */
static int32_t s_nvs_vmin, s_nvs_vmax;
static bool s_nvs_have_min, s_nvs_have_max;
static uint8_t s_nvs_man;
static bool s_nvs_have_man;
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
{
    (void)h;
    if (!strcmp(k, "man")) { s_nvs_man = v; s_nvs_have_man = true; }
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{
    (void)h;
    if (!strcmp(k, "man") && s_nvs_have_man) { *v = s_nvs_man; return ESP_OK; }
    return ESP_ERR_NVS_NOT_FOUND;
}
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

    /* B — conectou carregador: salto +90 mV -> CANDIDATO (BAT.1c: um poll
     * só não confirma mais; alívio de carga de brilho/beep recuperava
     * 50-150 mV num poll e fantasiava de carregador). 2º poll sustentado
     * E acima da EMA lenta +40 -> confirmou. */
    battery_ingest(4172);
    CHECK(strcmp(battery_state_str(), "bateria") == 0,
          "B: estado no candidato = %s (esperado bateria até confirmar)",
          battery_state_str());
    battery_ingest(4174);
    CHECK(strcmp(battery_state_str(), "carregando") == 0,
          "B: estado pós-confirmação = %s (esperado carregando)",
          battery_state_str());
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

    /* D — recolocou o carregador no meio do assentamento (reentrada imediata:
     * nível voltou ao pico-30; a confirmação de 2 polls é só p/ entrada
     * vindo de BATERIA, onde o transiente de carga acontece) */
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

    /* K1 — gate de pct: longe do vazio (~88%) NÃO aprende vmin, mesmo com
     * leitura baixa (sag transitório de falante/tela não envenena âncora) */
    battery_learn(3000);
    CHECK(battery_anchor_min() == 3300,
          "K1: vmin moveu p/ %d com pct alto (esperado 3300)",
          battery_anchor_min());

    /* H — perto do vazio aprende vmin (simétrico: célula que corta em
     * 3.45 V SOBE a âncora; senão o PDA "morre aos 15%"). BAT.1c: o % anda
     * a passo (1 pt/3 polls) — 200 polls p/ o passo levar o pct de ~88 ao
     * gate de aprendizado (<= 30) com a EMA lenta já colada em 3500. */
    for (int i = 0; i < 200; i++) battery_ingest(3500);
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
     * move; topo CC-CV sim). BAT.1c: salto +760 vira candidato e só o
     * 2º poll confirma. */
    battery_ingest(4260);
    CHECK(strcmp(battery_state_str(), "bateria") == 0,
          "I: estado no candidato = %s (esperado bateria)", battery_state_str());
    battery_ingest(4262);
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
     * o intervalo estreito demais é rejeitado; um válido passa.
     * (Vindo de carga: unplug -> assentando -> snap -> passo p/ descer.) */
    for (int i = 0; i < 150; i++) battery_ingest(3500);
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

    /* ============ BAT.1c (v5.15.0): estabilidade sob carga ==============
     * Estado aqui: bateria, ~3500 mV, âncoras 3250-4200 (K5). */

    /* L — transiente rejeitado: sag profundo de 1 poll (tudo ligado:
     * brilho+Wi-Fi+falante; campo real: 2815 mV) seguido de recuperação.
     * A RECUPERAÇÃO (+600 mV!) é candidato a carregador, mas não confirma:
     * voltou AO nível da EMA lenta, não ACIMA dela +40. O % anda no máx.
     * 2 pts nos 6 polls (o passo limita; a curva nem se moveu de verdade). */
    {
        const int p0 = battery_percent();
        battery_ingest(2900);                       /* sag de 1 poll */
        battery_ingest(3500);                       /* recuperação: candidato */
        CHECK(strcmp(battery_state_str(), "bateria") == 0,
              "L: candidato pós-sag = %s (esperado bateria)",
              battery_state_str());
        battery_ingest(3500);                       /* confirma? NÃO */
        CHECK(strcmp(battery_state_str(), "bateria") == 0,
              "L: estado pós-recuperação = %s (esperado bateria, não "
              "carregando)", battery_state_str());
        for (int i = 0; i < 3; i++) battery_ingest(3500);
        const int p1 = battery_percent();
        CHECK(p1 <= p0 && p0 - p1 <= 2,
              "L: pct moveu %d pts sob sag+recuperação (esperado <= 2; %d->%d)",
              p0 - p1, p0, p1);
        CHECK(strcmp(battery_state_str(), "bateria") == 0,
              "L: estado final = %s (esperado bateria)", battery_state_str());
    }

    /* M — queda ritmada: degrau sustentado 3500->3300 (curva cai ~14 pts);
     * o % exibido desce no máx. 1 pt a cada 3 polls, sem subir nunca. */
    {
        int prev = battery_percent();
        for (int i = 0; i < 30; i++) {
            battery_ingest(3300);
            const int p = battery_percent();
            if (p < prev) CHECK(prev - p == 1,
                  "M: queda de %d pts num poll (%d -> %d)", prev - p, prev, p);
            CHECK(p <= prev, "M: pct subiu durante queda (%d -> %d)", prev, p);
            prev = p;
        }
        CHECK(battery_percent() <= 5 && battery_percent() >= 1,
              "M: pct final = %d (esperado ~2-4, passo acompanhou a curva "
              "sem ultrapassá-la)", battery_percent());
        CHECK(strcmp(battery_state_str(), "bateria") == 0,
              "M: estado = %s (esperado bateria)", battery_state_str());
    }

    /* M2 — subida ritmada com banda morta: rampa suave (+3 mV/poll, abaixo
     * do limiar de candidato) até ~3390; o % sobe no máx. 1 pt a cada 6
     * polls e PARA a 2 pts da curva (nunca cola por completo). Os primeiros
     * polls ainda terminam a descida do passo de M (pct -> curva) — a
     * monotonia é cobrada após estabilizar. */
    {
        int prev = battery_percent();
        for (int i = 0; i < 30; i++) battery_ingest(3300 + 3 * i);
        for (int i = 0; i < 60; i++) {
            battery_ingest(3390);
            const int p = battery_percent();
            if (i >= 9) {   /* pós-estabilização: só sobe (1 pt por vez) */
                CHECK(p >= prev,
                      "M2: pct caiu durante subida (%d -> %d no poll %d)",
                      prev, p, i);
                if (p > prev) CHECK(p - prev == 1, "M2: subida de %d pts",
                                    p - prev);
            }
            prev = p;
        }
        CHECK(battery_percent() >= 3 && battery_percent() <= 6,
              "M2: pct final = %d (esperado 3-6: subiu ritmado e parou na "
              "banda morta)", battery_percent());
    }

    /* N — dança de carga ±15 mV (Wi-Fi/brilho): perfil oscilante 40 polls;
     * o % NÃO se move (curva estável + banda morta) — é exatamente a
     * queixa da 14ª rodada ("porcentagem variando muito durante o uso"). */
    {
        const int p0 = battery_percent();
        int mn = p0, mx = p0;
        for (int i = 0; i < 40; i++) {
            battery_ingest(i % 2 ? 3405 : 3375);
            const int p = battery_percent();
            if (p < mn) mn = p;
            if (p > mx) mx = p;
        }
        CHECK(mx - mn <= 1,
              "N: pct oscilou %d pts sob dança de ±15 mV (esperado <= 1)",
              mx - mn);
        CHECK(strcmp(battery_state_str(), "bateria") == 0,
              "N: estado = %s (esperado bateria)", battery_state_str());
    }

    /* O — "Zerar calibração" religa o % NA HORA (flag consumida no próximo
     * poll), em vez do passo caminhar até a curva nova. Perfil: rampa suave
     * até 3606 mV (o % sobe ritmado com âncoras 3250-4200), reset, 1 poll:
     * pct == curve_pct(lenta) com âncoras de fábrica. */
    {
        for (int i = 1; i <= 67; i++) battery_ingest(3405 + 3 * i);  /* ->3606 */
        for (int i = 0; i < 60; i++) battery_ingest(3606);
        CHECK(strcmp(battery_state_str(), "bateria") == 0,
              "O: rampa virou %s (esperado bateria)", battery_state_str());
        const int before = battery_percent();
        battery_reset_calibration();
        battery_ingest(3606);                    /* consome a flag s_recal */
        const int expect = curve_pct(battery_slow_mv());
        CHECK(battery_percent() == expect,
              "O: pct pós-reset = %d (esperado %d = curva nova na EMA lenta; "
              "antes era %d)", battery_percent(), expect, before);
        CHECK(battery_percent() <= before,
              "O: religação inflou o pct (%d > %d)", battery_percent(), before);
        CHECK(battery_anchor_min() == 3300 && battery_anchor_max() == 4200,
              "O: âncoras pós-reset = %d-%d", battery_anchor_min(),
              battery_anchor_max());
        for (int i = 0; i < 12; i++) battery_ingest(3606);
        CHECK(battery_percent() == expect,
              "O: pct instável pós-reset: %d != %d", battery_percent(), expect);
    }

    /* ===== BAT.1d (v5.16.1): âncoras MANUAIS — seções P-T =====
     * Cenário do usuário (16ª rodada): janela de teste 3600-3700 mV —
     * % ignora a tensão de carga (>= 3700 -> 100) e só se move na
     * bateria; aprendizado congelado; reboot mantém; reset devolve o
     * modo aprendido. */

    /* P — janela manual 3600-3700: mapeamento pedido pelo usuário
     * (carga "ignorada" em 100%, meio amplificado 9x pelo std_mv,
     * <= 3600 em 0%) + religação do % no próximo poll (s_recal). */
    {
        CHECK(battery_set_anchors(3600, 3700),
              "P: set(3600,3700) recusado (esperado: aceito)");
        CHECK(battery_anchor_min() == 3600 && battery_anchor_max() == 3700,
              "P: âncoras = %d-%d (esperado 3600-3700)",
              battery_anchor_min(), battery_anchor_max());
        CHECK(battery_anchors_manual(), "P: flag manual não ativa");
        CHECK(curve_pct(4176) == 100,
              "P: carga 4176 mV -> %d (esperado 100: ignora a carga alta)",
              curve_pct(4176));
        CHECK(curve_pct(3900) == 100,
              "P: repouso pós-carga 3900 mV -> %d (esperado 100)",
              curve_pct(3900));
        CHECK(curve_pct(3700) == 100,
              "P: topo 3700 mV -> %d (esperado 100)", curve_pct(3700));
        CHECK(curve_pct(3675) == 78,
              "P: 3675 mV -> %d (esperado 78)", curve_pct(3675));
        CHECK(curve_pct(3650) == 45,
              "P: 3650 mV -> %d (esperado 45)", curve_pct(3650));
        CHECK(curve_pct(3600) == 0,
              "P: base 3600 mV -> %d (esperado 0)", curve_pct(3600));
        CHECK(curve_pct(3500) == 0,
              "P: abaixo da base 3500 mV -> %d (esperado 0)", curve_pct(3500));
        battery_ingest(3650);           /* consome s_recal (como o reset) */
        CHECK(battery_percent() == curve_pct(battery_slow_mv()),
              "P: pct não religou na janela manual: %d != curva %d",
              battery_percent(), curve_pct(battery_slow_mv()));
    }

    /* Q — aprendizado CONGELADO enquanto manual. Sem o freeze: o sag
     * 2815 mV (14ª rodada, pct <= 30) arrastaria o vmin (|2815-3600| > 15,
     * vão 885 >= 600) e a carga 4250 arrastaria o vmax (> 3700+15, vão
     * 650 >= 600) — os dois envenenariam o teste do usuário. */
    {
        battery_ingest(3606);
        CHECK(battery_percent() <= 30,
              "Q: pct %d > 30 (precondição do learn de vmin)",
              battery_percent());
        battery_learn(2815);
        CHECK(battery_anchor_min() == 3600,
              "Q: vmin arrastado p/ %d (esperado congelado em 3600)",
              battery_anchor_min());
        battery_ingest(4172);           /* candidato (salto +566) */
        battery_ingest(4176);           /* sustentado -> carregando */
        CHECK(strcmp(battery_state_str(), "carregando") == 0,
              "Q: estado = %s (esperado carregando)", battery_state_str());
        battery_learn(4250);
        battery_learn(4250);
        CHECK(battery_anchor_max() == 3700,
              "Q: vmax arrastado p/ %d (esperado congelado em 3700)",
              battery_anchor_max());
    }

    /* R — round-trip NVS: "reboot" mantém a janela manual (flag man=1
     * bypassa os guardas do aprendido); SEM a flag, o loader mantém o
     * comportamento antigo chave a chave (vmin 3600 passa no guard
     * <= 3700; vmax 3700 REPROVA no guard >= 3900 -> fica 4200; vão
     * 600 >= 600 mantém o par) — NVS corrompida/apagada-man não abre
     * janela estreita sozinha. */
    {
        battery_calib_load();           /* simula o boot */
        CHECK(battery_anchor_min() == 3600 && battery_anchor_max() == 3700 &&
              battery_anchors_manual(),
              "R: reload manual = %d-%d man=%d (esperado 3600-3700 man=1)",
              battery_anchor_min(), battery_anchor_max(),
              battery_anchors_manual());
        nvs_handle_t h;
        nvs_open("batt", NVS_READWRITE, &h);
        nvs_set_u8(h, "man", 0);        /* mesmo par, sem a flag */
        nvs_commit(h);
        nvs_close(h);
        battery_calib_load();
        CHECK(battery_anchor_min() == 3600 && battery_anchor_max() == 4200 &&
              !battery_anchors_manual(),
              "R: sem flag, loader mudou: %d-%d man=%d (esperado 3600-4200 "
              "man=0: vmax 3700 reprovado no guard >= 3900)",
              battery_anchor_min(), battery_anchor_max(),
              battery_anchors_manual());
    }

    /* S — "Zerar calibração" limpa o manual (persistido) e o aprendizado
     * VOLTA: em carga, 4250 > 4200+15 com vão 950 >= 600 aprende. */
    {
        battery_set_anchors(3600, 3700);    /* reativa p/ testar o reset */
        battery_reset_calibration();
        CHECK(battery_anchor_min() == 3300 && battery_anchor_max() == 4200 &&
              !battery_anchors_manual(),
              "S: pós-reset = %d-%d man=%d (esperado 3300-4200 man=0)",
              battery_anchor_min(), battery_anchor_max(),
              battery_anchors_manual());
        battery_calib_load();               /* reboot pós-reset: fábrica */
        CHECK(battery_anchor_min() == 3300 && battery_anchor_max() == 4200,
              "S: reload pós-reset = %d-%d (esperado 3300-4200)",
              battery_anchor_min(), battery_anchor_max());
        CHECK(strcmp(battery_state_str(), "carregando") == 0,
              "S: estado = %s (esperado carregando: herdado de Q)",
              battery_state_str());
        battery_learn(4250);
        CHECK(battery_anchor_max() == 4250,
              "S: vmax não aprendido pós-reset: %d (esperado 4250)",
              battery_anchor_max());
        battery_reset_calibration();        /* devolve fábrica p/ seção T */
    }

    /* T — validação da entrada manual: invertido, fora da faixa elétrica
     * absoluta e vão < 50 são recusados SEM tocar nas âncoras; a
     * fronteira vão == 50 é aceita. */
    {
        CHECK(!battery_set_anchors(3700, 3600), "T: par invertido aceito");
        CHECK(!battery_set_anchors(2000, 4200), "T: vmin < 2800 aceito");
        CHECK(!battery_set_anchors(3600, 4500), "T: vmax > 4400 aceito");
        CHECK(!battery_set_anchors(3600, 3649), "T: vão 49 mV aceito");
        CHECK(battery_anchor_min() == 3300 && battery_anchor_max() == 4200,
              "T: set inválido mexeu nas âncoras: %d-%d (esperado 3300-4200)",
              battery_anchor_min(), battery_anchor_max());
        CHECK(battery_set_anchors(3600, 3650),
              "T: vão 50 mV (fronteira) recusado");
        CHECK(battery_anchors_manual(), "T: flag manual não ativa na fronteira");
        battery_reset_calibration();        /* limpeza: NVS volta a fábrica */
        battery_calib_load();
        CHECK(battery_anchor_min() == 3300 && battery_anchor_max() == 4200 &&
              !battery_anchors_manual(),
              "T: limpeza falhou: %d-%d man=%d",
              battery_anchor_min(), battery_anchor_max(),
              battery_anchors_manual());
    }

    if (fails) { printf("BATT TEST: %d FALHA(S)\n", fails); return 1; }
    printf("BATT TEST: OK (BAT.2 estados + BAT.1a âncoras + BAT.1c estabilidade + BAT.1d manual)\n");
    return 0;
}
