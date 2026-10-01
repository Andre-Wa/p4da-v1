idf_component_register(
    SRCS
        "main.cpp"
        "display_init.c"
        "touch_init.c"
        "usb_hid_keyboard.cpp"
        "storage_init.c"
        "pda_config.c"
        "lua_runtime.c"
        "power_mgmt.c"
        "wifi_net.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        usb
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        nvs_flash
        esp_psram
        fatfs
        sdmmc
        littlefs
        lua
        esp_wifi
        esp_netif
        esp_event
        lwip
        esp_pm
        esp_timer
        pthread
)

# ---------------------------------------------------------------------------
# ABI do espressif/lua 5.5.0 — OBRIGATÓRIO, não remover.
#
# O componente compila a VM com LUA_32BITS=1 (lua_Number=float 32 bits,
# lua_Integer=int 32 bits), mas esse define é PRIVATE no CMakeLists dele
# (idf-extra-components/lua/CMakeLists.txt: "target_compile_definitions(...
# PRIVATE LUA_32BITS=1)"; bug relatado em espressif/developer-portal
# discussion #188, comentário de 2026-07-24). Sem o define aqui, o main
# compila contra os headers com lua_Number=double/lua_Integer=int64 e o
# link é feito contra uma VM float/int32:
#   * lua_tonumber devolve float nos 32 bits baixos de fa0; ler como double
#     dá lixo finito -> (int) satura em INT_MAX no RISC-V (fcvt.w.d). Foi a
#     causa raiz do "veneno INT_MAX" do system.lua (boots 2026-09-24..30) —
#     o cartão SD era inocente.
#   * lua_tointeger "sobrevivia" por sorte: o psABI RISC-V garante retorno
#     int32 sign-extended em a0, e o caller lia int64 de a0.
#   * lua_pushnumber(double) do nosso lado era lido como float pela VM
#     (pda.settings.get de scripts devolvia lixo).
# O _Static_assert no topo de pda_config.c falha o build com mensagem clara
# se um dia este define sumir ou o componente mudar de ABI.
# ---------------------------------------------------------------------------
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_32BITS=1)

# CMP0116: sem isso o Ninja acha para sempre que o .cpp gerado a partir do
# .slint está desatualizado ("manifest still dirty after 100 tries").
cmake_policy(SET CMP0116 NEW)

# Compila ui/app_ui.slint -> C++ e linka no componente main.
slint_target_sources(${COMPONENT_LIB} ui/app_ui.slint)
