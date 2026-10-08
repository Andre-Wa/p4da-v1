#pragma once

#include <cstdint>
#include <functional>

/**
 * Teclado USB HID (boot protocol) via USB Host nativo do ESP32-P4.
 *
 * Refatorado para robustez (ver docs/USB.md):
 *  - hot-plug real: a task da biblioteca host NÃO se mata mais quando o
 *    barramento fica vazio (bug do protótipo: só via teclado plugado no boot);
 *  - sobrevive ao light sleep: teardown antes de dormir, reinstall no wake
 *    (o periférico DWC2 não sobrevive ao sleep, igual ao SDMMC);
 *  - dedup de relatórios: tecla segurada não vira flood dekeypress;
 *  - estado de conexão observável (UI + decisão do teclado virtual no M2).
 *
 * Porta certa nesta placa: a USB-C "High Speed" (OTG nativo do P4).
 * A "Full Speed" é USB-Serial-JTAG (flash/monitor) e NÃO serve p/ host.
 */

/** Chamado a cada tecla PRESSIONADA (nova imprensa), com ASCII quando
 *  houver mapeamento simples (0 p/ setas/F-keys — use o keycode). */
using KeyPressCallback = std::function<void(uint8_t ascii, uint8_t hid_keycode, uint8_t modifiers)>;

/* Ações de media key (HUT Consumer 0x0C) p/ o callback abaixo. */
enum {
    USB_MEDIA_NEXT = 1,
    USB_MEDIA_PREV,
    USB_MEDIA_STOP,
    USB_MEDIA_PLAYPAUSE,
    USB_MEDIA_VOL_UP,
    USB_MEDIA_VOL_DOWN,
    USB_MEDIA_MUTE,
};

/** M5a.2: teclas de mídia de consumer controls (H3S, teclados completos
 *  com interface media). Chamado de uma task do HID — faça hop p/ UI. */
using MediaKeyCallback = std::function<void(int action)>;
void usb_hid_keyboard_set_media_cb(MediaKeyCallback cb);

/** connected=true: interface de teclado aberta; false: desconectou. */
using UsbKbdEventCallback = std::function<void(bool connected)>;

/** Instala host USB + driver HID e começa a escutar. Reentrante: pode
 *  ser chamado de novo após usb_hid_keyboard_resume(). */
void usb_hid_keyboard_init(KeyPressCallback on_key_press,
                           UsbKbdEventCallback on_event = nullptr);

/** Há pelo menos uma interface de teclado aberta agora? */
bool usb_hid_keyboard_connected(void);

/** Teardown best-effort antes do light sleep (chamar no hook de standby). */
void usb_hid_keyboard_prepare_sleep(void);

/** Reinstala tudo após o wake (chamar no hook de standby). */
void usb_hid_keyboard_resume(void);
