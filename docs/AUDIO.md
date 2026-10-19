# AUDIO.md — dono do sistema de som (v5.16.0, 2026-10-17)

Este documento é o DONO das decisões do subsistema de áudio do PDA:
caminho local (ES8311 + NS4150), caminho USB (UAC host), roteamento,
player e políticas. USB.md mantém a arquitetura do HOST USB (HID/UAC/
hub); ROADMAP.md mantém as marcas de milestone; o plano de conclusão
abaixo é a fonte de trabalho do som (pedido do usuário na 15ª rodada:
"terminar o sistema de som, já que agora temos um alto-falante").

## 1. Estado atual (o que já existe e está validado em HW)

Dois caminhos independentes, sem roteamento entre eles ainda:

| Caminho | Arquivo | O que faz | Estado |
|---|---|---|---|
| Local I2S | `main/audio_i2s.c` | beep de UI 440 Hz (~1,2 s), senoide estéreo gerada in-loco; volume do codec (`audio_i2s_set_vol`, default `BOARD_SPK_VOL_DEFAULT`=60); ganho do beep `BOARD_BEEP_GAIN`=26000 | M5c.1/M5c.1b ✅ HW (13ª/14ª rodadas): beep sem duplicação (mutex), log no início do som |
| USB UAC | `main/audio_uac.c` | player WAV PCM16 (16 bits, 1–2 canais) p/ device UAC1 (QCY H3S validado); pause/resume; volume/mute com feature-detect; retomada pós-unplug (`s_resume_path`); fila de comandos `au_play` + eventos `au_evt` | M5a.1/M5a.2 ✅ HW (várias rodadas) |

Hardware (board_config.h / esquemático GUITION, folha 02):
- I2S0: MCLK 13, BCLK 12, LRCK 10, DOUT 9 (P4→codec), DIN 48 (codec→P4, mic reservado).
- ES8311 no I2C0 compartilhado com o GT911 (bus do `touch_init`); addr 0x30 (8-bit).
- NS4150 PA_EN GPIO11 — o `es8311_codec` do esp_codec_dev liga/desliga no open/close.
- Alto-falante real: unidade de telefone antigo, baixa sensibilidade (por isso
  vol 60 / ganho 26000 desde a v5.13.0).
- USB-A sem VBUS na bateria (USB5V_IN é entrada externa — ver USB.md/HARDWARE.md):
  o caminho UAC só existe com o carregador plugado ou JP1 VCC5V (KB-ALT-A).

Quirks do QCY H3S (evidência de campo, 16ª/17ª rodadas — não "consertar"):
- Índice 100 da curva de volume = MUTE no device → enviamos no máx. 99.
- 0% ainda é audível e 0–10% são indistinguíveis (curva do device); 0% vira mute real.
- SET volume funciona; GET range timeouta → nunca chamar GET no caminho de UI/stream.
- Media keys vendor (reports 2 B: 08 play/pause, 01 vol+, 02 vol−) + long-press
  next/prev interno do fone; tratados via `usb_hid_keyboard_set_media_cb`.

Threading (regra geral em ARCHITECTURE.md):
- Callbacks de driver NUNCA tocam na UI direto: passam por `ui_post()`
  (UI-LOOP-GATE, v5.16.0 — ver `main/ui_gate.h`; o crash da 15ª rodada foi
  exatamente um `invoke_from_event_loop` de evento UAC antes do loop).
- I/O de arquivo e writes longos em tasks próprias (`au_play`, prio 4, PSRAM);
  eventos de device em `au_evt` (nada abre stream em contexto de callback).
- Beep serializado por mutex: 2º clique na janela do 1º é descartado com log W.

## 2. Plano de conclusão do som (definido 2026-10-17 com o usuário)

Ordem de implementação = ordem das fases. Cada fase tem aceite próprio e
entra numa rodada de validação em HW antes da seguinte (disciplina do projeto).

### SOM.1 — Player local no alto-falante (WAV → I2S)   [próxima]
Hoje o alto-falante só bebe; a música só sai pelo USB. SOM.1 inverte:
- [ ] Extrair o parser WAV de `audio_uac.c` (`parse_wav`) p/ `main/wav_parse.[ch]`
      compartilhado (código puro → teste de host com WAVs sintéticos:
      fmt estendido, chunks ímpares, LIST/FACT antes do data).
- [ ] `audio_i2s_play(path)` / `stop` / `pause` / `resume`: task `spk_play`
      alimentando o canal do codec; reabrir o codec na taxa do arquivo
      (44,1k/48k no round 1; outras taxas → log e recusa honesta);
      mono→estéreo duplicando canal; write em blocos (~4 KB) com flag de stop.
- [ ] Beep × música no MESMO canal: beep durante play local = pause no write,
      beep, resume (o mutex do beep já existe; falta o handshake com o player).
- [ ] Tela Música: mesma lista, destino segue o seletor de SOM.2 (enquanto
      ele não existe: UAC se presente, senão local).
- Aceite HW: WAV 44k1 estéreo do cartão toca no falante com tela em DIM,
      stop/pause imediatos (<100 ms), beep de clique não corta a música
      permanentemente, SD removido no meio = stop limpo + estado "erro".
