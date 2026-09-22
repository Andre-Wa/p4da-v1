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
        esp_pm
)

# CMP0116: sem isso o Ninja acha para sempre que o .cpp gerado a partir do
# .slint está desatualizado ("manifest still dirty after 100 tries").
cmake_policy(SET CMP0116 NEW)

# Compila ui/app_ui.slint -> C++ e linka no componente main.
slint_target_sources(${COMPONENT_LIB} ui/app_ui.slint)
