#pragma once
// A dedicated worker's notification slot is its event latch. The handle is
// leased while notifying, so stop cannot notify a deleted task. FreeRTOS calls
// stay outside our spinlock. Only worker teardown waits for an in-flight lease;
// wake() neither allocates nor waits on another task.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <atomic>
namespace agentlink { namespace detail {
class WorkerWake {
public:
    void attach() {
        const TaskHandle_t self = xTaskGetCurrentTaskHandle();
        taskENTER_CRITICAL(&lock_);
        task_ = self;
        taskEXIT_CRITICAL(&lock_);
    }
    void detach() {
        taskENTER_CRITICAL(&lock_);
        task_ = nullptr;
        taskEXIT_CRITICAL(&lock_);
        while (users_.load(std::memory_order_acquire)) vTaskDelay(1);
    }
    void wake() {
        taskENTER_CRITICAL(&lock_);
        const TaskHandle_t task = task_;
        if (task) users_.fetch_add(1, std::memory_order_relaxed);
        taskEXIT_CRITICAL(&lock_);
        if (task) {
            xTaskNotifyGive(task);
            users_.fetch_sub(1, std::memory_order_release);
        }
    }
    static void wait(uint32_t milliseconds) {
        TickType_t ticks = portMAX_DELAY;
        if (milliseconds != UINT32_MAX) {
            // Round up so a sub-tick deadline cannot spin the worker. Finite
            // waits must never overflow into FreeRTOS's infinite-wait sentinel.
            const uint64_t rounded = (uint64_t(milliseconds) * configTICK_RATE_HZ + 999) / 1000;
            ticks = rounded >= portMAX_DELAY ? portMAX_DELAY - 1 : static_cast<TickType_t>(rounded);
        }
        (void)ulTaskNotifyTake(pdTRUE, ticks);
    }
private:
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    TaskHandle_t task_ = nullptr;
    std::atomic<unsigned> users_{0};
};
} }
