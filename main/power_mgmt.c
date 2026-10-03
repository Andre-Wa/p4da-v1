/**
 * power_mgmt.c — máquina de estados de energia em degraus.
 * Ver power_mgmt.h e docs/POWER.md para a justificativa de cada degrau
 * (incluindo por que deep sleep não é wake-able por botão nesta placa).
 */

#include "power_mgmt.h"
#include "board_config.h"
#include "pda_config.h"
#include "display_init.h"
#include "storage_init.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "power_mgmt";

static volatile int64_t s_last_activity_us = 0;
static volatile pda_power_state_t s_state = PDA_PWR_ACTIVE;
/* M4.13: pedido manual de standby PRECISA ser sticky. Antes a solicitação
 * só zerava o relógio de idle — e a "cauda" de eventos de toque do MESMO
 * tap no botão Suspender (o GT911 continua reportando por dezenas de ms
 * após o release; cada evento chama power_mgmt_activity()) religava o
 * relógio e o pedido evaporava em silêncio: o log mostrava "standby
 * solicitado manualmente" seguido de um DIM normal por idle. O flag só é
 * consumido ao entrar em standby; toque nenhum o cancela (quem pediu
 * suspende; para desistir, acorda de novo). */
static volatile bool s_manual_standby = false;
static pda_power_standby_cb s_standby_cb = NULL;
static void *s_standby_ctx = NULL;
static pda_power_hibernate_save_cb s_hib_save_cb = NULL;
static void *s_hib_ctx = NULL;
static SemaphoreHandle_t s_wake_sem = NULL;

#ifndef BOARD_BOOT_BTN_ACTIVE_LEVEL
#define BOARD_BOOT_BTN_ACTIVE_LEVEL 0   /* botão BOOT p/ GND (ativo baixo) */
#endif

/* ------------------------------------------------------------------ */
void power_mgmt_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_state == PDA_PWR_STANDBY && s_wake_sem) {
        xSemaphoreGive(s_wake_sem);   /* modo idle: qualquer atividade acorda */
    }
    if (s_state == PDA_PWR_DIM && !s_manual_standby) {
        s_state = PDA_PWR_ACTIVE;
        board_display_backlight_set((uint8_t)pda_settings()->brightness);
    }
}

pda_power_state_t power_mgmt_state(void) { return s_state; }

void power_mgmt_request_standby(void)
{
    ESP_LOGI(TAG, "standby solicitado manualmente");
    s_manual_standby = true;    /* sticky: sobrevive à cauda de toque do tap */
    s_last_activity_us = 0;     /* idle "infinito" -> próximo tick dorme */
}

bool power_mgmt_woke_from_hibernate(void)
{
    return esp_reset_reason() == ESP_RST_DEEPSLEEP;
}

void power_mgmt_set_standby_cb(pda_power_standby_cb cb, void *ctx)
{ s_standby_cb = cb; s_standby_ctx = ctx; }
void power_mgmt_set_hibernate_save_cb(pda_power_hibernate_save_cb cb, void *ctx)
{ s_hib_save_cb = cb; s_hib_ctx = ctx; }

/* ------------------------------------------------------------------ */
static void enter_standby(void);
void power_mgmt_hibernate(void);

/* Wake por GPIO no modo idle (sem light sleep): ISR dá o semáforo. */
static void IRAM_ATTR wake_gpio_isr(void *arg)
{
    (void)arg;
    BaseType_t hw = pdFALSE;
    xSemaphoreGiveFromISR(s_wake_sem, &hw);
    if (hw) portYIELD_FROM_ISR();
}

