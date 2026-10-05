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
#include "nvs.h"
#include "driver/gpio.h"
#include <stdint.h>
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
/* M4.14 #1: o gate de wake por toque é decidido AO ENTRAR em standby (o
 * config pode mudar em runtime; a ISR não pode ler pda_settings() com
 * segurança). OFF = toque não acorda (acidentes no bolso); BOOT/USB sim. */
static volatile bool s_touch_wake_ok = false;
/* M4.14: proveniência do wake p/ log de aceite: 0 nenh. 1 boot 2 toque 3 ativ. */
static volatile int s_wake_src = 0;
/* M4.14.4: borda do botão BOOT p/ o toggle de standby (nível baixo = press.). */
static volatile bool s_boot_was_down = false;
static pda_power_standby_cb s_standby_cb = NULL;
static void *s_standby_ctx = NULL;
static pda_power_hibernate_save_cb s_hib_save_cb = NULL;
static void *s_hib_ctx = NULL;
static SemaphoreHandle_t s_wake_sem = NULL;

#ifndef BOARD_BOOT_BTN_ACTIVE_LEVEL
#define BOARD_BOOT_BTN_ACTIVE_LEVEL 0   /* botão BOOT p/ GND (ativo baixo) */
#endif

/* ------------------------------------------------------------------ */
/* M4.14 #4: instrumentação de transições. Toda escrita de s_state passa
 * por set_state() com motivo; a anomalia "DIM (idle 21s/10s) logo após o
 * NTP" dos logs de validação de 2026-10-04 vira diagnosticável: quem
 * vira o estado deixa assinatura (motivo + idle no instante). */
static const char *state_name(pda_power_state_t st)
{
    switch (st) {
    case PDA_PWR_ACTIVE:   return "ACTIVE";
    case PDA_PWR_DIM:      return "DIM";
    default:               return "STANDBY";
    }
}

static const char *wake_src_name(int src)
{
    switch (src) {
    case 1:  return "botao BOOT";
    case 2:  return "toque";
    case 3:  return "atividade UI/USB";
    default: return "?";
    }
}

/* M5.0b (A4): high-water-mark de pilhas p/ catching stack-creep sem
 * debugger (histórico do projeto: ui_loop 8K→32K por fault). */
static void log_hwm(const char *when)
{
    static const char *names[] = { "main", "ui_loop", "pda_power", "wifi_net",
                                   "wifi_rcn", "io_edit", "io_list",
                                   "io_notes", "io_scripts", "lua_script" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(names[i]);
        if (h) {
            ESP_LOGI(TAG, "hwm[%s] %s: %u B livres no mínimo",
                     when, names[i],
                     (unsigned)uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t));
        }
    }
}

static void set_state(pda_power_state_t st, const char *why)
{
    if (s_state == st) return;
    const int64_t idle_s = (esp_timer_get_time() - s_last_activity_us) / 1000000LL;
    ESP_LOGI(TAG, "pwr: %s -> %s (%s, idle %lds)",
             state_name(s_state), state_name(st), why, (long)idle_s);
    s_state = st;
}

/* ------------------------------------------------------------------ */
void power_mgmt_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_state == PDA_PWR_STANDBY && s_wake_sem) {
        s_wake_src = 3;
        xSemaphoreGive(s_wake_sem);   /* modo idle: qualquer atividade acorda */
    }
    if (s_state == PDA_PWR_DIM && !s_manual_standby) {
        set_state(PDA_PWR_ACTIVE, "atividade UI/USB");
        board_display_backlight_set((uint8_t)pda_settings()->brightness);
    }
}

pda_power_state_t power_mgmt_state(void) { return s_state; }

/* M4.14.3: STANDBY com wake_on_touch OFF = não entregar toque ao Slint
 * (nem wake por callback de UI, nem action fantasma de elemento). */
bool power_mgmt_touch_blind(void)
{
    return s_state == PDA_PWR_STANDBY && !s_touch_wake_ok;
}

/** M4.15: a chave de wake por toque estava armada AO ENTRAR neste standby?
 *  (standby_cb usa p/ decidir se o GT911 dorme.) */
bool power_mgmt_touch_wake_armed(void) { return s_touch_wake_ok; }

void power_mgmt_request_standby(void)
{
    ESP_LOGI(TAG, "standby solicitado manualmente");
    s_manual_standby = true;    /* sticky: sobrevive à cauda de toque do tap */
    s_last_activity_us = 0;     /* idle "infinito" -> próximo tick dorme */
}

/* M4.13b: o hibernate entra em deep sleep SEM wake source (a volta é
 * RESET/power-on), então esp_reset_reason() NUNCA retorna
 * ESP_RST_DEEPSLEEP nesta placa — a restauração de sessão virou código
 * morto no M4.13 (relato do usuário na validação de 2026-10-04: "não
 * restaura a tela ao apertar RESET"). Cura: flag em NVS gravado antes do
 * deep sleep (flash interna: sobrevive a RESET, power-cycle e ao
 * storage_shutdown_sd() que desmonta o SD logo depois) e consumido no
 * primeiro boot que o encontrar. */
