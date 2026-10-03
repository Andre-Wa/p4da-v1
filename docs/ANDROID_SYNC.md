# App companheiro Android — sync de notas e dados (design)

Estado: **proposta** (2026-09-30). Sem código ainda — este documento é o
plano para o usuário revisar antes de qualquer milestone. A ideia do
usuário: um app Android que sincroniza notas/dados com o PDA "como um
smartwatch" (ping, hora, presença, capture rápido). Sugestões deste doc
respondem à pergunta "como seria essa conexão?".

## 1. Princípio: dois canais complementares, não um só

| Canal | Papel | Por quê |
|---|---|---|
| **Wi-Fi LAN (HTTP/REST)** | Volume: notas, árvores de arquivos, backup, config | Já funciona (M4a). `esp_http_server` é nativo do IDF. Dezenas de KB/s–MB/s. Sem reflash do C6. |
| **BLE GATT (C6 via ESP-Hosted HCI/SDIO)** | Presença: ping, hora, notifica, capture rápido, descoberta do IP | Funciona sem roteador (rua, campo). Baixa energia. Já pesquisado e viável (`docs/BLUETOOTH.md`, M4c). MTU 512 + DLE ≈ 10–50 KB/s — ok p/ texto, ruim p/ volume. |

Regra de bolso: **BLE encontra e cumprimenta; Wi-Fi carrega.** O app
Android escuta o advertisement BLE do PDA (nome `p4da-XXXX`), recebe por
GATT o IP/mDNS e o estado, e faz o sync pesado por HTTP quando ambos estão
na mesma rede. Sem Wi-Fi comum, o GATT vira fallback lento (só notas
pequenas).

## 2. Fases (cada uma entrega valor sozinha)

### F0 — PDA como servidor WebDAV (SEM app próprio)
- `esp_http_server` + rotas WebDAV mínimas (PROPFIND/GET/PUT/DELETE/MKCOL)
  sobre `/sdcard/pda/notes`.
- No telefone: qualquer cliente pronto (Solid Explorer, Roundcube-style
  WebDAV, rclone/Termux, Obsidian+WebDAV plugin).
- Valor imediato com esforço mínimo e zero código Android. Serve também de
  "prova de carga" do servidor HTTP antes da API própria.
- Risco: WebDAV completo tem cantos (locks, chunked). Manter subconjunto.

### F1 — API REST própria + descoberta + pareamento
- Endpoints (JSON, `esp_http_server`, porta 80 na LAN; TLS só em F4):

| Método/Rota | Função |
|---|---|
| `GET /api/v1/status` | versão firmware, bateria (quando existir), hora, modo |
| `GET /api/v1/tree?dir=notes` | lista `{path,size,mtime,fnv1a}` recursiva |
| `GET /api/v1/file?path=...` | conteúdo (ETag = fnv1a; `If-None-Match` → 304) |
| `PUT /api/v1/file?path=...` | grava (`If-Match` obrigatório p/ evitar overwrite cego) |
| `DELETE /api/v1/file?path=...` | apaga |
| `POST /api/v1/pair` | body = código de 6 dígitos exibido na tela do PDA → devolve token |
| `GET /api/v1/config` / `PUT` | subconjunto editável de `system.lua` (brilho, dim, TZ…) |

- Descoberta: `espressif/mdns` → `p4da.local` (verificar coexistência com
  esp_hosted/esp_wifi_remote; fallback: IP fixo ou BLE F3 informa).
- Pareamento: PDA mostra código na tela (Slint) por 60 s; app faz `POST
  /pair`; token em header `Authorization: Bearer` daí em diante. Sem par →
  só `GET /status`. Simples, offline, sem contas.

### F2 — App Android MVP (Kotlin + Jetpack Compose)
- Telas: Parear (código/QR) · Notas (árvore, pull-to-refresh) · Editor
  (Markdown simples, preview) · Conflitos · Ajustes (endereço, auto-sync).
- Sync = algoritmo da §3. Roda em foreground enquanto a tela está aberta;
  `WorkManager` p/ sync periódico (F4).
- Export p/ telefone via SAF (pasta do Obsidian, p.ex.) — notas do PDA
  viram `.md` no vault do usuário.
- minSdk 26; permissões: `INTERNET`, `ACCESS_NETWORK_STATE` (+ BLE em F3:
  `BLUETOOTH_SCAN/CONNECT`, `CompanionDeviceManager`).

### F3 — Canal BLE (casa com o M4c já escopado)
- GATT service `P4DA` (UUID próprio):
  - `STATUS` (notify): bateria, app ativa, tela on/off — "presença".
  - `TIME` (write): telefone empurra a hora (cura relógio sem NTP).
  - `CAPTURE` (write): texto curto do telefone → `notes/inbox/*.md` no PDA
    (captura rápida com o PDA no bolso/standby).
  - `WIFI_INFO` (read): ssid/ip/pareado — bootstrapping do canal HTTP.
- Advertisement com nome `p4da-XXXX` + service UUID; app usa
  `CompanionDeviceManager` (pareamento do Android) e assina `STATUS`.

### F4 — Robustez
- TLS (self-signed + pinning do token) ou WireGuard/tailnet do usuário —
  decidir depois; LAN-first já é útil.
- Auto-sync em background (WorkManager + notificação de conflito).
- Glance widget no telefone: "capturar → PDA".
- OTA de scripts Lua pelo app (PUT `scripts/*.lua` já coberto pela F1).

