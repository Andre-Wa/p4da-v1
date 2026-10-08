# USB Host HID — refatoração (M1.5)

## Sintomas que motivaram (hardware, 2026-09-22)

1. Teclado só era reconhecido se plugado **antes** do boot.
2. Após acordar do standby, o teclado nunca mais reconectava.

## Causas raiz

### 1. A task da biblioteca host se suicidava no boot sem teclado
O laço do protótipo:

```c
if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) break;   // <-- armadilha
...
vTaskDelete(NULL);
```

Existe uma janela logo após `usb_host_install()` e antes de
`hid_host_install()` registrar seu client em que o barramento tem
**zero clients e zero devices** → `NO_CLIENTS` + `ALL_FREE` disparam →
`break` + `vTaskDelete`. Sem teclado no boot, a task morria ali e nenhum
evento de conexão era processado depois (hot-plug morto). Com teclado
plugado, a enumeração em andamento mantinha `ALL_FREE` falso e a task
sobrevivia — daí a impressão de que "só funciona se plugar antes".

**Fix:** barramento vazio é o estado *normal* de um PDA. O laço só termina
quando `usb_host_uninstall()` invalida o handle (`handle_events` retorna
erro) — que é exatamente o caminho de teardown do standby.

### 2. O periférico DWC2 não sobrevive ao light sleep
Mesma família de problema do SDMMC (`0x107`): durante o light sleep o
controlador USB sai do estado funcional e o IDF 5.5 não o re-inicializa no
wake. Diferente do SD (que dá para remontar mantendo o resto), aqui o
caminho limpo é **teardown antes de dormir + reinstall no wake**:

- `usb_hid_keyboard_prepare_sleep()`, **nesta ordem** (a ordem inversa
  deixa o host zumbi e o resume falha — corrigido em 2026-09-22):
  1. fecha interfaces abertas e zera o tracking;
  2. `s_shutdown = true` e `hid_host_uninstall()` → sem client, o host
     libera tudo e a lib task vê `NO_CLIENTS`+`ALL_FREE` e **sai sozinha**,
     dando um semáforo (`s_lib_done_sem`);
  3. espera o semáforo (timeout 1 s) e só então `usb_host_uninstall()` —
     o uninstall com a lib task viva dentro de `handle_events()` não
     completa (padrão dos exemplos do IDF é a task sair primeiro).
- `usb_hid_keyboard_resume()`: recria lib task + `hid_host_install()`.
  Reentrante por construção (`start_stack()` guarda `s_installed`).
  Cada fase loga (`usb_host_uninstall: ESP_OK`, `USB apos wake: instalado=1`)
  para o próximo diagnóstico ser direto do log.

Ambos são chamados pelo hook de standby do `power_mgmt` (task de energia),
nunca pela task de UI.

## Outras melhorias incluídas

- **Dedup de relatórios**: comparamos os 6 keycodes com o relatório
  anterior e só emitimos *novas* imprensa — tecla segurada (ou dispositivo
  que ignora `set_idle(0)`) não vira flood dekeypress no editor.
- **Estado observável**: `usb_hid_keyboard_connected()` + callback
  `UsbKbdEventCallback(bool)` — a UI atualiza `has-keyboard` (base da
  decisão do teclado virtual no M2) e o log mostra conect/desconect.
- **Fim do flood de log**: hex-dump e log por relatório viraram `LOGD`;
  conect/desconect/tecla ficam em `LOGI`/`LOGD` conforme utilidade.
- **Tracking de handles** (`s_devs[]`): teardown fecha o que estiver
  aberto, sem vazar interface órfã entre ciclos de sleep.
- Mantido do protótipo (testado): filtro subclass/proto que aceita
  boot-protocol **e** HID genérico (KMK/CircuitPython), e o strip de
  Report ID de 1 byte (relatórios de 9 bytes).

## Matriz de teste

| # | Passo | Esperado |
|---|---|---|
| 1 | Boot **sem** teclado, plugar depois | `teclado conectado (subclass=.. proto=..)` + digita |
| 2 | Desplugar | `interface HID desconectada` + `teclado USB: ausente` |
| 3 | Standby → wake pelo BOOT | `teardown USB p/ standby...` → `reinstall USB apos wake...` |
| 4 | Digitar após o wake | teclas chegam ao editor |
| 5 | Segurar uma tecla | 1 dekeypress (sem flood), exceto repeat do próprio firmware do teclado |
| 6 | Boot com teclado já plugado | continua funcionando (regressão) |

## M5a — áudio USB: arquitetura (da revisão pré-M5, 2026-10-05)

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

