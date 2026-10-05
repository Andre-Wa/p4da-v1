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
        ok = true;   /* arquivo válido: sobe a pilha mesmo sem ssid (scan) */
    } else {
        ESP_LOGW(TAG, "config/wifi.lua inválido — Wi-Fi desativado");
    }
    lua_close(L);
    if (ok) ESP_LOGI(TAG, "wifi.lua: ssid=\"%s\" auto=%d", s_ssid, (int)s_auto);
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

static void sntp_synced(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
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
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;
    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA up (ssid=%s)", s_ssid[0] ? s_ssid : "-");
        if (s_auto && s_ssid[0]) esp_wifi_connect();
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
        if (s_auto) {
            ESP_LOGI(TAG, "desconectado — reconectando em 2 s");
            vTaskDelay(pdMS_TO_TICKS(2000));
            /* M4.15: o standby pode ter começado DURANTE os 2 s (visto no
             * log da senha errada: connect espúrio em pleno standby);
             * re-checa antes de gastar rádio. */
            if (s_paused) {
                ESP_LOGI(TAG, "standby no meio do retry — reconexão pausada (M4.15)");
                break;
            }
            esp_wifi_connect();
        }
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
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        wifi_net_on_event_ui(true, wifi_net_rssi());
        if (!s_synced) sntp_start();
    }
}

/* ---------------- task ---------------- */
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
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start falhou");
        wifi_net_on_event_ui(false, 0);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi inicializado (host P4 <-> C6 via SDIO)");
    vTaskDelete(NULL);
}

esp_err_t wifi_net_init(void)
{
    if (!load_wifi_lua()) return ESP_ERR_NOT_FOUND;
    BaseType_t ok = xTaskCreate(wifi_task, "wifi_net", 8192, NULL, 4, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

const char *wifi_net_ssid(void) { return s_ssid; }

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
    if (s_sntp_on) {
        /* M4.14.2: sntp_restart() é NO-OP com o serviço parado (lwIP
         * 5.5.1, sntp.c: só restarta se enabled — bug achado no log da
         * validação: NTP morria após o 1º ciclo). esp_sntp_init() é o
         * caminho de religar; servidores e cb sobrevivem ao stop. Não
         * consultar esp_sntp_enabled(): é assíncrono e corre aqui. */
        esp_sntp_init();
        ESP_LOGI(TAG, "NTP retomado");
    }
    if (!s_connected && s_auto && s_ssid[0]) {
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
    vTaskDelete(NULL);
}

esp_err_t wifi_net_scan(wifi_net_scan_cb cb, void *ctx)
{
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
    char es[64], ep[128];
    escape_lua_str(es, sizeof(es), ssid);
    escape_lua_str(ep, sizeof(ep), pass);
    char buf[320];
    int n = snprintf(buf, sizeof(buf),
        "-- config/wifi.lua (gerado pelo PDA ao conectar/salvar rede)\n"
        "return {\n"
        "  ssid = \"%s\",\n"
        "  password = \"%s\",\n"
        "  auto_connect = true,\n"
        "}\n", es, ep);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    char path[160];
    if (pda_path(path, sizeof(path), "config/wifi.lua") != ESP_OK) return ESP_ERR_INVALID_SIZE;
    return storage_write_text_file(path, buf, (size_t)n);
}

esp_err_t wifi_net_connect(const char *ssid, const char *pass, bool save)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", pass);
    s_auto = true;
    if (save) wifi_net_save_config(ssid, pass);

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
