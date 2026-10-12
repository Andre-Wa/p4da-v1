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

#include "battery.h"

void battery_ingest(int bmv);   /* não-static sob BATTERY_UNIT_TEST */

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

    if (fails) { printf("BATT TEST: %d FALHA(S)\n", fails); return 1; }
    printf("BATT TEST: OK (máquina de estados BAT.2)\n");
    return 0;
}
