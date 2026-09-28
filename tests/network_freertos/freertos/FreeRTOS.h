#pragma once
// Deliberately tiny host model for worker_wake.h lifecycle tests. It checks that
// kernel calls are outside the application's critical section; not an RTOS emulator.
#include <stdint.h>
#include <mutex>
using TickType_t = uint32_t;
using BaseType_t = int;
constexpr BaseType_t pdTRUE = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define configTICK_RATE_HZ 100
struct portMUX_TYPE { std::mutex mutex; };
#define portMUX_INITIALIZER_UNLOCKED {}
extern thread_local unsigned fake_critical_depth;
inline void taskENTER_CRITICAL(portMUX_TYPE* lock) { lock->mutex.lock(); ++fake_critical_depth; }
inline void taskEXIT_CRITICAL(portMUX_TYPE* lock) { --fake_critical_depth; lock->mutex.unlock(); }
