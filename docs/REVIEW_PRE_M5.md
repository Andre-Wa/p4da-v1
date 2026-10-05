# Revisão pré-M5 — código + pesquisa (2026-10-05)

Pedido do usuário: revisão geral com pesquisa online; melhorias entram
NA M5 (não agora). Método: leitura de `main/*` + logs das 7 rodadas de
validação + pesquisa (fontes no fim). Nada aqui muda código nesta
rodada; cada item tem dono de milestone.

## A. Quick wins p/ implementar no INÍCIO da M5

**A1. Backoff exponencial de reconexão Wi-Fi + handler não-bloqueante.**
Hoje o handler de `WIFI_EVENT_STA_DISCONNECTED` faz `vTaskDelay(2 s)`
DENTRO da task do event loop (atrasa todos os outros eventos por 2 s a
cada retry) e retry fixo p/ sempre (log fora de casa: ~40 retries/h).
Prática IDF: reconectar no DISCONNECTED é certo, mas o delay/backoff
devem viver em task própria; backoff 2/4/8/16/30 s com reset no
CONNECTED;_reason code_ de auth-fail vs AP-ausente pode distinguir
"senha errada" (parar e avisar UI) de "fora de casa" (backoff longo).
Fontes: station-scenarios e wifi guide (ver [1][2]).

**A2. Eco do clamp na UI.** `clamp_all()` ajusta em silêncio
(`deep < off+10 → off+10`, caso real: 40→66); os sliders não refletem o
valor clampado → usuário vê 40, arquivo tem 66. Fix: após
`pda_config_save()`, repassar `pda_settings()` aos setters do
`AppWindow` (deep/off/dim/brilho).

**A3. Seed de relógio sem rede.** Sem NTP, clock = epoch → mtime FAT
1980 e status bar sem HH:MM até sincronizar. Fix: persistir último UTC
bom no NVS a cada sync/set; no boot, `settimeofday` com ele + flag
"não-confiável" p/ UI (uptime até NTP, como hoje).

**A4. High-water-mark de pilhas.** Sem visibilidade de stack hoje
(histórico: ui_loop 8K→32K por Stack protection fault). Best practice
IDF: logar `uxTaskGetStackHighWaterMark()` por task (ui_loop, main,
io_*, lua, power, wifi_net) 60 s após boot + em cada entrada de
standby (dev-log INFO). Fonte: [3].

**A5. Auditoria de layouts Slint 1.12 (família do bug M4.16.2).**
Caracterização empírica: no VerticalLayout do diálogo, uma fileira
DEPOIS de um irmão de altura fixa foi mispositioned p/ fora do diálogo;
a MESMA fileira antes do irmão renderiza. Suspeitos com condicional/
altura-fixa + irmãos posteriores: `screens/networks.slint:56`
(NÃO validado em HW ainda — renderizar/validar), `screens/editor.slint:131`
(provado SEGURO: render_edit mostra a toolbar completa com a condicional
verdadeira), `app_ui.slint:363/464` (gesture layer/prompt no root,
fora de layout — seguro). Ação: render offscreen de `networks` + teste
HW da tela Redes na M5.

## B. Decisões certas confirmadas (NÃO mexer)

- **LittleFS** (não SPIFFS): SPIFFS depreciado/lento; LittleFS
  fail-safe + wear leveling + dirs. Fonte: [4][5]. Nossa partição
  `storage` usa subtype 0x82 (rótulo "spiffs" no csv) — mount é por
  LABEL, funciona; renomear o rótulo no csv é cosmético e exige
  reflash completo: deixar como está, documentado.
- **Slint 1.12.1 pinado** (bloat de glifos no 1.18) — revisitado só com
  mudança de IDF/fontes.
- **Standby robusto + kill-switch do light sleep** até o rework M-power.
- **Sombra NVS + censo/FNV + self-test do parser** (pegaram mídia
  insana e o bug de ABI do lua).
- **Transições de estado assinadas** (`pwr: X -> Y`): acharam 2 bugs
  reais (NTP no-op, churn do DIM) — manter e estender p/ Wi-Fi (A1).
- **Render offscreen como juiz antes do HW** (achou o bug do prompt que
  3 rounds de conjectura não acharam).

## C. Preparação M5a (áudio USB) — achado de arquitetura

- **P4 tem DOIS controladores USB 2.0 OTG (HS + FS), cada um host
  independente, e a Host Library aceita múltiplos class drivers
  simultâneos** (clientes separados, 1 task por driver) [6]. Consequência:
  teclado HID e DAC/caixa UAC podem coexistir SEM troca de papel — via
  hub externo (`CONFIG_USB_HOST_HUBS_SUPPORTED`) no conector OTG, ou um
  device composto. Plano M5a: (1) `usb_host_uac` como 2º client ao lado
  do HID; (2) hub support ligado; (3) seletor de saída (UAC vs futuro
  ES8311) em Config; (4) fallback sem hub: 1 device por vez com hot-swap
  já tratado pelo HID.
- Regra de task da Host Library: clients = tasks; nosso `spawn_thread`
  já casa com isso.
- Alto-falante/bateria JST chegados: caminho ES8311/NS4150 volta ao plano
  (beep de UI primeiro), UAC vira alternativa, não única saída.

## D. Backlog que a M5 NÃO precisa resolver (mantido)

Virtualização do editor (M3c); retry de enumeração USB (M4 backlog);
senha Wi-Fi fora de texto plano (M4d: NVS crypt); modem-sleep do C6
(host-power-save, Estágio 1b); DFS/`esp_pm` + teardown de painel
(Estágios 0/2, pós-medição); hibernate v2 com C6 off (Estágio 4);
mod BF2→LP GPIO; itálico (wishlist prio 3, adiado pelo usuário).

## E. Ruído de log aceito (não é bug)

`RPC_WRAP`/`H_API` durante reconexão (reduzível com A1); `LCD ID: FF FF
FF`; warning de pull-up I2C (hardware); `--- ERROR: device reports
readiness...` do USB-Serial-JTAG dormindo; `ESP-IDF
HEAD-HASH-NOTFOUND` (IDF sem metadados git na máquina do usuário).

## Fontes

[1] https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/station-scenarios.html
[2] https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32/api-guides/wifi.html
[3] https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32/api-guides/performance/ram-usage.html
[4] https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/file-system-considerations.html
[5] https://github.com/joltwallet/esp_littlefs
[6] https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_host.html
