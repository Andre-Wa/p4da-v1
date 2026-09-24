#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wi-Fi STA + NTP via coprocessador ESP32-C6 (ESP-Hosted 2.12.x SDIO +
 * esp_wifi_remote). Configuração em <raiz>/config/wifi.lua:
 *
 *   return { ssid = "minha-rede", password = "secreta", auto_connect = true }
 *
 * Sem o arquivo (ou ssid vazio), o Wi-Fi nem inicializa (PDA segue
 * offline e o relógio permanece em uptime).
 *
 * Limitação conhecida (docs/WIFI.md): o link SDIO com o C6 não sobrevive
 * ao light sleep do standby; ao acordar, o wifi reconecta sozinho em
 * alguns segundos (ou exige reconexão manual se o C6 tiver reiniciado).
 */

/** Sobe a pilha (nvs/netif/event/wifi) e conecta se auto_connect. */
esp_err_t wifi_net_init(void);

bool wifi_net_connected(void);
bool wifi_net_clock_synced(void);

/** "HH:MM" local (TZ de locale.timezone) se sincronizado; senão NULL. */
const char *wifi_net_time_hhmm(void);

/** RSSI em dBm (0 se desconectado). */
int wifi_net_rssi(void);

#ifdef __cplusplus
}
#endif