## M5a.1 — áudio USB implementado (2026-10-06)

- Componente `espressif/usb_host_uac ==1.5.0` como **segundo client** da
  Host Library (o 1º é o HID; `usb_host_install` segue uma vez só na
  `usb_host_lib_task` do HID). O driver UAC roda a própria background
  task; nossos comandos play/stop vivem na task `au_play` (fila de 2).
- Fluxo: `TX_CONNECTED` → open curto p/ ler nome do produto → close;
  `audio_uac_play()` → open → `get_device_alt_param` escolhe o alt
  setting compatível com o WAV (PCM16, 1–2 ch, rate discreto/contínuo) →
  `device_start` → loop de `device_write` em chunks de 4 KB (timeout
  2 s) → stop/close no fim/stop/unplug.
- WAV round 1: PCM 16 bits 1–2 canais de `/pda/music/*.wav`; outro
  formato = estado "erro: …" na tela, sem crash.
- UI: tela Música (`screens/music.slint`) + tile do launcher habilitado
  (glifo music_note já estava no subset) + `AppState::Music` + restore
  de sessão "music". Estado/dispositivo/faixa via event cb →
  `invoke_from_event_loop`.
- Evidência offscreen: `render_music.png`.
- Round 2 (M5a.2): hub p/ teclado+DAC juntos
  (`CONFIG_USB_HOST_HUBS_SUPPORTED`), pause (suspend/resume), teclas de
  mídia, volume por device; M5b: decoder MP3 e playlists.

### M5a.1.1 — enumeração de devices compostos (QCY H3s, 2026-10-07)
`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` 256→1024: o config
descriptor do H3s (composite BT+UAC) excedia 256 B e a enumeração
morria em `CHECK_SHORT_CONFIG_DESC` antes de qualquer class driver
ver o device (por isso "não aparece erro" no UAC). ATENÇÃO: mudar o
default exige regenerar o `sdkconfig` (backup + rm + build), senão o
valor velho persiste.

### M5a.1.2 — QCY H3S: UAC1 aceito, crash por callback-context nosso (2026-10-07)
CORREÇÃO da hipótese inicial (que dizia UAC2): o H3S (3654:4a55) é
**UAC 1.0** — o driver emite `speaker UAC encontrado` e lê o nome
(`UAC device: "QCY H3S"`). As 4 interfaces Streaming são alts de
playback/capture UAC1, não evidência de UAC2.
O crash dos logs de 2026-10-07 (asserts `spinlock_acquire` e
`xQueueGenericSend` dentro de `usb_host_client_handle_events`, via
`uac_host_handle_events`) era **reentrância nossa**: o open/get_info/
close para ler o nome rodava DENTRO do callback do driver (contexto da
task de eventos do UAC). Cura v5.4.4: callback só enfileira evento;
task `au_evt` faz o open/getName/close.
Não-fatais aceitos: `Control Transfer Timeout` + `Failed to get volume
min/range` (query de volume que o H3S não responde a tempo).
Consequências práticas:
- M5a.1 testável COM o H3S (UAC1) — som real possível; alt setting
  precisa casar com o WAV (daí os dois tons de teste, 44k1 e 48k).
- HID do H3S (subclass 0/proto 0 = consumer control) segue anexando
  como "teclado" inofensivo; media keys via report descriptor = M5a.2.
- O teto de control transfer 1024 (M5a.1.1) segue necessário.

### M5a.1.3 — pilhas do player (2026-10-07)
`au_play` estourou 6144 B internos no primeiro play (Stack protection
fault em `_svfprintf_r`: FATFS + UAC + vprintf não cabem). Agora 10240 B
em PSRAM (`xTaskCreateWithCaps`), `au_evt` 6144 PSRAM; ambas no HWM do
power_mgmt. Ruído aceito: `uac stream interface not found` (E) ao
plugar device SEM classe de áudio (ex.: teclado) — é o driver dizendo
"não é comigo".

### M5a.2 (v5.6) — pause e media keys
- Pause: `uac_host_device_suspend/resume`; posição = offset do arquivo
  (loop de write espera em `s_paused`); stop/unplug furam a espera.
- Media keys: reports HID curtos (2–3 B, HUT Consumer 0x0C) parseados em
  `hid_keyboard_report_callback` ANTES do gate de 8 B do boot protocol;
  play/pause, next, prev, stop viram comandos do player; vol/mute logam
  "sem suporte" até o feature-detect de volume (v5.7). Teclados boot
  (8/9 B) intocados; a interface media de teclados completos (subclass 0)
  ganha função de graça.