#define PWR_NVS_NS "pdapwr"

static void hibernate_flag_set(void)
{
    nvs_handle_t h;
    if (nvs_open(PWR_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "HIBERNATE: NVS indisponível — sessão NÃO será restaurada");
        return;
    }
    nvs_set_u8(h, "hib", 1);
    nvs_commit(h);
    nvs_close(h);
}

/* Consome o flag (apaga ao ler): um boot que não restaura não deixa o
 * seguinte restaurar de novo. */
static bool hibernate_flag_take(void)
{
    nvs_handle_t h;
    if (nvs_open(PWR_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, "hib", &v);
    nvs_close(h);
    if (err != ESP_OK || v == 0) return false;
    if (nvs_open(PWR_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "hib");
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

bool power_mgmt_woke_from_hibernate(void)
{
    const bool flag = hibernate_flag_take();      /* consome se existir */
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) return true;  /* wake futuro */
    return flag;                                  /* M4.13b: RESET/power-on */
}

void power_mgmt_set_standby_cb(pda_power_standby_cb cb, void *ctx)
{ s_standby_cb = cb; s_standby_ctx = ctx; }
void power_mgmt_set_hibernate_save_cb(pda_power_hibernate_save_cb cb, void *ctx)
{ s_hib_save_cb = cb; s_hib_ctx = ctx; }

/* ------------------------------------------------------------------ */
static void enter_standby(const char *why);
void power_mgmt_hibernate(void);

/* Wake por GPIO no modo idle (sem light sleep): ISR dá o semáforo.
 * M4.14 #1: toque só acorda se a chave estava ON ao entrar no standby. */
static void IRAM_ATTR wake_gpio_isr(void *arg)
{
    const int pin = (int)(intptr_t)arg;
    if (pin == BOARD_TOUCH_INT_GPIO && !s_touch_wake_ok) return;
    BaseType_t hw = pdFALSE;
    s_wake_src = (pin == BOARD_TOUCH_INT_GPIO) ? 2 : 1;
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

static void enter_standby(const char *why)
{
    const pda_settings_t *cfg = pda_settings();
    s_touch_wake_ok = cfg->wake_on_touch;   /* M4.14 #1: gate da ISR de toque */
    s_wake_src = 0;
    set_state(PDA_PWR_STANDBY, why);
    ESP_LOGI(TAG, "entrando em STANDBY (%s); wake armado: boot=sim touch=%s",
             power_mgmt_light_sleep_active() ? "light sleep" : "idle robusto",
             s_touch_wake_ok ? "sim" : "nao");

    log_hwm("standby");
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
        /* M4.15.2: bounce do BOOT na entrada — o negedge da MESMA pressão
         * que pediu o standby chega com a placa já dormindo (log do
         * usuário: wake 42 ms após o entry). Wake por BOOT em até 400 ms
         * da entrada = bounce: ignora e re-bloqueia. Pressão intencional
         * p/ acordar vem depois disso. */
        const int64_t t_enter_us = esp_timer_get_time();
        const int64_t deadline_us = t_enter_us +
                                    (int64_t)cfg->deep_sleep_after_s * 1000000LL;
        for (;;) {
            bool got;
            if (cfg->deep_sleep_after_s > 0) {
                const int64_t now_us = esp_timer_get_time();
                const int64_t remain_ms = (deadline_us - now_us) / 1000LL;
                if (remain_ms <= 0) {
                    ESP_LOGI(TAG, "idle longo -> hibernando");
                    power_mgmt_hibernate();   /* noreturn */
                }
                got = xSemaphoreTake(s_wake_sem,
                                     pdMS_TO_TICKS((uint32_t)remain_ms)) == pdTRUE;
                if (!got) continue;   /* reavalia o deadline */
            } else {
                xSemaphoreTake(s_wake_sem, portMAX_DELAY);
                got = true;
            }
            if (got && s_wake_src == 1 &&
                (esp_timer_get_time() - t_enter_us) < 400000LL) {
                ESP_LOGI(TAG, "bounce do BOOT ignorado (<400 ms no standby)");
                xSemaphoreTake(s_wake_sem, 0);
                continue;
            }
            break;
        }
    }

    /* ---- acordou ---- */
    s_manual_standby = false;   /* M4.13: pedido consumido (manual ou idle) */
    s_last_activity_us = esp_timer_get_time();
    const char *wk = wake_src_name(s_wake_src);
    s_wake_src = 0;
    set_state(PDA_PWR_ACTIVE, wk);

    board_display_panel_blank(false);
    board_display_backlight_set((uint8_t)pda_settings()->brightness);
    if (s_standby_cb) s_standby_cb(false, s_standby_ctx);
    ESP_LOGI(TAG, "acordou do STANDBY (wake: %s)", wk);
}

void power_mgmt_hibernate(void)
{
    ESP_LOGW(TAG, "HIBERNATE: salvando sessão e entrando em deep sleep");
    if (s_hib_save_cb) s_hib_save_cb(s_hib_ctx);
    hibernate_flag_set();
    ESP_LOGI(TAG, "HIBERNATE: flag NVS gravado — volta só por RESET/power-on "
                  "(sessão será restaurada no boot)");
    /* M4.15.2: REVERT do C6-off-no-hibernate. No P4 (XIP em PSRAM) o
     * gpio_force_hold_all() latcha pads de MSPI/SPI e a entrada do deep
     * sleep stallou: HP_SYS_HP_WDT_RESET (rst 0x7) no log do usuário de
     * 2026-10-05 + boot seguinte travado com latch residual. A receita da
     * esp-idf#18443 não é portável p/ esta placa; "C6 off no hibernate"
     * fica ABERTO p/ o hibernate v2 (Estágio 4) com outra abordagem. */
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
        /* M5.0b (A4): amostra única 1 min após o boot. */
        static bool hwm_boot = false;
        if (!hwm_boot && esp_timer_get_time() > 60000000LL) {
            hwm_boot = true;
            log_hwm("boot+60s");
        }
        const pda_settings_t *cfg = pda_settings();
        int64_t idle_s = (esp_timer_get_time() - s_last_activity_us) / 1000000LL;

        /* Toque em área vazia não gera callback de UI (Slint só reporta
         * elementos interativos), então o INT do GT911 é pollado aqui:
         * toque SEMPRE segura o idle e acorda de DIM; o wake do STANDBY
         * por toque é gateado SÓ na ISR pela chave (M4.14.2: semântica
         * pedida na validação — a chave desliga o wake, não a atividade). */
        if (gpio_get_level(BOARD_TOUCH_INT_GPIO) == 0) {
            power_mgmt_activity();
        }

        /* M4.14.4: BOOT como toggle p/ standby (opt-in por config): uma
         * pressão NOVA em ACTIVE/DIM pede standby (mesmo caminho sticky
         * do "Suspender"). Borda: pressão longa já em curso no boot
         * (strapping) não dispara; só release+pressão. */
        const bool boot_down = gpio_get_level(BOARD_BOOT_BTN_GPIO) == 0;
        if (boot_down && !s_boot_was_down && cfg->boot_btn_standby) {
            ESP_LOGI(TAG, "BOOT pressionado -> standby (opcao ligada)");
            power_mgmt_request_standby();
        }
        s_boot_was_down = boot_down;

        /* idle NOVO após os polls: activity() no meio do tick resetou o
         * relógio; usar o valor do topo re-dimerizava no mesmo tick
         * (churn DIM->ACTIVE->DIM visto no log de 2026-10-04). */
        idle_s = (esp_timer_get_time() - s_last_activity_us) / 1000000LL;

        switch (s_state) {
        case PDA_PWR_ACTIVE:
            if (s_manual_standby) {
                /* M4.13: escurece já (feedback visual) e o próximo tick
                 * entra em standby — sem esperar dim_after/screen_off. */
                set_state(PDA_PWR_DIM, "pedido manual (SUSPENDER)");
                board_display_backlight_set(12);
                ESP_LOGI(TAG, "SUSPENDER: DIM imediato (pedido manual)");
            } else if (idle_s >= cfg->dim_after_s) {
                set_state(PDA_PWR_DIM, "idle >= dim_after");
                board_display_backlight_set(12);
                ESP_LOGI(TAG, "DIM (idle %lds)", (long)idle_s);
            }
            break;
        case PDA_PWR_DIM:
            if (s_manual_standby) {
                enter_standby("pedido manual"); /* bloqueia até acordar */
            } else if (idle_s < cfg->dim_after_s) {
                set_state(PDA_PWR_ACTIVE, "idle voltou p/ < dim_after");
                board_display_backlight_set((uint8_t)cfg->brightness);
            } else if (idle_s >= cfg->screen_off_after_s) {
                enter_standby("idle >= screen_off"); /* bloqueia até acordar */
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
    /* M4.14.4: se o BOOT já está pressionado no boot (strapping/flash),
     * não contar como borda — só release+pressão nova pedem standby. */
    s_boot_was_down = gpio_get_level(BOARD_BOOT_BTN_GPIO) == 0;
    gpio_set_intr_type(BOARD_BOOT_BTN_GPIO, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(BOARD_BOOT_BTN_GPIO, wake_gpio_isr,
                         (void *)(intptr_t)BOARD_BOOT_BTN_GPIO);
    gpio_set_intr_type(BOARD_TOUCH_INT_GPIO, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(BOARD_TOUCH_INT_GPIO, wake_gpio_isr,
                         (void *)(intptr_t)BOARD_TOUCH_INT_GPIO);

    BaseType_t ok = xTaskCreate(power_task, "pda_power", 6144, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
