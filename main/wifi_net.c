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
        ok = s_ssid[0] != '\0';
    } else {
        ESP_LOGW(TAG, "config/wifi.lua inválido — Wi-Fi desativado");
    }
    lua_close(L);
    if (ok) ESP_LOGI(TAG, "wifi.lua: ssid=\"%s\" auto=%d", s_ssid, (int)s_auto);
    return ok;
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

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;
    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA up");
        if (s_auto) esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        if (s_connected) {
            s_connected = false;
            wifi_net_on_event_ui(false, 0);
        }
        if (s_auto) {
            ESP_LOGI(TAG, "desconectado — reconectando em 2 s");
            vTaskDelay(pdMS_TO_TICKS(2000));
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
        if (!s_synced) {
            esp_sntp_config_t cfg = ESP_SNTP_DEFAULT_CONFIG(pda_settings()->ntp_server);
            cfg.sync_cb = sntp_synced;
            esp_sntp_init(&cfg);
        }
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
    strncpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
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

bool wifi_net_connected(void) { return s_connected; }
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
