/**
 * usb_hid_keyboard.cpp — teclado USB HID via USB Host nativo do ESP32-P4.
 *
 * Refatorado do protótipo (ver docs/USB.md). Três correções estruturais:
 *
 *  1) HOT-PLUG: a task da biblioteca host não se mata mais em ALL_FREE.
 *     No protótipo, a janela entre usb_host_install() e hid_host_install()
 *     (zero clients, zero devices) fazia a task dar break+vTaskDelete no
 *     boot sem teclado — e nenhum hot-plug era visto depois.
 *
 *  2) LIGHT SLEEP: o periférico DWC2 não sobrevive ao sleep (como o
 *     SDMMC). prepare_sleep() faz teardown ordenado e resume() reinstala.
 *
 *  3) DEDUP: com set_idle(0) o teclado manda relatório só em mudança, mas
 *     dispositivos que ignoram idle mandariam o report inteiro repetido —
 *     comparamos com o relatório anterior e só emitimos NOVAS imprensa.
 */

#include "usb_hid_keyboard.h"

#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <cstring>

static const char *TAG = "usb_hid_kbd";

static KeyPressCallback s_callback = nullptr;
static UsbKbdEventCallback s_event_cb = nullptr;

static TaskHandle_t s_lib_task = NULL;
static volatile bool s_installed = false;
static volatile bool s_shutdown = false;      /* pede saída da lib task */
static SemaphoreHandle_t s_lib_done_sem = NULL; /* lib task avisou que saiu */

#define USB_KBD_MAX_IFACES 4
static hid_host_device_handle_t s_devs[USB_KBD_MAX_IFACES] = { nullptr };
static volatile int s_conn_count = 0;

static uint8_t s_prev_keys[6] = { 0 };

/* ------------------------------------------------------------------ */
/* Mapa HID keycode -> ASCII (US-QWERTY, com shift na linha de números) */
static const char s_shifted_numbers[] = "!@#$%^&*()";

static uint8_t hid_keycode_to_ascii(uint8_t keycode, bool shift)
{
    if (keycode >= 0x04 && keycode <= 0x1D) { // a-z
        char c = 'a' + (keycode - 0x04);
        return shift ? (char)(c - 'a' + 'A') : (uint8_t)c;
    }
    if (keycode >= 0x1E && keycode <= 0x27) { // 1-9, 0
        if (shift) return (uint8_t)s_shifted_numbers[keycode - 0x1E];
        return (keycode == 0x27) ? '0' : (uint8_t)('1' + (keycode - 0x1E));
    }
    switch (keycode) {
        case 0x28: return '\n';   // Enter
        case 0x2A: return '\b';   // Backspace
        case 0x2B: return '\t';   // Tab
        case 0x2C: return ' ';    // Space
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        default:   return 0;      // setas, F1-F12 etc: use o keycode no app
    }
}

/* ------------------------------------------------------------------ */
static void track_add(hid_host_device_handle_t h)
{
    for (int i = 0; i < USB_KBD_MAX_IFACES; i++) {
        if (s_devs[i] == nullptr) { s_devs[i] = h; break; }
    }
    s_conn_count++;
    if (s_event_cb) s_event_cb(true);
}

static void track_remove(hid_host_device_handle_t h)
{
    bool found = false;
    for (int i = 0; i < USB_KBD_MAX_IFACES; i++) {
        if (s_devs[i] == h) { s_devs[i] = nullptr; found = true; }
    }
    if (!found) return;
    if (s_conn_count > 0) s_conn_count--;
    if (s_event_cb) s_event_cb(s_conn_count > 0);
}

bool usb_hid_keyboard_connected(void) { return s_conn_count > 0; }

/* ------------------------------------------------------------------ */
static void hid_keyboard_report_callback(const uint8_t *const data, int length)
{
    const uint8_t *report = data;
    int report_len = length;

    /* CircuitPython/KMK podem prefixar 1 byte de Report ID antes dos 8
     * bytes do boot protocol (tamanho 9 = 1 + 8). */
    if (length == 9) {
        report = data + 1;
        report_len = 8;
    }
    if (report_len < 8) {
        ESP_LOGD(TAG, "relatório descartado: tamanho %d", length);
        return;
    }

    const uint8_t modifiers = report[0];
    const bool shift = (modifiers & 0x22) != 0;   // left|right shift

    /* dedup: só emite tecla que não estava no relatório anterior */
    uint8_t cur[6];
    memcpy(cur, &report[2], 6);
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = cur[i];
        if (keycode == 0) continue;
        bool was_down = false;
        for (int j = 0; j < 6; j++) {
            if (s_prev_keys[j] == keycode) { was_down = true; break; }
        }
        if (was_down) continue;

        uint8_t ascii = hid_keycode_to_ascii(keycode, shift);
        ESP_LOGD(TAG, "tecla kc=0x%02X ascii=0x%02X mod=0x%02X", keycode, ascii, modifiers);
        if (s_callback) s_callback(ascii, keycode, modifiers);
    }
    memcpy(s_prev_keys, cur, 6);
}

static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                        const hid_host_interface_event_t event,
                                        void *arg)
{
    (void)arg;
    uint8_t data[64] = { 0 };
    size_t data_length = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(hid_device_handle, data,
                                                        sizeof(data), &data_length) == ESP_OK) {
            hid_keyboard_report_callback(data, (int)data_length);
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "interface HID desconectada");
        hid_host_device_close(hid_device_handle);
        track_remove(hid_device_handle);
        break;
    default:
        break;
    }
}