- Risco: ES8311 PLL fora de 48k (validar 44,1k cedo no bring-up da fase).

### SOM.2 — Seletor de saída (M5c.2): interno | UAC | auto
- [ ] Setting `audio.out` em `config/system.lua` (default "auto"), clampado
      pelo parser do pda_config; linha nova em Config → Tela/Som:
      três botões de texto (largura intrínseca — regra de UI.md, nunca width fixo).
- [ ] `audio_route.[ch]`: fonte única da decisão (auto = UAC se
      `audio_uac_present()`, senão interno); hot-plug UAC reavalia e migra a
      faixa corrente (stop no destino velho + play no novo a partir do offset —
      `s_resume_off` já existe no UAC; espelhar no local).
- [ ] Política de beep (decisão registrada): beep segue SEMPRE o interno —
      feedback de UI não deve exigir fone plugado; música segue o seletor.
- [ ] Decisão de mix documentada aqui (fecha o "mix de decisão" que o
      ROADMAP M5c devia a USB.md/HARDWARE.md).
- Aceite HW: alternar interno↔UAC com música tocando retoma no mesmo ponto
      (±2 s); auto escolhe certo nos 4 casos (fone plugado/desplugado ×
      carregador/bateria); beep sempre no falante.

### SOM.3 — Volume local real (M5c.3)
- [ ] Slider de volume do falante em Config → Som (0–100 → `audio_i2s_set_vol`),
      persistido (`audio.spk_vol`), default atual 60; aplicar no boot.
- [ ] Teclas de mídia do H3S/teclado com saída local: vol±/mute locais
      (hoje só mandam p/ o device UAC); mesma feature-detect de mute.
- [ ] Beep usa o volume do player (um único knob do usuário; `BOARD_BEEP_GAIN`
      vira calibração interna, não preferência).
- Aceite HW: slider muda o volume ao vivo sem estalo audível grave; 0 = mudo
      de verdade (PA desligado, não só gain 0 — medir corrente se possível).

### SOM.4 — Acervo: MP3 + playlists (M5b)
- [ ] Decoder MP3 fixed-point: candidato 1 = componente do registry
      (verificar `espressif/esp_audio_effects` ou libhelix empacotado; PINAR
      versão + DEPENDENCIAS.md); candidato 2 = fontes helix-mp3 vendored
      (licença compatível, sem float). Decidir com teste de host ANTES de
      integrar (decode de arquivo-referência → PCM comparado no host).
- [ ] Abstração `audio_sink` (write/stop/pause + taxa/canais): UAC e I2S
      implementam; player único (fila de decode alimenta o sink ativo) —
      não duplicar player por formato/destino.
- [ ] Playlists `.m3u` simples em `/pda/music/` (caminhos relativos; parser
      puro → teste de host) + "Tocar pasta" (fila = diretório ordenado) +
      repetir/aleatório mínimo (off/todas/uma — pendência antiga da M5a.2).
- [ ] Metadata: nome de arquivo no round 1; ID3v2 depois (fora de escopo).
- Aceite HW: álbum MP3 do cartão toca no falante E no H3S (via seletor) com
      tela em standby; troca de faixa sem gap audível > 300 ms; CPU < 60%
      num core durante decode+play (medir com `vTaskGetRunTimeStats` se disponível).

### SOM.5 — Energia × áudio (fecha com o plano de bateria)
- [ ] Medir corrente do PA tocando (multímetro do usuário em série): perfil
      p/ autonomia (POWER.md §BAT.3) e p/ o limiar de auto-desligue do IP5306
      (aceite herdado do M5c original: "alto-falante com bateria e USB
      desplugado" — hoje impossível sem KB-ALT/VBUS: documentar o limite real).
- [ ] Política de standby: música tocando segura o DIM (tela apaga, som
      segue) e INIBE o hibernate automático; light sleep × I2S — medir antes
      de prometer (o DWC2 não sobrevive ao light sleep; o I2S pode ter o
      mesmo problema — se tiver, música inibe standby profundo).
- [ ] Sessão: hibernate manual com música = stop limpo + salvar posição
      (retomar é SOM.6, não obrigatório).
- Aceite HW: 30 min de música no falante com tela apagada sem reset/brownout;
      log de bateria mostra o sag do PA (e o % NÃO dança — BAT.1c já filtra).

### SOM.6 — Conforto (esticáveis, só depois de SOM.1–5 validados)
- [ ] Sleep timer (15/30/60 min → fade + stop).
- [ ] Audiobook: posição por faixa persistida (NVS), retoma no plug.
- [ ] Gapless WAV/MP3 (double-buffer de decode) se SOM.4 mostrar gap > 300 ms.
- [ ] Mic/line-in (DIN 48 já mapeado): gravação de nota de voz (fora do plano
      atual — registrar que o hardware existe).

## 3. Fora de escopo (registrado para não renegociar sem evidência)
- A2DP/Bluetooth: o C6 é Wi-Fi-only nesta placa (BLUETOOTH.md).
- Dolby/EQ: sem DSP disponível; se um dia entrar, é no sink, não no player.
- Multi-dispositivo UAC simultâneo (2 fones): hub é M5a.3, mas 2 streams
  UAC1 ao mesmo tempo não é objetivo.
