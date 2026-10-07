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
