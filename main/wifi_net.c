/**
 * wifi_net.c — Wi-Fi STA + NTP sobre o coprocessor C6 (ESP-Hosted SDIO).
 *
 * Sequência espelhada do exemplo oficial host_wifi_itwt (esp-hosted-mcu
 * 2.12.x): a API esp_wifi "normal" funciona porque esp_wifi_remote
 * proxy-iza tudo para o C6. NVS é obrigatório p/ esp_wifi.
 */

#include "wifi_net.h"
#include "storage_init.h"
#include "pda_config.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua.h"
#include "lauxlib.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "wifi_net";

static bool s_auto = false;
static bool s_connected = false;
static bool s_synced = false;
static volatile bool s_paused = false;   /* M4.14: standby pausa reconexão/NTP */
/* M5.0b (A1): backoff exponencial em task própria — o event loop NÃO
 * bloqueia mais 2 s por retry, e fora de casa o martelo vira 2/4/8/16/30 s. */
static int s_backoff_s = 0;
static TaskHandle_t s_recon_task = NULL;
/* M5.2: multi-redes salvas. bad = auth falhou (senha); noap = AP não
 * achado no scan desta rodada; rotação por reason de disconnect. */
#define MAX_SAVED 8
typedef struct { char ssid[33]; char pass[65]; uint8_t bad; uint8_t noap; } saved_net_t;
static saved_net_t s_nets[MAX_SAVED];
static int s_nets_n = 0;
static int s_cur = -1;        /* índice em tentativa */
static int s_last_good = 0;   /* última conectada (ponto de partida) */
/* M5.3 (fluxo Android): orçamento de tentativas POR rede antes de
 * rotacionar; scan dirige a escolha; espera sem martelo se nenhuma
 * salva estiver presente; chave de rádio on/off pela UI. */
#define TRY_BUDGET 3
static uint8_t s_auth_hits[MAX_SAVED];
static int s_attempts = 0;
static bool s_enabled = true;
static bool s_sntp_on = false;
static char s_ssid[33] = { 0 };
static char s_pass[65] = { 0 };
static char s_hhmm[8] = { 0 };

/* IANA -> TZ POSIX (tabela curta; fallback UTC). Sem DST onde não há. */
static const char *tz_for(const char *iana)
{
    if (!strcmp(iana, "America/Sao_Paulo")) return "<-03>3";
    if (!strcmp(iana, "America/Manaus"))    return "<-04>4";
    if (!strcmp(iana, "America/Noronha"))   return "<-02>2";
    if (!strcmp(iana, "Europe/Lisbon"))     return "WET0WEST1,M3.5.0,M10.5.3";
    if (!strcmp(iana, "Europe/Berlin"))     return "CET-1CEST2,M3.5.0,M10.5.3";
    if (!strcmp(iana, "America/New_York"))  return "EST5EDT4,M3.2.0,M11.1.0";
    if (!strcmp(iana, "UTC"))               return "UTC0";
    return "UTC0";
}

