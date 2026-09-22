# Roadmap — do M1 ao PDA completo

Cada milestone fecha com critério de aceite testável no hardware.
Nada de M(n+1) começa com pendência de M(n).

## M1 — Fundação (ESTE commit)
- [x] Pinagem reconciliada com a JC4880P443C_I_W (`board_config.h`, `docs/HARDWARE.md`)
- [x] SD como armazenamento principal: 4-bit @ 40 MHz, raiz lógica `/pda`, espelho de settings
- [x] Configurações em Lua (`config/system.lua`) + tela Config funcional
- [x] Runtime Lua protegido + tela Scripts (lista + console) + API `pda.*`
- [x] Gerenciador de arquivos navegável (entrar/sair de diretórios, abrir texto)
- [x] Notas (criar/editar/salvar/apagar) na raiz ativa
- [x] Energia em degraus: DIM → STANDBY (light sleep, wake botão/toque) → HIBERNATE
      (deep sleep c/ sessão restaurada); botões "Dormir agora"/"Hibernar"
- [x] Status bar (uptime, armazenamento, bateria "--", estado de energia)
- **Aceite**: boot sem cartão não trava; com cartão, tudo grava nele; standby
  acorda por botão; hibernate restaura a nota aberta; `exemplo.lua` roda sem derrubar.

## M2 — Edição de texto de verdade
- Editor com cursor visível/móvel (touch + setas do teclado), scroll nas duas direções
- **Teclado virtual** (touch) quando não houver HID (`ui.onscreen_keyboard_auto`)
- Abrir/salvar qualquer arquivo texto pelo gerenciador (não só `notes/`)
- Destaque leve para `.lua` (opcional)
- **Aceite**: editar um `.lua` pelo teclado virtual e executá-lo na tela Scripts.

## M3 — Arquivos avançados
- Hot-plug do SD (remontar sem reboot) + aviso na status bar
- Copiar/mover/renomear/apagar com confirmação; novo diretório
- Visualizador hex/imagem básica; ordenação e tamanho visível
- **Aceite**: trocar o cartão com o aparelho ligado e continuar navegando.

## M4 — Conectividade (Wi-Fi/BT via C6)
- Reflash do C6 (ESP-Hosted slave 2.12.x) — `tools/flash_c6_wifi.sh`
- `espressif/esp_hosted` no P4; STA + DHCP; NTP → relógio real na status bar
- Config de redes em Lua (`config/wifi.lua`) + tela de redes
- BT: começar por BLE (scan/announce); A2DP só se houver demanda real
- **Aceite**: hora certa após boot com Wi-Fi salvo; `pda.net.*` mínimo p/ scripts.

## M5 — Áudio & player de música
- ES8311 + NS4150: beep de UI primeiro (prova do caminho I2S/PA_EN)
- Player local do SD: WAV → MP3 (decoder) → filas/playlists (`.m3u`/Lua)
- Controles na UI + teclas de mídia do teclado USB
- **Aceite**: tocar um álbum do cartão com tela apagada (standby não mata o áudio).

## M6 — Apps de PDA
- Agenda/Contatos/Relógio (alarmes) com dados em Lua/CSV no cartão
- Integração alarme ↔ áudio (M5) e wake agendado
- **Aceite**: alarme dispara com o aparelho em standby.

## Dívidas técnicas conhecidas
- Medição real de corrente por degrau (`docs/POWER.md` → plano de medição)
- Teardown completo do DSI no standby se light sleep passar de ~40 mA
- Validação do INT do GT911 (GPIO21) para `wake_on_touch`
- Confirmar possibilidade de medir bateria (folha 05 do esquemático) ou mod de divisor
