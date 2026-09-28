#pragma once
#include "FreeRTOS.h"
struct FakeTask;
using TaskHandle_t = FakeTask*;
TaskHandle_t xTaskGetCurrentTaskHandle();
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
void vTaskDelay(TickType_t ticks);