/* ---------------- config/wifi.lua ---------------- */
static bool load_wifi_lua(void)
{
    char path[160];
    if (pda_path(path, sizeof(path), "config/wifi.lua") != ESP_OK) return false;
    if (!storage_file_exists(path)) {
        ESP_LOGI(TAG, "sem config/wifi.lua — Wi-Fi desativado");
        return false;
    }
    lua_State *L = luaL_newstate();
    if (!L) return false;
    bool ok = false;
    if (luaL_loadfile(L, path) == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK &&
        lua_istable(L, -1)) {
        lua_getfield(L, -1, "ssid");
        if (lua_isstring(L, -1)) {
            snprintf(s_ssid, sizeof(s_ssid), "%s", lua_tostring(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "password");
        if (lua_isstring(L, -1)) {
            snprintf(s_pass, sizeof(s_pass), "%s", lua_tostring(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "auto_connect");
        s_auto = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : true;
        lua_pop(L, 1);
        /* M5.2: lista de redes; legado ssid/password vira entrada 1. */
        lua_getfield(L, -1, "networks");
        if (lua_istable(L, -1)) {
            for (int i = 1; s_nets_n < MAX_SAVED; i++) {
                lua_rawgeti(L, -1, i);
                if (!lua_istable(L, -1)) { lua_pop(L, 1); break; }
                saved_net_t *e = &s_nets[s_nets_n];
                memset(e, 0, sizeof(*e));
                lua_getfield(L, -1, "ssid");
                if (lua_isstring(L, -1))
                    snprintf(e->ssid, sizeof(e->ssid), "%s", lua_tostring(L, -1));
                lua_pop(L, 1);
                lua_getfield(L, -1, "password");
                if (lua_isstring(L, -1))
                    snprintf(e->pass, sizeof(e->pass), "%s", lua_tostring(L, -1));
                lua_pop(L, 1);
                lua_pop(L, 1);               /* a entrada */
                if (e->ssid[0]) s_nets_n++;
            }
        }
        lua_pop(L, 1);
        if (s_nets_n == 0 && s_ssid[0]) {
            snprintf(s_nets[0].ssid, sizeof(s_nets[0].ssid), "%s", s_ssid);
            snprintf(s_nets[0].pass, sizeof(s_nets[0].pass), "%s", s_pass);
            s_nets_n = 1;
        }
        ok = true;   /* arquivo válido: sobe a pilha mesmo sem ssid (scan) */
    } else {
        ESP_LOGW(TAG, "config/wifi.lua inválido — Wi-Fi desativado");
    }
    lua_close(L);
    if (ok) ESP_LOGI(TAG, "wifi.lua: %d rede(s) salva(s) auto=%d",
                     s_nets_n, (int)s_auto);
    return ok;
}

/* Copia p/ buffers fixos do wifi_config sem truncamento silencioso
 * (satisfaz -Werror=stringop-truncation: clamp + NUL explícito). */
static void copy_wifi_str(char *dst, size_t cap, const char *src)
{
    size_t l = strlen(src);
    if (l > cap - 1) l = cap - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

/* ---------------- eventos ---------------- */
extern void wifi_net_on_event_ui(bool connected, int rssi);  /* main.cpp */

/* M5.2: forward p/ o handler de STA_START (definição após os eventos,
 * junto de pick_next/recon_task). */
static void apply_saved(int i);

static void sntp_synced(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    /* M5.0b (A3): persiste o último UTC bom p/ semear o relógio no boot
     * sem rede (mtime FAT 1980 / status bar sem HH:MM). */
    nvs_handle_t h;
    if (nvs_open("pdawifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "last_utc", (uint32_t)time(NULL));
        nvs_commit(h);
        nvs_close(h);
    }
    setenv("TZ", tz_for(pda_settings()->timezone), 1);
    tzset();
    ESP_LOGI(TAG, "hora sincronizada via NTP (TZ=%s)", pda_settings()->timezone);
}

/* M4.14: fatorado p/ o pause/resume de standby. DEPOIS de sntp_synced:
 * C exige declaração antes do uso — o build do usuário de 2026-10-04
 * pegou esta função na frente da definição do callback. */
static void sntp_start(void)
{
    /* IDF 5.5: API legacy do esp_sntp (sem config struct) */
    esp_sntp_setservername(0, pda_settings()->ntp_server);
    esp_sntp_set_time_sync_notification_cb(sntp_synced);
    esp_sntp_init();
    s_sntp_on = true;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) return;
    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA up (ssid=%s)", s_ssid[0] ? s_ssid : "-");
        if (s_recon_task) xTaskNotifyGive(s_recon_task);   /* M5.3: scan primeiro */
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        if (s_connected) {
            s_connected = false;
            wifi_net_on_event_ui(false, 0);
        }
        if (s_paused) {
            /* M4.14: standby não fica martelando reconexão (C6 acordado
             * à toa); o resume() reconecta ao acordar. */
            ESP_LOGI(TAG, "desconectado em standby — reconexão pausada (M4.14)");
            break;
        }
        /* M5.2: reason dirige a rotação entre redes salvas: auth =
         * senha errada NESSA rede (marca bad); NO_AP_FOUND = AP fora
         * (marca noap). Com uma rede só, comporta como antes. */
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *)data;
        const int r = disc ? (int)disc->reason : 0;
        const bool auth = (r == WIFI_REASON_AUTH_FAIL ||
                           r == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                           r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);
        if (s_cur >= 0 && auth) {
            /* M5.3: AUTH_FAIL espúrio acontece (rede boa!); só marca a
             * rede após TRY_BUDGET falhas de auth na mesma rede. */
            s_auth_hits[s_cur]++;
            if (s_auth_hits[s_cur] >= TRY_BUDGET) {
                s_nets[s_cur].bad = 1;
                ESP_LOGW(TAG, "AUTH_FAIL x%d em \"%s\": senha errada? "
                              "rede marcada p/ este ciclo",
                         (int)s_auth_hits[s_cur], s_nets[s_cur].ssid);
            } else {
                ESP_LOGW(TAG, "AUTH_FAIL (reason %d) em \"%s\": "
                              "tentativa %d/%d na mesma rede",
                         r, s_nets[s_cur].ssid,
                         (int)s_auth_hits[s_cur], TRY_BUDGET);
            }
        } else if (s_cur >= 0 && r == WIFI_REASON_NO_AP_FOUND) {
            s_nets[s_cur].noap = 1;      /* some do scan: roda já */
            s_attempts = TRY_BUDGET;
        } else {
            s_attempts++;                /* falha genérica conta p/ roda */
        }
        if (s_auto && s_recon_task) xTaskNotifyGive(s_recon_task);
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_connected = true;
        s_backoff_s = 0;      /* M5.0b: conectou zera backoff */
        s_attempts = 0;       /* M5.3: e o orçamento de tentativas */
        if (s_cur >= 0) {     /* M5.2: rede atual volta a ser confiável */
            s_nets[s_cur].bad = 0;
            s_nets[s_cur].noap = 0;
            s_auth_hits[s_cur] = 0;
            s_last_good = s_cur;
        }
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        wifi_net_on_event_ui(true, wifi_net_rssi());
        if (!s_synced) sntp_start();
    }
}

/* ---------------- task ---------------- */
static void apply_saved(int i)
{
    s_cur = i;
    snprintf(s_ssid, sizeof(s_ssid), "%s", s_nets[i].ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", s_nets[i].pass);
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    copy_wifi_str((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), s_nets[i].ssid);
    copy_wifi_str((char *)cfg.sta.password, sizeof(cfg.sta.password), s_nets[i].pass);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    ESP_LOGI(TAG, "rede salva %d/%d: \"%s\"", i + 1, s_nets_n, s_nets[i].ssid);
}

/* M5.3: escolha dirigida por SCAN (passo 1-3 do fluxo pedido): varre
 * bloqueante no contexto da recon_task e devolve a rede salva PRESENTE
 * de melhor RSSI (não-bad). -1 = nenhuma presente; -2 = scan ocupado.
 * Efeito colateral bom: rede que voltou limpa o próprio noap. */
static int scan_pick_best(int *out_rssi)
{
    wifi_ap_record_t *aps = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * 32);
    if (!aps) return -1;
    uint16_t n = 32;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) { free(aps); return -2; }
    if (esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK) n = 0;
    int best = -1, best_rssi = -128;
    for (int k = 0; k < s_nets_n; k++) {
        if (s_nets[k].bad) continue;
        for (uint16_t a = 0; a < n; a++) {
            if (!strcmp((const char *)aps[a].ssid, s_nets[k].ssid)) {
                s_nets[k].noap = 0;
                if (aps[a].rssi > best_rssi) { best_rssi = aps[a].rssi; best = k; }
                break;
            }
        }
    }
    free(aps);
    if (out_rssi) *out_rssi = best_rssi;
    return best;
}

/* próxima candidata: prefere !bad && !noap girando a partir de s_cur;
 * sem nenhuma, limpa os noap (APs podem ter voltado) e aceita !bad;
 * todas bad (senhas) => -1 = suspende até ação do usuário. */
static int pick_next(void)
{
    for (int k = 1; k <= s_nets_n; k++) {
        int i = (s_cur + k) % s_nets_n;
        if (!s_nets[i].bad && !s_nets[i].noap) return i;
    }
    bool any_noap = false;
    for (int i = 0; i < s_nets_n; i++) if (s_nets[i].noap) any_noap = true;
    if (any_noap) {
        for (int i = 0; i < s_nets_n; i++) s_nets[i].noap = 0;
        for (int k = 1; k <= s_nets_n; k++) {
            int i = (s_cur + k) % s_nets_n;
            if (!s_nets[i].bad) return i;
        }
    }
    return -1;
}

static void recon_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_enabled || s_paused || s_connected || !s_auto || !s_nets_n)
            continue;
        /* M5.3: dentro do orçamento, insiste na MESMA rede (transiente/
         * auth<3); esgotou ou AP sumiu → scan e melhor presente. */
        const bool stay = s_cur >= 0 && !s_nets[s_cur].bad &&
                          !s_nets[s_cur].noap && s_attempts < TRY_BUDGET;
        int idx = -1, rssi = 0;
        if (stay) {
            idx = s_cur;
        } else {
            idx = scan_pick_best(&rssi);
            if (idx == -2) {
                ESP_LOGI(TAG, "scan ocupado — reavaliando em 5 s");
                vTaskDelay(pdMS_TO_TICKS(5000));
                xTaskNotifyGive(s_recon_task);
                continue;
            }
            if (idx < 0) {
                ESP_LOGW(TAG, "nenhuma rede salva presente — Wi-Fi em "
                              "espera até ação manual (Redes/chave/rescan)");
                continue;
            }
        }
        s_backoff_s = (idx == s_cur && s_attempts)
            ? (s_backoff_s ? (s_backoff_s * 2 > 30 ? 30 : s_backoff_s * 2) : 2)
            : 2;
        ESP_LOGI(TAG, "reconexão em %d s — rede \"%s\" (tentativa %d/%d%s)",
                 s_backoff_s, s_nets[idx].ssid, s_attempts + 1, TRY_BUDGET,
                 stay ? "" : ", escolhida por scan");
        vTaskDelay(pdMS_TO_TICKS((uint32_t)s_backoff_s * 1000U));
        if (!s_enabled || s_paused || s_connected) continue;
        if (idx != s_cur) { apply_saved(idx); s_attempts = 0; }
        esp_wifi_connect();
    }
}

