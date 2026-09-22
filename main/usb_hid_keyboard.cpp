/**
 * usb_hid_keyboard.cpp
 *
 * Leitura de teclado USB via USB Host nativo do ESP32-P4, usando o
 * driver "hid_host" do ESP-IDF (boot protocol — funciona com qualquer
 * teclado USB padrão, incluindo o que você está construindo, desde que
 * ele se enumere como HID keyboard genérico).
 *
 * Isso é 100% independente da placa/tela — é a mesma API que funcionaria
 * em qualquer board com USB-OTG do ESP-IDF. A única coisa específica da
 * JC4880P433C é: confirme qual das portas USB-C é a OTG de verdade.
 */

#include "usb_hid_keyboard.h"

#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_hid_kbd";
static KeyPressCallback s_callback = nullptr;

/* Mapa mínimo HID keycode -> ASCII (letras minúsculas, números, espaço,
 * enter, backspace, tab). Suficiente pra provar que a entrada funciona;
 * expanda depois pra maiúsculas/símbolos conforme o app precisar. */
/* Símbolos de layout US-QWERTY quando Shift é segurado junto com a
 * linha de números (1-9, 0, nessa ordem). */
static const char s_shifted_numbers[] = "!@#$%^&*()";

static uint8_t hid_keycode_to_ascii(uint8_t keycode, bool shift)
{
    if (keycode >= 0x04 && keycode <= 0x1D) { // a-z
        char c = 'a' + (keycode - 0x04);
        return shift ? (c - 'a' + 'A') : c;
    }
    if (keycode >= 0x1E && keycode <= 0x27) { // 1-9, 0 (0x27 = '0')
        if (shift) {
            return s_shifted_numbers[keycode - 0x1E];
        }
        return (keycode == 0x27) ? '0' : ('1' + (keycode - 0x1E));
    }
    switch (keycode) {
        case 0x28: return '\n';   // Enter
        case 0x2A: return '\b';   // Backspace
        case 0x2B: return '\t';   // Tab
        case 0x2C: return ' ';    // Space
        case 0x2D: return shift ? '_' : '-';  // - _
        case 0x2E: return shift ? '+' : '=';  // = +
        case 0x2F: return shift ? '{' : '[';  // [ {
        case 0x30: return shift ? '}' : ']';  // ] }
        case 0x31: return shift ? '|' : '\\'; // \ |
        case 0x33: return shift ? ':' : ';';  // ; :
        case 0x34: return shift ? '"' : '\''; // ' "
        case 0x36: return shift ? '<' : ',';  // , <
        case 0x37: return shift ? '>' : '.';  // . >
        case 0x38: return shift ? '?' : '/';  // / ?
        default: return 0;        // sem mapeamento simples (setas, F1-F12, etc.)
    }
}

static void hid_keyboard_report_callback(const uint8_t *const data, const int length)
{
    // DEBUG: mostra exatamente o que chegou, byte a byte — remova depois
    // de confirmar que está tudo funcionando.
    ESP_LOGI(TAG, "relatório recebido: length=%d", length);
    ESP_LOG_BUFFER_HEX(TAG, data, length > 0 ? length : 0);

    const uint8_t *report = data;
    int report_len = length;

    // Alguns dispositivos (CircuitPython/KMK quando expõem mais de um
    // tipo de relatório na mesma interface — ex: teclado + teclas de
    // mídia) prefixam cada relatório com 1 byte de Report ID antes do
    // payload padrão de 8 bytes do boot protocol. Se o tamanho bater com
    // 9 (1 + 8), pula esse primeiro byte e usa o resto normalmente.
    if (length == 9) {
        report = data + 1;
        report_len = 8;
    }

    // Boot protocol: byte0 = modificadores, byte1 = reservado, bytes2..7 = até 6 keycodes
    if (report_len < 8) {
        ESP_LOGW(TAG, "relatório descartado: tamanho %d não reconhecido (esperado 8 ou 9)", length);
        return;
    }

    uint8_t modifiers = report[0];
    bool shift = modifiers & 0x22; // left shift (0x02) | right shift (0x20)

    for (int i = 2; i < 8; i++) {
        uint8_t keycode = report[i];
        if (keycode == 0) {
            continue;
        }
        uint8_t ascii = hid_keycode_to_ascii(keycode, shift);
        ESP_LOGI(TAG, "tecla: keycode=0x%02X ascii=%c(0x%02X) modifiers=0x%02X", keycode, ascii ? ascii : '?', ascii, modifiers);
        if (s_callback) {
            s_callback(ascii, keycode, modifiers);
        }
    }
}