## 3. Algoritmo de sync (proposto)

Estado por arquivo nos dois lados: `(path, mtime, fnv1a)`.

> **Caveat `mtime` (visto no log de aceite do M4.11b):** gravações feitas
> antes do primeiro sync NTP do boot levam época baseada em 1980 (LittleFS sem
> relógio — ex.: `mtime=315533004`). O sync NÃO pode confiar em `mtime`
> isoladamente: comparar sempre `fnv1a`+`size` primeiro, e tratar
> `mtime < 2020-01-01` como "desconhecida" (a válida vence). Mitigação no
> firmware (F1): se `time(NULL) < 1577836800`, carimbar gravações com o
> mtime da leitura anterior ou omitir do manifesto.

Três casos:

1. **Só um lado mudou** (fnv difere da última base comum) → copia.
2. **Nenhum mudou** → nada (304).
3. **Ambos mudaram** → conflito: mantém os dois
   (`x.md` + `x.conflito-<data>.md`) e lista na tela Conflitos do app.
   Nunca overwrite silencioso.

Base comum: o app guarda um manifesto `{path: fnv}` do último sync (SQLite
local). Deletes: manifesto sem o path + PDA com o path = apagado num lado →
regra configurável (padrão: re-baixa; apagar exige confirmação). Arquivos
grandes (>256 KB) em chunks com `Range`.

`fnv1a` já existe no firmware (`pda_config.c`) — reusar como função pública
(`storage_fnv1a`) para tree/ETags.

## 4. Orçamento no firmware (o que custa quê)

- `esp_http_server`: ~30–40 KB RAM por instância enxuta (task stack +
  sockets); roda sob demanda (tela ligada / Wi-Fi up), dorme no standby.
- `espressif/mdns`: leve (~10 KB); testar com esp_wifi_remote.
- NimBLE host no P4 (M4c): ~50–70 KB RAM (já orçado em `BLUETOOTH.md`).
- Flash: http_server+mdns+API ≈ 150–250 KB — cabe folgado no factory de 6 MB
  (imagem atual ~3,7 MB).
- Concorrência: sync durante edição é ok (escritas são atômicas via
  `storage_write_text_file` + rename a adicionar se necessário).

## 5. Alternativas avaliadas (e por que não)

- **Syncthing no PDA**: inviável (Go/peso) — existe porte C? nada maduro p/ IDF.
- **MQTT + broker na nuvem**: depende de internet e de servidor de
  terceiros; o caso de uso é LAN-first. Pode voltar como extra (F5) para
  "penseiro" remoto.
- **Só BLE (sem Wi-Fi)**: throughput limita sync de árvore inteira a
  minutos; ok apenas p/ presença + capture (por isso F3 é complemento).
- **App próprio desde o dia 1**: pular F0/F1 atrasa o primeiro valor real;
  WebDAV/REST primeiro também des-risca o protocolo antes do app.

## 6. Encaixe no roadmap

Ordem sugerida (respeitando as prioridades do usuário — gestos primeiro):

1. M4.12 gestos + pulldown (já proposto)
2. M4c BLE GATT mínimo (STATUS/TIME/WIFI_INFO) — habilita F3 e testa a
   receita NimBLE do `BLUETOOTH.md`
3. M4.13 = F0+F1 (WebDAV mínimo + REST + pareamento na tela)
4. F2 (app Android MVP) — fora do firmware; pode andar em paralelo
5. F3/F4 quando o resto estiver estável

## 7. Perguntas em aberto → DECIDIDO (feedback de 2026-10-02)

- **Nome do dispositivo BLE**: `PDA-P4`, editável depois (chave em
  `system.lua`, ex.: `net.ble_name`, default `PDA-P4` — o nome do projeto,
  `p4da`, é a junção). Nome do app: fica a critério do repo do app.
- **Pareamento SEM QR/câmera no MVP.** QR exigiria permissão de câmera ou
  intent externa no celular; decisão: o app descobre o PDA por **scan BLE**
  (lista dispositivos `PDA-P4*`, toque pareia/bonda via GATT). QR fica como
  opcional pós-MVP (F4), se um dia fizer sentido.
- **Conexão/sync no MVP: BLE primeiro** (presença + controle + arquivos
  pequenos), Wi‑Fi REST/WebDAV como canal de volume depois — o desenho de
  dois canais (§1) se mantém, mas a ORDEM de implementação inverte: F3
  (BLE) sobe para antes de F0/F1 no lado do app; o firmware já tem o
  caminho BLE documentado em `docs/BLUETOOTH.md` (M4c).
- **Escopo de arquivos internos**: `notas/`, `scripts/` e `config/`
  (system.lua/wifi.lua). Conflitos: manter os dois (§3) confirmado como
  padrão.
- **Ordem geral**: sistema de áudio + player (M5) vem ANTES do app; como
  tudo vive no cartão, o acesso manual cobre o intervalo. App fica em
  M7/M8.
- **Onde desenvolver o app**: repo + conversa SEPARADOS (decisão do
  usuário). Este ambiente/sandbox NÃO é adequado p/ o app (sem Android
  SDK/emulator/Gradle Android; o toolchain Android é pesado e o ciclo
  emulator não fecha aqui) — o app nasce no Android Studio da máquina do
  usuário; este lado mantém o CONTRATO (este doc) e os endpoints de
  firmware (F0/F1/M4c) quando começarem.