static void wifi_task(void *arg)
{
    (void)arg;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wcfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init falhou (C6 ligado? firmware slave 2.12.x "
                      "flashed? ver tools/flash_c6_wifi.sh)");
        wifi_net_on_event_ui(false, 0);
        vTaskDelete(NULL);
        return;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL);

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    copy_wifi_str((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), s_ssid);
    copy_wifi_str((char *)cfg.sta.password, sizeof(cfg.sta.password), s_pass);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (!s_enabled) {   /* M5.3: chave OFF = rádio não sobe no boot */
        ESP_LOGI(TAG, "Wi-Fi desligado (chave) — rádio não sobe");
        vTaskDelete(NULL);
        return;
    }
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start falhou");
        wifi_net_on_event_ui(false, 0);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi inicializado (host P4 <-> C6 via SDIO)");
    vTaskDelete(NULL);
}

void wifi_net_set_enabled_boot(bool on) { s_enabled = on; }  /* pré-init */

void wifi_net_set_enabled(bool on)
{
    if (on == s_enabled) return;
    s_enabled = on;
    if (!on) {
        ESP_LOGI(TAG, "Wi-Fi desligado pela chave (Config)");
        esp_wifi_disconnect();
        esp_wifi_stop();
        wifi_net_on_event_ui(false, 0);
    } else {
        ESP_LOGI(TAG, "Wi-Fi ligado pela chave (Config)");
        for (int i = 0; i < s_nets_n; i++) {
            s_nets[i].bad = 0; s_nets[i].noap = 0; s_auth_hits[i] = 0;
        }
        s_attempts = 0; s_backoff_s = 0;
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();   /* STA_START -> recon_task -> scan */
    }
}

