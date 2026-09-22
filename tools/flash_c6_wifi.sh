#!/usr/bin/env bash
# flash_c6_wifi.sh — reflash do coprocessador ESP32-C6 (Wi-Fi/BT) da JC4880P443C_I_W
#
# POR QUÊ: o slave ESP-Hosted que vem de fábrica (2.3.0) não casa com o host
# atual (~2.12) => "Version mismatch", drops de SDIO, loop de ASSOC_LEAVE.
# A solução comprovada pela comunidade é flashear network_adapter 2.12.9.
#
# ANTES DE QUALQUER COISA: o P4 precisa estar HALTADO, senão o app rodando
# fica resetando o C6 (GPIO54) no meio do flash.
#
#   1) Segure BOOT do P4 ou:  esptool --chip esp32p4 --before default_reset \
#        --after no_reset flash_id        # deixa o P4 parado no bootloader
#   2) Conecte um adaptador USB-UART no header JP1:
#        UART-TX  -> C6_U0RXD
#        UART-RX  <- C6_U0TXD
#        GND      -> GND
#      (se não enumerar sozinho: C6_IO9 (BOOT) em GND durante o reset do C6)
#
# USO:  PORT=/dev/ttyUSB0 FW=network_adapter_esp32c6.bin ./flash_c6_wifi.sh
set -euo pipefail

PORT="${PORT:-/dev/ttyUSB0}"
FW="${FW:-network_adapter_esp32c6.bin}"   # baixe: https://esphome.github.io/esp-hosted-firmware/
CHIP="esp32c6"

if [[ ! -f "$FW" ]]; then
  echo "Erro: firmware '$FW' não encontrado no diretório atual." >&2
  echo "Baixe network_adapter_esp32c6.bin (2.12.x) de:" >&2
  echo "  https://esphome.github.io/esp-hosted-firmware/" >&2
  exit 1
fi

echo ">> P4 haltado? (Ctrl-C agora se não)"
sleep 3

esptool --chip "$CHIP" -p "$PORT" write_flash 0x10000 "$FW"
# aponta o bootloader para o app recém-gravado (ota_0)
esptool --chip "$CHIP" -p "$PORT" erase_region 0xd000 0x2000

echo ">> C6 reflasheado. Reinicie P4 e C6."
echo ">> No P4, o componente host (espressif/esp_hosted) deve casar com a"
echo ">> versão do slave: confira o log de boot (handshake SDIO)."
