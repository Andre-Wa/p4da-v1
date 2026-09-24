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

## M2 — Edição de texto de verdade (FEITO, exceto destaque)
- [x] Editor com cursor visível ("|" na linha de exibição) e móvel:
      setas/Home/End/Delete no teclado USB, toque posiciona o cursor no
      FIM da linha tocada, OSK tem setas/Home? (setas + del; Home/End via
      teclado físico), scroll vertical e horizontal (Flickable)
- [x] **Teclado virtual** em 4 fileiras + shift + espaço/enter/backspace/
      setas/del, automático quando `onscreen_keyboard_auto && !USB`,
      ou manual pelo botão "Tecl" (tri-estado: auto/força-on/força-off)
- [x] Gerenciador abre qualquer arquivo texto (.txt .lua .md .csv .log
      .ini .json) no editor; Salvar grava de volta no mesmo caminho
- [x] "Apagar" só aparece para arquivos dentro de `notes/` (segurança)
- [ ] Destaque leve para `.lua` — DEFERIDO (opcional no escopo original)
- **Aceite**: editar um `.lua` pelo teclado virtual e executá-lo na tela
  Scripts.
- Limitações conhecidas (documentadas, não bugs): o toque posiciona a
  LINHA (coluna via setas/OSK); largura de scroll horizontal é estimada
  (10 px/char p/ fonte 16 px); o editor instancia um elemento por linha
  (Flickable), então arquivos com milhares de linhas ficam lentos —
  virtualização fica p/ M3.

## M2.5 — Design system + modularização da UI (FEITO)
- [x] `app_ui.slint` monolítico -> módulos: `theme.slint` (tokens M3-dark
      adaptados), `elements/` (botões/superfícies/OSK), `screens/` (1 tela/arquivo)
- [x] Padronização visual: state layers, elevação tonal, type scale,
      botões por papel (filled/tonal/danger/ghost) — ver `docs/UI.md`
- [x] API pública da `AppWindow` (contrato com o C++) intacta

## M3a — Ícones (FEITO)
- [x] Subset Material Icons (3,4 KB, Apache-2.0) + `elements/icons.slint`
      (`IconForKind` com literais estáticos; C++ envia só categoria)
- [x] Ícones por tipo nas listas + ícone de armazenamento na status bar

## M3b — Operações de arquivo (FEITO, aguardando validação em hardware)
- [x] Sheet de ações por entrada (⋮): abrir / renomear / copiar / mover / apagar
- [x] Prompt com OSK (renomear, novo diretório) e confirmação p/ apagar
      (apaga recursivo p/ diretórios)
- [x] Copiar/mover com seletor de destino (modo picker na própria lista);
      não sobrescreve destino existente (falha com aviso no console)
- [x] Voltar/Sobe fecham overlays em camadas (prompt > sheet > picker)

## M3c — Pendentes do M3
- [ ] Hot-plug do SD (remontagem a quente + aviso na status bar)
- [ ] Virtualização do editor (arquivos com milhares de linhas)

## M3 — Arquivos avançados
- Hot-plug do SD (remontar sem reboot) + aviso na status bar
- Copiar/mover/renomear/apagar com confirmação; novo diretório
- Visualizador hex/imagem básica; ordenação e tamanho visível
- **Aceite**: trocar o cartão com o aparelho ligado e continuar navegando.

## M4a — Wi-Fi STA + NTP (FEITO, aguardando flash do C6 + validação)
- [x] `espressif/esp_hosted ==2.12.9` + `esp_wifi_remote` (par do slave 2.12.9)
- [x] `main/wifi_net.c`: STA + reconexão automática + SNTP + relógio HH:MM
      local na status bar (uptime até sincronizar) + RSSI na status bar
- [x] `config/wifi.lua` (ssid/password/auto_connect); sem arquivo = offline
- [x] docs/WIFI.md: procedimento do C6, limitação standby×SDIO, segurança
- [ ] M4b: tela de redes/scan + Bluetooth

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

### M3b.1 — Correções da validação em hardware (2026-09-23)
- [x] Picker: `Sobe` navega (antes cancelava o seletor, prendendo o usuário
      em subdiretórios); overlays fecham só com prompt/sheet
- [x] Histórico de diretórios: `Voltar` em Arquivos desfaz navegação
      (incluindo Sobe) antes de sair da tela
- [x] Pilha de navegação de apps (`nav_goto`/`nav_back`): Launcher →
      Arquivos → Editor → Voltar retorna p/ Arquivos (não p/ Launcher)
- [x] "Novo arq" no cabeçalho: cria arquivo vazio e já abre no editor
- [x] `pda_config_init`: se `system.lua` estiver ausente na raiz ativa,
      promove o espelho da outra raiz ANTES de cair em defaults (nunca
      "esquece" config por boot com uma raiz ausente); log explícito de
      promoção/criação no boot

## Backlog / issues conhecidas
- **USB primeira enumeração**: `USBH: Dev 1 EP 0 Error` +
  `ENUM: CHECK_FULL_DEV_DESC FAILED` no hot-plug/boot com teclado (ramp de
  energia); retry interno do IDF recupera em alguns segundos. No unplug,
  `EP command error: ESP_ERR_INVALID_STATE` (corrida de teardown do host
  lib). Workaround: reconectar. Futuro: retry de enumeração próprio
  (liberar dispositivo e re-enumerar) — candidato a M4.
- **system.lua envenenado com INT_MAX** (herança do bug de NaN): curado em
  2026-09-23 com saneamento+regravação única no boot (pda_config.c).