bool wifi_net_enabled(void) { return s_enabled; }

void wifi_net_seed_clock(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open("pdawifi", NVS_READONLY, &h) != ESP_OK) return;
    esp_err_t err = nvs_get_u32(h, "last_utc", &v);
    nvs_close(h);
    if (err != ESP_OK || v < 946684800U) return;      /* 2000-01-01 */
    if (time(NULL) >= 946684800) return;              /* já plausível */
    struct timeval tv = { (time_t)v, 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "relógio semeado do NVS (ultimo UTC conhecido; NTP refina)");
}

esp_err_t wifi_net_init(void)
{
    if (!load_wifi_lua()) return ESP_ERR_NOT_FOUND;
    xTaskCreate(recon_task, "wifi_rcn", 6144, NULL, 3, &s_recon_task);  /* M5.3.2: era 4096; HWM de 1960 B livres no scan (11ª rodada) */
    BaseType_t ok = xTaskCreate(wifi_task, "wifi_net", 8192, NULL, 4, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

const char *wifi_net_ssid(void) { return s_ssid; }

/* M5.2: UI de Redes conecta direto se a senha já está salva. */
const char *wifi_net_saved_pass(const char *ssid)
{
    for (int k = 0; k < s_nets_n; k++)
        if (!strcmp(s_nets[k].ssid, ssid)) return s_nets[k].pass;
    return NULL;
}

/* ---------------- pausa de standby (M4.14) ----------------
 * A associação Wi-Fi VIVE durante o standby robusto (CPU viva); o que
 * pausa é o loop de reconexão e o poll do NTP — era o que logava
 * "desconectado — reconectando em 2 s" DENTRO do standby. Modem-sleep
 * do C6 (esp_hosted power save) é o próximo estágio (POWER_REWORK). */
void wifi_net_pause(void)
{
    s_paused = true;
    if (s_sntp_on) {
        esp_sntp_stop();   /* assíncrono (tcpip_callback); entra na fila */
        ESP_LOGI(TAG, "NTP pausado (standby)");
    }
    ESP_LOGI(TAG, "Wi-Fi pausado p/ standby (reconexão automática off)");
}

void wifi_net_resume(void)
{
    s_paused = false;
    /* M5.3: acordar = ciclo novo: redes marcadas voltam a concorrer
     * (APs podem ter voltado; blacklist não sobrevive ao standby). */
    for (int i = 0; i < s_nets_n; i++) {
        s_nets[i].bad = 0; s_nets[i].noap = 0; s_auth_hits[i] = 0;
    }
    s_attempts = 0;
    if (s_sntp_on) {
        /* M4.14.2: sntp_restart() é NO-OP com o serviço parado (lwIP
         * 5.5.1, sntp.c: só restarta se enabled — bug achado no log da
         * validação: NTP morria após o 1º ciclo). esp_sntp_init() é o
         * caminho de religar; servidores e cb sobrevivem ao stop. Não
         * consultar esp_sntp_enabled(): é assíncrono e corre aqui. */
        esp_sntp_init();
        ESP_LOGI(TAG, "NTP retomado");
    }
    s_backoff_s = 0;
    if (!s_connected && s_auto && s_nets_n) {
        if (s_cur < 0) apply_saved(s_last_good < s_nets_n ? s_last_good : 0);
        ESP_LOGI(TAG, "Wi-Fi retomado do standby — reconectando");
        esp_wifi_connect();
    } else {
        ESP_LOGI(TAG, "Wi-Fi retomado do standby");
    }
}

static void scan_task(void *arg)
{
    void **ctxs = (void **)arg;
    wifi_net_scan_cb cb = (wifi_net_scan_cb)(uintptr_t)ctxs[0];
    void *ctx = ctxs[1];
    free(ctxs);

    wifi_scan_config_t sc = { 0 };
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 300;
    sc.scan_time.passive = 300;
    sc.show_hidden = true;
    wifi_net_ap_t *out = NULL;
    int n = 0;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t total = 0;
        esp_wifi_scan_get_ap_num(&total);
        if (total > 16) total = 16;
        wifi_ap_record_t *rec = (wifi_ap_record_t *)calloc(total, sizeof(*rec));
        if (rec && esp_wifi_scan_get_ap_records(&total, rec) == ESP_OK) {
            out = (wifi_net_ap_t *)calloc(total ? total : 1, sizeof(*out));
            if (out) {
                for (uint16_t i = 0; i < total; i++) {
                    copy_wifi_str(out[i].ssid, sizeof(out[i].ssid), (const char *)rec[i].ssid);
                    out[i].rssi = rec[i].rssi;
                    out[i].open = (rec[i].authmode == WIFI_AUTH_OPEN);
                    n = i + 1;
                }
                /* ordena por RSSI desc (insertion sort, n<=16) */
                for (int i = 1; i < n; i++) {
                    wifi_net_ap_t k = out[i];
                    int j = i - 1;
                    while (j >= 0 && out[j].rssi < k.rssi) { out[j + 1] = out[j]; j--; }
                    out[j + 1] = k;
                }
            }
        }
        free(rec);
    }
    if (cb) cb(out, n, ctx);
    free(out);
    /* M5.3: rescan da tela Redes = intervenção manual: reavalia o
     * manager (acorda o modo "nenhuma rede presente"). */
    if (s_recon_task) xTaskNotifyGive(s_recon_task);
    vTaskDelete(NULL);
}

esp_err_t wifi_net_scan(wifi_net_scan_cb cb, void *ctx)
{
    /* M5.3.2: rádio parado => scan era RPC inválido no C6 (resp 12290
     * no log da 11ª rodada); falha limpo em vez de martelar o hosted. */
    if (!s_enabled) {
        ESP_LOGW(TAG, "scan pedido com rádio desligado — ignora");
        return ESP_ERR_INVALID_STATE;
    }
    void **ctxs = (void **)malloc(2 * sizeof(void *));
    if (!ctxs) return ESP_ERR_NO_MEM;
    ctxs[0] = (void *)(uintptr_t)cb;
    ctxs[1] = ctx;
    BaseType_t ok = xTaskCreate(scan_task, "wifi_scan", 6144, ctxs, 3, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

static void escape_lua_str(char *dst, size_t sz, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < sz; p++) {
        if (*p == '"' || *p == '\\') dst[o++] = '\\';
        dst[o++] = *p;
    }
    dst[o] = '\0';
}

esp_err_t wifi_net_save_config(const char *ssid, const char *pass)
{
    (void)ssid; (void)pass;   /* M5.2: a lista s_nets já é a fonte */
    char buf[1200];
    int n = snprintf(buf, sizeof(buf),
        "-- config/wifi.lua (gerado pelo PDA ao conectar/salvar rede)\n"
        "-- M5.2: várias redes salvas; o PDA rotaciona por sinal/falha.\n"
        "return {\n"
        "  auto_connect = true,\n"
        "  networks = {\n");
    for (int k = 0; k < s_nets_n && n < (int)sizeof(buf) - 160; k++) {
        char es[64], ep[128];
        escape_lua_str(es, sizeof(es), s_nets[k].ssid);
        escape_lua_str(ep, sizeof(ep), s_nets[k].pass);
        n += snprintf(buf + n, sizeof(buf) - n,
                      "    { ssid = \"%s\", password = \"%s\" },\n", es, ep);
    }
    snprintf(buf + n, sizeof(buf) - n, "  },\n}\n");
    char path[160];
    if (pda_path(path, sizeof(path), "config/wifi.lua") != ESP_OK) return ESP_FAIL;
    esp_err_t e = storage_write_text_file(path, buf, strlen(buf));
    if (e == ESP_OK) ESP_LOGI(TAG, "salvo: %s (%d bytes, %d rede(s))",
                              path, (int)strlen(buf), s_nets_n);
    return e;
}

esp_err_t wifi_net_connect(const char *ssid, const char *pass, bool save)
{
    if (!s_enabled) wifi_net_set_enabled(true);   /* M5.3: conectar liga */
    /* M5.2: upsert na lista de salvas (ação do usuário limpa flags). */
    int idx = -1;
    for (int k = 0; k < s_nets_n; k++)
        if (!strcmp(s_nets[k].ssid, ssid)) { idx = k; break; }
    if (idx < 0 && s_nets_n < MAX_SAVED) idx = s_nets_n++;
    if (idx >= 0) {
        snprintf(s_nets[idx].ssid, sizeof(s_nets[idx].ssid), "%s", ssid);
        snprintf(s_nets[idx].pass, sizeof(s_nets[idx].pass), "%s", pass);
        s_nets[idx].bad = 0;
        s_nets[idx].noap = 0;
        s_last_good = idx;
    }
    s_auto = true;
    s_backoff_s = 0;
    if (save) wifi_net_save_config(ssid, pass);
    if (idx >= 0) apply_saved(idx);

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    copy_wifi_str((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), ssid);
    copy_wifi_str((char *)cfg.sta.password, sizeof(cfg.sta.password), pass);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* aceita WPA2/WPA3 do AP */
    cfg.sta.pmf_cfg.capable = true;   /* hotspots WPA3/PMF (ex.: alguns Android) */
    cfg.sta.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;
    esp_wifi_disconnect();
    return esp_wifi_connect();
}

bool wifi_net_connected(void) { return s_connected; }

void wifi_net_set_autoreconnect(bool on)
{
    /* Só a nossa flag: o loop de reconexão é nosso (handler de
     * DISCONNECTED checa s_auto). esp_wifi_set_autoreconnect não faz parte
     * do subset proxied pelo esp_wifi_remote no IDF 5.5. */
    s_auto = on;
    ESP_LOGI(TAG, "auto-reconnect: %s", on ? "on" : "off");
}
bool wifi_net_clock_synced(void) { return s_synced; }

int wifi_net_rssi(void)
{
    wifi_ap_record_t rec;
    if (esp_wifi_sta_get_ap_info(&rec) != ESP_OK) return 0;
    return rec.rssi;
}

const char *wifi_net_time_hhmm(void)
{
    if (!s_synced) return NULL;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(s_hhmm, sizeof(s_hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return s_hhmm;
}
