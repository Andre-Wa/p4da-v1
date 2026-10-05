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

/** M5.0b (A3): semeia o relógio com o último UTC persistido no NVS
 *  (chamar após nvs_flash_init, antes de writes que viram mtime). */
void wifi_net_seed_clock(void);

bool wifi_net_connected(void);
bool wifi_net_clock_synced(void);

/** "HH:MM" local (TZ de locale.timezone) se sincronizado; senão NULL. */
const char *wifi_net_time_hhmm(void);

/** RSSI em dBm (0 se desconectado). */
int wifi_net_rssi(void);

/** SSID atual (configurado), nunca NULL. */
const char *wifi_net_ssid(void);

/** M5.2: senha salva p/ o ssid (NULL se não salvo) — UI conecta direto. */
const char *wifi_net_saved_pass(const char *ssid);

typedef struct {
    char ssid[33];
    int  rssi;
    bool open;
} wifi_net_ap_t;

/** Pausa/retoma o loop de reconexão automática (usado durante scan p/
 *  o rádio não ficar preso tentando a rede salva longe de casa). */
void wifi_net_set_autoreconnect(bool on);

/** Pausa/retoma p/ STANDBY (M4.14): congela o loop de reconexão e o poll
 *  do NTP (esp_sntp_stop/sntp_restart); a associação viva é mantida.
 *  Chamado pelo standby_cb do power_mgmt (entrando/saindo). */
void wifi_net_pause(void);
void wifi_net_resume(void);

/** Scan bloqueante em task própria; cb chamado dessa task (hop p/ UI
 *  é responsabilidade do caller). count==0 se nada/erro. */
typedef void (*wifi_net_scan_cb)(const wifi_net_ap_t *aps, int count, void *ctx);
esp_err_t wifi_net_scan(wifi_net_scan_cb cb, void *ctx);

/** Conecta (e opcionalmente persiste em config/wifi.lua). */
esp_err_t wifi_net_connect(const char *ssid, const char *pass, bool save);

#ifdef __cplusplus
}
#endif
