/**
 * audio_uac.c — M5a.1: saída de áudio USB (UAC host) + player WAV PCM16.
 *
 * Segundo client da USB Host Library (o 1º é o HID do teclado). O driver
 * UAC roda a própria background task p/ eventos; a nossa fila de comandos
 * (play/stop) vive na task "au_play".
 *
 * Round 1: sem ring buffer próprio (o write bloqueia no ring do driver),
 * sem pause (suspend/resume ficam p/ M5a.2), sem hub (teclado+DAC juntos
 * é o round 2, CONFIG_USB_HOST_HUBS_SUPPORTED).
 */

#include "audio_uac.h"
#include "usb/uac_host.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const char *TAG = "audio_uac";

typedef struct { int op; char path[144]; } cmd_t;   /* op 1=play 2=stop */

static QueueHandle_t s_q = NULL;
static volatile bool s_present = false;
static volatile bool s_playing = false;
static volatile bool s_stop_req = false;
static char s_devname[48] = "";
static char s_track[64] = "";
static char s_state[48] = "iniciando";
static uint8_t s_addr = 0, s_iface = 0;
static audio_uac_event_cb s_cb = NULL;
static void *s_ctx = NULL;

static void notify(void) { if (s_cb) s_cb(s_ctx); }
static void set_state(const char *s)
{
    snprintf(s_state, sizeof(s_state), "%s", s);
    notify();
}

/* ---------------- callbacks do driver (contexto da task dele) ---------------- */

static void dev_event_cb(uac_host_device_handle_t h,
                         uac_host_device_event_t ev, void *arg)
{
    (void)h; (void)arg;
    if (ev == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "dispositivo UAC desconectado");
        s_present = false;
        s_stop_req = true;
        s_playing = false;
        set_state("sem dispositivo");
    } else if (ev == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(TAG, "transfer error UAC — parando faixa");
        s_stop_req = true;
    }
}

static void utf8_from_wchar(char *dst, size_t sz, const wchar_t *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < sz; i++) {
        unsigned c = (unsigned)src[i];
        if (c < 0x80) dst[o++] = (char)c;
        else if (c < 0x800) {
            if (o + 2 >= sz) break;
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= sz) break;
            dst[o++] = (char)(0xE0 | (c >> 12));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = 0;
}

/* M5.4.4: o callback do driver roda NA task de eventos do UAC — chamar
 * APIs bloqueantes do driver ali (open/close p/ ler o nome) corrompia a
 * fila de client events (asserts em usb_host_client_handle_events nos
 * logs de 2026-10-07 com o QCY H3S). Callback só enfileira; quem abre
 * é a task au_evt. */
typedef struct { uint8_t op; uint8_t addr; uint8_t iface; } evt_t;
static QueueHandle_t s_evtq = NULL;

static void fetch_name_taskish(uint8_t addr, uint8_t iface)
{
    uac_host_device_config_t dcfg = {
        .addr = addr, .iface_num = iface,
        .buffer_size = 4096, .buffer_threshold = 2048,
        .callback = dev_event_cb, .callback_arg = NULL,
    };
    uac_host_device_handle_t dev = NULL;
    if (uac_host_device_open(&dcfg, &dev) != ESP_OK) return;
    uac_host_dev_info_t info;
    if (uac_host_get_device_info(dev, &info) == ESP_OK) {
        utf8_from_wchar(s_devname, sizeof(s_devname), info.iProduct);
        ESP_LOGI(TAG, "UAC device: \"%s\"", s_devname);
    }
    uac_host_device_close(dev);
}

static void evt_task(void *arg)
{
    (void)arg;
    evt_t e;
    for (;;) {
        if (xQueueReceive(s_evtq, &e, portMAX_DELAY) != pdTRUE) continue;
        if (e.op == 1) fetch_name_taskish(e.addr, e.iface);
        set_state(s_present ? "pronto" : "sem dispositivo");
    }
}

