# Energia & standby — como e por quê

## As quatro opções clássicas, com números esperados

| Modo | O que fica ligado | Consumo típico estimado* | Resume |
|---|---|---|---|
| Só dimerizar/apagar backlight | CPU + UI + DSI + SD | 150–300 mA | instantâneo |
| **Light sleep** (nosso STANDBY) | RAM, PSRAM, DSI parado (DISPOFF), backlight 0 | 20–80 mA | **< 100 ms** |
| **Deep sleep** (nosso HIBERNATE) | nada (só domínio RTC) | 0,1–5 mA | reboot 1,5–3 s (sessão restaurada do SD) |
| Desligado (IP5306 corta rail) | nada | ~µA | boot frio |

\* Estimativas de projeto para validar com medição real (ver "Plano de medição").
O backlight e o painel dominam o consumo ativo; PSRAM em retenção domina o light sleep.

## Por que o deep sleep NÃO é o degrau automático padrão

No ESP32-P4, **wake de deep sleep só por LP GPIO (GPIO0–15), timer ou LP-UART**.
Nesta placa:

- botão BOOT = GPIO35 → **fora** do domínio LP;
- INT do GT911 = GPIO21 → **fora** do domínio LP;
- nenhum botão/wake de usuário em GPIO0–15 (esses pinos estão ocupados por
  I²S/SDIO/I²C/reset de painel).

Logo, deep sleep automático = aparelho "some" até tirar/recarregar a bateria.
Inaceitável como comportamento silencioso. Light sleep, por outro lado, aceita
wake por **qualquer** GPIO (gpio_wakeup) → botão e toque funcionam.

## Arquitetura em degraus (implementada em `power_mgmt.c`)

```
ACTIVE ──idle>=dim_after_s──▶ DIM (backlight 12%)
  ▲                            │
  │ qualquer atividade         │ idle>=screen_off_after_s
  └────────────────────────────▼
                          STANDBY: backlight 0 + DISPOFF + light sleep
                          wake: GPIO35 (botão) [+ GPIO21 se wake_on_touch]
                          │ timer (se deep_sleep_after_s>0) e segue ocioso
                          ▼
                          HIBERNATE: salva sessão → unmount SD → deep sleep
                          volta apenas no power-on/reset, sessão restaurada
```

- `power_mgmt_activity()` é chamado por: teclas USB, callbacks de UI e (opcional)
  toque. Em STANDBY a task fica bloqueada em `esp_light_sleep_start()`.
- Hibernar também é **manual** (tela Config → "Hibernar"), que é o "desligar"
  do PDA: sessão (tela + nota aberta) gravada em `<raiz>/.state/session.txt`.
- Antes do deep sleep: `storage_shutdown_sd()` (unmount FatFS + LDO ch4 off)
  para não corromper o cartão nem gastar o rail dele dormindo.

## Decisões configuráveis (Lua: `config/system.lua`)

| Chave | Default | Significado |
|---|---|---|
| `power.dim_after_s` | 30 | ocioso → dimeriza |
| `power.screen_off_after_s` | 120 | ocioso → STANDBY |
| `power.deep_sleep_after_s` | 0 | em STANDBY, hiberna após N s (0 = nunca) |
| `power.wake_on_touch` | false | [HW?] GPIO21 como wake; ligue após validar o pino |

## Plano de medição (próximo passo de hardware)

1. Amperímetro em série no rail da bateria (ou no cabo do CN4).
2. Registrar: ACTIVE (brilho 100/50/10), DIM, STANDBY, HIBERNATE.
3. Se STANDBY ficar > ~40 mA, o próximo alvo é o painel: tear-down completo do
   DSI (release LDO ch3) no standby e re-init no wake (~0,3–0,5 s aceitáveis).
   Já está mapeado como otimização; não fizemos agora para manter o wake instantâneo.
4. Se HIBERNATE ficar > ~1 mA, investigar rails sempre-vivos (TLV62569, IP5306
   quiescente) — aí é limite de placa, não de firmware.

## SD × light sleep: por que remontamos no wake

Medido em hardware (2026-09-22): após `STANDBY`, qualquer I/O no cartão
falha com `sdmmc_host_wait_for_event returned 0x107` — o host SDMMC sai
do estado funcional no sleep e o driver do IDF 5.5 não o re-inicializa no
wake. `storage_remount_sd()` (unmount + mount, ~0,3–0,5 s) roda no hook de
wake do `power_mgmt`, antes de a UI voltar a tocar no VFS; se o cartão não
voltar, a raiz ativa degrada para `/internal/pda` e o PDA segue usável.
Os `--- ERROR: device reports readiness to read...` do monitor são só o
USB-Serial-JTAG dormindo (cosmético).
