/* stub p/ compilar módulos IDF-dependent no host (tools/hosttest) */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t TickType_t;

#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
