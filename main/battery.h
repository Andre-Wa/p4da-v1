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
 *    BAT.1a (v5.14.0): os EXTREMOS da curva se ajustam sozinhos à célula
 *    (âncoras vmin/vmax aprendidas e persistidas no NVS) — ver battery.c.
 *  - Sob carga pesada (tela + falante + Wi-Fi) a tensão de terminal SAG
 *    pela resistência interna (campo 14ª rodada: ±50 mV de dança, mínimo
 *    2815 mV com tudo ligado). BAT.1c (v5.15.0): o % na bateria deriva de
 *    uma EMA lenta (τ≈64 s) e anda a PASSO (queda ≤1 pt/12 s; subida só
 *    fora de banda morta de 2 pts, ≤1 pt/24 s) — o número da UI não dança
 *    mais com brilho/Wi-Fi. O sag continua visível no log serial (mV
 *    rápido vs. lento). Refino da FORMA da curva contra multímetro segue
 *    em BAT.1b (docs/POWER.md, §BAT.1).
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

/** Curto p/ status bar: "87%" | "87%+" (carregando) | "87%~" (assentando)
 *  | "--". O sufixo existe p/ leitura de campo SEM serial (na bateria o
 *  log USB não existe). Thread-safe (cópia). */
void battery_status_str(char *buf, int sz);

/** Longo p/ Config→Sistema: "87% (4012 mV, ânc 3300-4200 mV,
 *  ADC2_CH4/GPIO53, cal. eFuse[, estado])". Atualizado ao vivo a cada 4 s
 *  (v5.14.0) — teste de bateria sem serial se faz por esta linha.
 *  BAT.1c: o mV mostrado é o filtro do qual o % deriva (EMA lenta na
 *  bateria/assentando, rápida em carga) — par %-mV sempre coerente. */
void battery_describe(char *buf, int sz);

/** BAT.1a (v5.14.0): âncoras mín/máx do remapeamento da curva (mV).
 *  Padrão de fábrica = domínio da s_curve (3300-4200); o módulo aprende
 *  sozinho em campo (vmin perto do vazio, vmax no topo da carga) e
 *  persiste no NVS (ns "batt"). */
int battery_anchor_min(void);
int battery_anchor_max(void);

/** Restaura as âncoras de fábrica (botão Config→Sistema; também limpa a
 *  janela manual da BAT.1d). Persiste. */
void battery_reset_calibration(void);

/** BAT.1d (v5.16.1): janela de âncoras MANUAL — teste de campo sem novo
 *  firmware (Lua: pda.battery.set(min,max)). Valida só pela sanidade
 *  ELÉTRICA absoluta (2800..4400 mV, vão >= 50 mV): os guardas do modo
 *  aprendido (vão >= 600, vmax >= 3900) protegem o APRENDIZ de sags, não
 *  o usuário de uma janela de teste estreita. Persiste no NVS (ns "batt",
 *  flag "man") e sobrevive a reboot; enquanto ativa, battery_learn()
 *  congela (senão um sag profundo — 2815 mV na 14ª rodada — arrasta o
 *  vmin e envenena a medição) e a linha Config→Sistema mostra "(manual)".
 *  Retorna false se o par for recusado (âncoras intactas). Chamável da
 *  thread Lua/UI: mesmo padrão do reset (ints + flag s_recal consumida
 *  pela task no próximo poll). */
bool battery_set_anchors(int vmin, int vmax);

/** true enquanto a janela manual (BAT.1d) estiver ativa. */
bool battery_anchors_manual(void);

#ifdef __cplusplus
}
#endif