static void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                                  const hid_host_driver_event_t event,
                                  void *arg)
{
    (void)arg;
    hid_host_dev_params_t dev_params;
    if (hid_host_device_get_params(hid_device_handle, &dev_params) != ESP_OK) return;

    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;

    /* Aceita boot-protocol keyboard E HID genérico (subclass=0/proto=0,
     * o caso do KMK/CircuitPython); rejeita mouse boot (proto=2) etc. */
    const bool parece_teclado =
        (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE && dev_params.proto == HID_PROTOCOL_KEYBOARD) ||
        (dev_params.sub_class == 0 && dev_params.proto == 0);
    if (!parece_teclado) {
        ESP_LOGW(TAG, "ignorando interface HID não-teclado (subclass=%d proto=%d)",
                 dev_params.sub_class, dev_params.proto);
        return;
    }

    ESP_LOGI(TAG, "teclado conectado (subclass=%d proto=%d)",
             dev_params.sub_class, dev_params.proto);

    const hid_host_device_config_t dev_config = {
        .callback = hid_host_interface_callback,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_device_open(hid_device_handle, &dev_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "falha ao abrir interface HID (%s)", esp_err_to_name(err));
        return;
    }
    if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
        hid_class_request_set_idle(hid_device_handle, 0, 0); /* só reporta em mudança */
    }
    err = hid_host_device_start(hid_device_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "falha ao iniciar interface HID (%s)", esp_err_to_name(err));
        hid_host_device_close(hid_device_handle);
        return;
    }
    memset(s_prev_keys, 0, sizeof(s_prev_keys));
    track_add(hid_device_handle);
}

/* ------------------------------------------------------------------ */
static void usb_host_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    if (usb_host_install(&host_config) != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install falhou");
        xTaskNotifyGive((TaskHandle_t)arg);
        vTaskDelete(NULL);
        return;
    }
    xTaskNotifyGive((TaskHandle_t)arg);

    /* NÃO sair em ALL_FREE por conta própria: barramento vazio é o
     * estado NORMAL de um PDA sem teclado (sair aqui matava o hot-plug).
     * A saída só ocorre em dois casos:
     *   - s_shutdown && ALL_FREE  (teardown pedido por prepare_sleep);
     *   - handle_events retornar erro (abort raro).
     * Ordem importa: o usb_host_uninstall() só pode ser chamado por OUTRO
     * task depois que esta task acabou (padrão dos exemplos do IDF). */
    while (true) {
        uint32_t event_flags = 0;
        if (usb_host_lib_handle_events(portMAX_DELAY, &event_flags) != ESP_OK) break;
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (s_shutdown && (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) break;
    }
    ESP_LOGI(TAG, "task usb_host encerrada");
    if (s_lib_done_sem) xSemaphoreGive(s_lib_done_sem);
    vTaskDelete(NULL);
}

static void start_stack(void)
{
    if (s_installed) return;
    if (!s_lib_done_sem) s_lib_done_sem = xSemaphoreCreateBinary();
    s_shutdown = false;

    xTaskCreate(usb_host_lib_task, "usb_host", 4096,
                xTaskGetCurrentTaskHandle(), 2, &s_lib_task);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_event,
        .callback_arg = NULL,
    };
    if (hid_host_install(&hid_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install falhou");
        return;
    }
    s_installed = true;
    ESP_LOGI(TAG, "USB Host HID pronto — hot-plug ativo");
}

void usb_hid_keyboard_init(KeyPressCallback on_key_press, UsbKbdEventCallback on_event)
{
    s_callback = on_key_press;
    if (on_event) s_event_cb = on_event;
    start_stack();
}

void usb_hid_keyboard_prepare_sleep(void)
{
    if (!s_installed) return;
    ESP_LOGI(TAG, "teardown USB p/ standby...");
    for (int i = 0; i < USB_KBD_MAX_IFACES; i++) {
        if (s_devs[i]) {
            hid_host_device_close(s_devs[i]);
            s_devs[i] = nullptr;
        }
    }
    s_conn_count = 0;

    /* 1) pede à lib task para sair quando o barramento esvaziar; */
    s_shutdown = true;
    /* 2) sem client, o host libera tudo e a lib task vê ALL_FREE e sai; */
    if (hid_host_uninstall() != ESP_OK) {
        ESP_LOGW(TAG, "hid_host_uninstall reportou erro (seguindo)");
    }
    s_installed = false;
    /* 3) espera a lib task confirmar a saída ANTES de desinstalar o host; */
    if (s_lib_done_sem &&
        xSemaphoreTake(s_lib_done_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "lib task não confirmou saída em 1 s (uninstall mesmo assim)");
    }
    esp_err_t err = usb_host_uninstall();
    ESP_LOGI(TAG, "usb_host_uninstall: %s", esp_err_to_name(err));
    s_lib_task = NULL;
}

void usb_hid_keyboard_resume(void)
{
    if (s_installed) {
        ESP_LOGW(TAG, "resume chamados com USB já instalado (ignorado)");
        return;
    }
    ESP_LOGI(TAG, "reinstall USB apos wake...");
    start_stack();
    ESP_LOGI(TAG, "USB apos wake: instalado=%d", (int)s_installed);
}