static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                         const hid_host_interface_event_t event,
                                         void *arg)
{
    uint8_t data[64] = {0};
    size_t data_length = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        hid_host_device_get_raw_input_report_data(hid_device_handle, data, sizeof(data), &data_length);
        // O filtro de "isso parece teclado?" já aconteceu em
        // hid_host_device_event (no momento da conexão) — filtrar de
        // novo aqui por subclass/proto só rejeitava relatórios válidos
        // de dispositivos sem boot protocol declarado (como o KMK).
        hid_keyboard_report_callback(data, (int)data_length);
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "teclado desconectado");
        hid_host_device_close(hid_device_handle);
        break;
    default:
        break;
    }
}

static void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                                   const hid_host_driver_event_t event,
                                   void *arg)
{
    hid_host_dev_params_t dev_params;
    hid_host_device_get_params(hid_device_handle, &dev_params);

    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        /* Por especificação USB HID, o campo `proto` só tem significado
         * quando o dispositivo declara boot protocol (subclass=1) — é um
         * modo legado pra digitar antes do SO carregar um driver de
         * verdade (BIOS/UEFI). Um teclado desktop comum normalmente
         * declara isso (subclass=1, proto=1). Mas o usb_hid do
         * CircuitPython/KMK NÃO declara boot protocol por padrão — expõe
         * um HID genérico válido (subclass=0, proto=0), que é
         * perfeitamente funcional em qualquer SO moderno, só não é
         * "boot compatible". Por isso aceitamos as duas situações, e só
         * rejeitamos explicitamente o que sabemos que não é teclado
         * (ex: um mouse em boot protocol, proto=2). Isso também aceita
         * interfaces HID genéricas não-teclado (a tal "interface
         * fantasma" que já derrubou o firmware antes) — por isso as
         * chamadas abaixo agora checam erro em vez de ignorar, pra uma
         * interface que falhe ao abrir/iniciar não colocar o sistema
         * num estado inconsistente. */
        bool parece_teclado =
            (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE && dev_params.proto == HID_PROTOCOL_KEYBOARD) ||
            (dev_params.sub_class == 0 && dev_params.proto == 0);

        if (!parece_teclado) {
            ESP_LOGW(TAG, "ignorando interface HID que não parece teclado (subclass=%d proto=%d)",
                     dev_params.sub_class, dev_params.proto);
            return;
        }

        ESP_LOGI(TAG, "possível teclado conectado (subclass=%d proto=%d)",
                 dev_params.sub_class, dev_params.proto);

        const hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL,
        };
        esp_err_t err = hid_host_device_open(hid_device_handle, &dev_config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "falha ao abrir interface HID (%s) — ignorando", esp_err_to_name(err));
            return;
        }

        if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
            hid_class_request_set_idle(hid_device_handle, 0, 0);
        }

        err = hid_host_device_start(hid_device_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "falha ao iniciar interface HID (%s) — desistindo dela", esp_err_to_name(err));
            hid_host_device_close(hid_device_handle);
            return;
        }
    }
}

static void usb_host_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            break;
        }
    }
    vTaskDelete(NULL);
}

void usb_hid_keyboard_init(KeyPressCallback on_key_press)
{
    s_callback = on_key_press;

    TaskHandle_t task_handle;
    xTaskCreate(usb_host_lib_task, "usb_host", 4096, xTaskGetCurrentTaskHandle(), 2, &task_handle);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_event,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    ESP_LOGI(TAG, "USB Host HID pronto — aguardando teclado ser conectado");
}