static void configure_wake_gpios(bool wake_on_touch)
{
    /* Botão BOOT: sempre candidato a wake (light sleep aceita qualquer GPIO). */
    gpio_config_t io = {0};
    io.pin_bit_mask = (1ULL << BOARD_BOOT_BTN_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_wakeup_enable(BOARD_BOOT_BTN_GPIO,
                       BOARD_BOOT_BTN_ACTIVE_LEVEL ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);

    if (wake_on_touch) {
        /* [HW?] INT do GT911: open-drain, idla alto, pulso baixo no toque.
         * Só leitura aqui — NÃO mexemos no reset do chip de propósito
         * (ver touch_init.c). Se o pino não for o 21 nesta unidade, o
         * efeito é "nunca acorda por toque" (inofensivo). */
        gpio_config_t io2 = {0};
        io2.pin_bit_mask = (1ULL << BOARD_TOUCH_INT_GPIO);
        io2.mode = GPIO_MODE_INPUT;
        io2.pull_up_en = GPIO_PULLUP_ENABLE;
        io2.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io2.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io2);
        gpio_wakeup_enable(BOARD_TOUCH_INT_GPIO, GPIO_INTR_LOW_LEVEL);
    }
    esp_sleep_enable_gpio_wakeup();
}

bool power_mgmt_light_sleep_active(void)
{
    /* 2026-09-24: o caminho light sleep crasha no wake (remount do SDMMC
     * compartilhado com o ESP-Hosted + Instruction access fault no core 1)
     * e o wake por BOTÃO/toque não disparava confiavelmente. Desativado
     * até o rework (ROADMAP "M-power"); o standby robusto cobre o uso
     * diário e o HIBERNATE cobre a economia máxima. */
    (void)0;
    return false;
}

static void enter_standby(void)
{
    const pda_settings_t *cfg = pda_settings();
    s_state = PDA_PWR_STANDBY;
    ESP_LOGI(TAG, "entrando em STANDBY (%s)",
             power_mgmt_light_sleep_active() ? "light sleep" : "idle robusto");

    if (s_standby_cb) s_standby_cb(true, s_standby_ctx);

    board_display_backlight_set(0);
    board_display_panel_blank(true);

    if (power_mgmt_light_sleep_active()) {
        /* Modo experimental (hoje inalcançável, ver kill-switch): menor consumo, MAS SDMMC/SDIO(hosted)/USB-DWC2
         * não sobrevivem ao light sleep no P4 — o wake exige remount do SD
         * e reinstall do USB (ver main.cpp standby_cb e docs/POWER.md). */
        configure_wake_gpios(cfg->wake_on_touch);
        if (cfg->deep_sleep_after_s > 0) {
            esp_sleep_enable_timer_wakeup((uint64_t)cfg->deep_sleep_after_s * 1000000ULL);
        }
        for (;;) {
            esp_light_sleep_start();
            esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
            if (cause == ESP_SLEEP_WAKEUP_GPIO) break;
            if (cause == ESP_SLEEP_WAKEUP_TIMER && cfg->deep_sleep_after_s > 0) {
                ESP_LOGI(TAG, "idle longo -> hibernando");
                power_mgmt_hibernate();
            }
        }
    } else {
        /* Modo robusto (default): CPU idle, periféricos VIVOS. O consumo
         * economizado vem de backlight 0 + painel blank. Wake por ISR de
         * GPIO (botão/toque) ou por qualquer atividade de UI/USB. */
        xSemaphoreTake(s_wake_sem, 0);   /* drain */
        if (cfg->deep_sleep_after_s > 0) {
            if (xSemaphoreTake(s_wake_sem,
                               pdMS_TO_TICKS((uint32_t)cfg->deep_sleep_after_s * 1000U)) == pdFALSE) {
                ESP_LOGI(TAG, "idle longo -> hibernando");
                power_mgmt_hibernate();   /* noreturn */
            }
        } else {
            xSemaphoreTake(s_wake_sem, portMAX_DELAY);
        }
    }

    /* ---- acordou ---- */
    s_manual_standby = false;   /* M4.13: pedido consumido (manual ou idle) */
    s_state = PDA_PWR_ACTIVE;
    s_last_activity_us = esp_timer_get_time();

    board_display_panel_blank(false);
    board_display_backlight_set((uint8_t)pda_settings()->brightness);
    if (s_standby_cb) s_standby_cb(false, s_standby_ctx);
    ESP_LOGI(TAG, "acordou do STANDBY");
}

