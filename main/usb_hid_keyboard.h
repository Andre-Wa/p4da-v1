#pragma once

#include <cstdint>
#include <functional>

/**
 * Leitura de teclado USB HID (boot protocol) via USB Host nativo do
 * ESP32-P4. Independe do painel/touch — usa a porta USB-OTG da placa.
 *
 * Se sua placa tiver duas portas USB-C, teste as duas: só uma delas é a
 * OTG de verdade (a outra costuma ser só USB-Serial-JTAG pra flash/log).
 */

/** Callback chamado a cada tecla PRESSIONADA, já convertida pra ASCII
 *  quando possível (0 se a tecla não tiver equivalente ASCII simples,
 *  ex: setas, F1-F12). */
using KeyPressCallback = std::function<void(uint8_t ascii, uint8_t hid_keycode, uint8_t modifiers)>;

/** Inicializa o host USB e começa a tarefa de leitura do teclado.
 *  Deve ser chamado uma vez, no início do app_main. */
void usb_hid_keyboard_init(KeyPressCallback on_key_press);
