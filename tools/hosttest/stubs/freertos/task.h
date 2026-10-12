/* stub p/ compilar módulos IDF-dependent no host (tools/hosttest).
 * xTaskCreate NÃO roda a função: os testes exercitam só a lógica pura
 * (battery_ingest), nunca as tasks. */
#pragma once
#include "FreeRTOS.h"

typedef void *TaskHandle_t;

static inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name,
                                     uint32_t stack, void *arg,
                                     UBaseType_t prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (out) *out = NULL;
    return pdPASS;
}

static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline void vTaskDelete(TaskHandle_t h) { (void)h; }