static void drv_event_cb(uint8_t addr, uint8_t iface_num,
                         uac_host_driver_event_t ev, void *arg)
{
    (void)arg;
    if (ev != UAC_HOST_DRIVER_EVENT_TX_CONNECTED) return;   /* speaker/DAC */
    s_addr = addr;
    s_iface = iface_num;
    s_present = true;
    ESP_LOGI(TAG, "speaker UAC encontrado (addr %u iface %u)", addr, iface_num);
    evt_t e = { .op = 1, .addr = addr, .iface = iface_num };
    xQueueSend(s_evtq, &e, 0);   /* NÃO abrir nada neste contexto */
}

/* ---------------- WAV PCM16 ---------------- */

static bool parse_wav(FILE *f, uint16_t *ch, uint32_t *rate, uint16_t *bits,
                      long *data_off, uint32_t *data_len)
{
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return false;
    bool fmt_ok = false;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) return false;
        uint32_t sz = (uint32_t)c[4] | ((uint32_t)c[5] << 8) |
                      ((uint32_t)c[6] << 16) | ((uint32_t)c[7] << 24);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[16];
            if (sz < 16 || fread(fm, 1, 16, f) != 16) return false;
            uint16_t fmt = (uint16_t)(fm[0] | (fm[1] << 8));
            *ch   = (uint16_t)(fm[2] | (fm[3] << 8));
            *rate = (uint32_t)fm[4] | ((uint32_t)fm[5] << 8) |
                    ((uint32_t)fm[6] << 16) | ((uint32_t)fm[7] << 24);
            *bits = (uint16_t)(fm[14] | (fm[15] << 8));
            if (fmt != 1) return false;              /* só PCM */
            fmt_ok = true;
            long adv = (long)sz - 16;
            if (adv > 0 && fseek(f, adv, SEEK_CUR)) return false;
        } else if (!memcmp(c, "data", 4)) {
            *data_off = ftell(f);
            *data_len = sz;
            return fmt_ok;
        } else {
            if (fseek(f, (long)((sz + 1u) & ~1u), SEEK_CUR)) return false;
        }
    }
}

/* ---------------- player ---------------- */

static void do_play(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { set_state("erro: arquivo não abre"); return; }
    uint16_t ch = 0, bits = 0;
    uint32_t rate = 0, dlen = 0;
    long doff = 0;
    if (!parse_wav(f, &ch, &rate, &bits, &doff, &dlen)) {
        fclose(f); set_state("erro: só WAV PCM16"); return;
    }
    if (bits != 16 || (ch != 1 && ch != 2)) {
        fclose(f); set_state("erro: WAV 16 bits 1-2 canais"); return;
    }
    if (!s_present) { fclose(f); set_state("sem dispositivo"); return; }

    uac_host_device_config_t dcfg = {
        .addr = s_addr, .iface_num = s_iface,
        .buffer_size = 16384, .buffer_threshold = 4096,
        .callback = dev_event_cb, .callback_arg = NULL,
    };
    uac_host_device_handle_t dev = NULL;
    if (uac_host_device_open(&dcfg, &dev) != ESP_OK) {
        fclose(f); set_state("erro: open device"); return;
    }
    uac_host_dev_info_t info;
    if (uac_host_get_device_info(dev, &info) != ESP_OK) {
        uac_host_device_close(dev); fclose(f); set_state("erro: info"); return;
    }
    bool found = false;
    for (uint8_t a = 0; a < info.iface_alt_num && !found; a++) {
        uac_host_dev_alt_param_t p;
        if (uac_host_get_device_alt_param(dev, a, &p) != ESP_OK) continue;
        if (p.format != 1 || p.channels != ch || p.bit_resolution != 16) continue;
        if (p.sample_freq_type == 0) {
            found = (rate >= p.sample_freq_lower && rate <= p.sample_freq_upper);
        } else {
            for (int q = 0; q < p.sample_freq_type && q < UAC_FREQ_NUM_MAX; q++)
                if (p.sample_freq[q] == rate) found = true;
        }
    }
    if (!found) {
        uac_host_device_close(dev); fclose(f);
        set_state("erro: device não suporta o fmt do WAV"); return;
    }
    uac_host_stream_config_t sc = {
        .channels = (uint8_t)ch, .bit_resolution = 16,
        .sample_freq = rate, .flags = 0,
    };
    if (uac_host_device_start(dev, &sc) != ESP_OK) {
        uac_host_device_close(dev); fclose(f); set_state("erro: start stream"); return;
    }
    s_playing = true;
    s_stop_req = false;
    set_state("tocando");
    ESP_LOGI(TAG, "tocando \"%s\" (%lu Hz, %u ch)", s_track,
             (unsigned long)rate, (unsigned)ch);
    fseek(f, doff, SEEK_SET);
    uint8_t buf[4096];
    uint32_t left = dlen;
    while (!s_stop_req && s_present && left > 0) {
        uint32_t n = left < sizeof(buf) ? left : (uint32_t)sizeof(buf);
        if (fread(buf, 1, n, f) != n) break;
        esp_err_t e = uac_host_device_write(dev, buf, n, pdMS_TO_TICKS(2000));
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "write: %s", esp_err_to_name(e));
            break;
        }
        left -= n;
    }
    vTaskDelay(pdMS_TO_TICKS(120));   /* drain do ring do driver */
    uac_host_device_stop(dev);
    uac_host_device_close(dev);
    s_playing = false;
    fclose(f);
    const bool ended = !s_stop_req && left == 0;
    s_stop_req = false;
    set_state(ended ? "fim da faixa" : "parado");
}

