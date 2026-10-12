#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * battery.h — BAT.0 (v5.12.0): leitura de bateria da JC4880P443C_I_W.
 *
 * A placa TEM como medir bateria, e não é pelo IP5306: a folha 03 do
 * esquemático ("Expand IO") traz um divisor resistivo do rail da bateria:
 *
 *     BAT+ --R52 68K--+--R57 100K--GND      (C61 10nF no nó, p/ ruído)
 *                     |
 *                     +--R55 0R-- GPIO53    (ADC2_CH4 do ESP32-P4)
 *
 * GPIO53 = ADC2_CHANNEL_4 em soc/adc_channel.h do IDF 5.5.1; o Wi-Fi/BT
 * roda no C6 via ESP-Hosted, então o ADC2 do P4 está livre (a arbitragem
 * ADC2-vs-WiFi só existe quando o Wi-Fi roda no MESMO chip).
 *
 * O IP5306 continua SEM telemetria útil de nível: a versão padrão não tem
 * I2C (só a variante customizada "IP5306_I2C", nos pinos L1/L2/L3, aqui
 * NC), e mesmo a variante com I2C NÃO expõe tensão/corrente — só flags de
 * carga/cheia/carga-leve/tecla (register file V1.21, reg 0x70/0x71/0x72/
 * 0x77). O que medimos aqui é a bateria no divisor, direto no SAR ADC.
 *
 * Notas de uso:
 *  - Durante a CARGA a leitura reflete a tensão de carga (lê alto, ~4.2 V
 *    no topo) — é esperado, não é bug. Desde a v5.13.0 (BAT.2) uma máquina
 *    de estados detecta o carregador por salto de tensão e, ao desconectar,
 *    CONGELA o % ("assentando") até a tensão de carga dissipar — mata o
 *    efeito sanfona ao tirar/recolocar o carregador. Detalhes e limiares:
 *    docs/POWER.md §BAT.2.
 *  - A curva %-vs-mV é uma LiPo/Li-ion típica SEM carga (tabela s_curve em
 *    battery.c); vale p/ qualquer célula 4.2 V (14500, 18650, LiPo): o que
 *    muda entre elas é a CAPACIDADE (autonomia), não a curva de tensão.
 *    Químicas diferentes (LiFePO4, 4.35 V) exigem editar a s_curve.
 *  - Sob carga pesada (tela + falante) a tensão cai por resistência
 *    interna e o % lê pessimista. Calibrar depois contra multímetro
 *    (docs/POWER.md, BAT.1).
 *  - O divisor consome ~25 uA permanentes (4.2 V / 168 K) — irrelevante
 *    perto do consumo de tela/C6.
 */

/** Cria a task de polling (idempotente). Chamar depois do boot básico. */
esp_err_t battery_start(void);

/** true quando já existe leitura estável (após ~1 poll). */
bool battery_ready(void);

/** 0-100; -1 enquanto não pronto. */
int battery_percent(void);

/** Tensão da bateria em mV (já compensado o divisor); -1 se não pronto. */
int battery_mv(void);

/** Tensão no nó do divisor (mV), p/ diagnóstico/calibração; -1 idem. */
int battery_node_mv(void);

/** true se a calibração eFuse (line fitting) foi aplicada. */
bool battery_calibrated(void);

/** BAT.2: "bateria" | "carregando" | "assentando" (string estática). */
const char *battery_state_str(void);

/** Curto p/ status bar: "87%" | "--". Thread-safe (cópia). */
void battery_status_str(char *buf, int sz);

/** Longo p/ Config→Sistema: "87% (4012 mV, ADC2_CH4/GPIO53, cal.)". */
void battery_describe(char *buf, int sz);

#ifdef __cplusplus
}
#endif
