#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * usb_host_state.h — a USB Host Library já foi instalada?
 *
 * O install único da Host Library mora em usb_hid_keyboard.cpp
 * (usb_host_lib_task). audio_uac.c é o 2º client e, se chamar
 * uac_host_install() ANTES do host existir, o componente falha e loga
 * "E uac-host: Unable to register USB Host client" (inofensivo: o retry
 * de 200 ms resolvia — mas era uma linha E fixa em todo boot, log de
 * 2026-10-11). Esperar por este flag torna o 1º attempt determinístico.
 * vira false de novo no teardown do light sleep (prepare_sleep) e true
 * no reinstall do wake.
 */
bool usb_host_state_ready(void);

#ifdef __cplusplus
}
#endif