static void player_task(void *arg)
{
    (void)arg;
    cmd_t c;
    for (;;) {
        if (xQueueReceive(s_q, &c, portMAX_DELAY) != pdTRUE) continue;
        if (c.op == 1) do_play(c.path);
        else if (s_playing) { s_stop_req = true; }
    }
}

/* ---------------- API ---------------- */

static void init_task(void *arg)
{
    (void)arg;
    uac_host_driver_config_t cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 6144,
        .core_id = tskNO_AFFINITY,
        .callback = drv_event_cb,
        .callback_arg = NULL,
    };
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 50 && e != ESP_OK; i++) {   /* espera a Host Library */
        e = uac_host_install(&cfg);
        if (e != ESP_OK) vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_install falhou: %s", esp_err_to_name(e));
        set_state("indisponível");
    } else {
        ESP_LOGI(TAG, "UAC host instalado (M5a)");
        set_state(s_present ? "pronto" : "sem dispositivo");
    }
    vTaskDelete(NULL);
}

esp_err_t audio_uac_init(void)
{
    if (s_q) return ESP_OK;
    s_q = xQueueCreate(2, sizeof(cmd_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    s_evtq = xQueueCreate(4, sizeof(evt_t));
    if (!s_evtq) return ESP_ERR_NO_MEM;
    /* M5.4.5: 6148→10240 em PSRAM: a cadeia fopen/fread FATFS +
     * uac open/start + ESP_LOGI(vprintf) estourou 6144 internos no
     * primeiro play (Stack protection fault em _svfprintf_r, log de
     * 2026-10-07). Mesma política das threads de I/O do main.cpp. */
    if (xTaskCreateWithCaps(player_task, "au_play", 10240, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS)
        return ESP_FAIL;
    if (xTaskCreateWithCaps(evt_task, "au_evt", 6144, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS)
        return ESP_FAIL;
    if (xTaskCreate(init_task, "au_init", 3072, NULL, 3, NULL) != pdPASS)
        return ESP_FAIL;
    return ESP_OK;
}

bool audio_uac_present(void) { return s_present; }
const char *audio_uac_dev_name(void) { return s_devname; }
bool audio_uac_playing(void) { return s_playing; }
const char *audio_uac_track(void) { return s_track; }
const char *audio_uac_state(void) { return s_state; }
void audio_uac_set_event_cb(audio_uac_event_cb cb, void *ctx)
{
    s_cb = cb; s_ctx = ctx;
}

esp_err_t audio_uac_play(const char *path)
{
    if (!s_q) return ESP_ERR_INVALID_STATE;
    const char *base = strrchr(path, '/');
    snprintf(s_track, sizeof(s_track), "%s", base ? base + 1 : path);
    notify();
    cmd_t c = { .op = 1 };
    snprintf(c.path, sizeof(c.path), "%s", path);
    xQueueReset(s_q);
    return xQueueSend(s_q, &c, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

void audio_uac_stop(void)
{
    if (!s_q) return;
    cmd_t c = { .op = 2 };
    xQueueSend(s_q, &c, 0);
}