void power_mgmt_hibernate(void)
{
    ESP_LOGW(TAG, "HIBERNATE: salvando sessão e entrando em deep sleep");
    if (s_hib_save_cb) s_hib_save_cb(s_hib_ctx);
    board_display_backlight_set(0);
    storage_shutdown_sd();          /* unmount + LDO off: sem corrupção */
    esp_deep_sleep_start();         /* sem wake source: volta no power-on */
}

/* ------------------------------------------------------------------ */
static void power_task(void *arg)
{
    (void)arg;
    s_last_activity_us = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        const pda_settings_t *cfg = pda_settings();
        int64_t idle_s = (esp_timer_get_time() - s_last_activity_us) / 1000000LL;

        /* Toque em área vazia não gera callback de UI (Slint só reporta
         * elementos interativos), então o INT do GT911 é pollado aqui:
         * com wake_on_touch ligado, toque segura o idle/acorda. */
        if (cfg->wake_on_touch &&
            gpio_get_level(BOARD_TOUCH_INT_GPIO) == 0) {
            power_mgmt_activity();
        }

        switch (s_state) {
        case PDA_PWR_ACTIVE:
            if (s_manual_standby) {
                /* M4.13: escurece já (feedback visual) e o próximo tick
                 * entra em standby — sem esperar dim_after/screen_off. */
                s_state = PDA_PWR_DIM;
                board_display_backlight_set(12);
                ESP_LOGI(TAG, "SUSPENDER: DIM imediato (pedido manual)");
            } else if (idle_s >= cfg->dim_after_s) {
                s_state = PDA_PWR_DIM;
                board_display_backlight_set(12);
                ESP_LOGI(TAG, "DIM (idle %lds)", (long)idle_s);
            }
            break;
        case PDA_PWR_DIM:
            if (s_manual_standby) {
                enter_standby(); /* bloqueia até acordar */
            } else if (idle_s < cfg->dim_after_s) {
                s_state = PDA_PWR_ACTIVE;
                board_display_backlight_set((uint8_t)cfg->brightness);
            } else if (idle_s >= cfg->screen_off_after_s) {
                enter_standby(); /* bloqueia até acordar */
            }
            break;
        case PDA_PWR_STANDBY:
            /* não ocorre: enter_standby() bloqueia esta task */
            break;
        }
    }
}

esp_err_t power_mgmt_init(void)
{
    if (pda_settings()->light_sleep) {
        ESP_LOGW(TAG, "power.light_sleep=true no config, mas o modo está "
                      "desativado (kill-switch) — standby robusto em uso");
    }
    s_wake_sem = xSemaphoreCreateBinary();
    if (!s_wake_sem) return ESP_FAIL;

    /* Configura OS PINOS de wake como entrada c/ pull-up AQUI: no modo
     * robusto configure_wake_gpios() (light sleep) nunca roda, e sem
     * input-enable + pull-up o ISR não dispara (botão/toque não acordavam). */
    gpio_config_t io = {0};
    io.pin_bit_mask = (1ULL << BOARD_BOOT_BTN_GPIO) | (1ULL << BOARD_TOUCH_INT_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    gpio_install_isr_service(0);
    gpio_set_intr_type(BOARD_BOOT_BTN_GPIO, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(BOARD_BOOT_BTN_GPIO, wake_gpio_isr, NULL);
    gpio_set_intr_type(BOARD_TOUCH_INT_GPIO, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(BOARD_TOUCH_INT_GPIO, wake_gpio_isr, NULL);

    BaseType_t ok = xTaskCreate(power_task, "pda_power", 6144, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
