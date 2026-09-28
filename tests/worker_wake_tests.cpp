#include "worker_wake.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <thread>

struct FakeTask {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned notifications = 0;
    bool alive = true;
};
thread_local unsigned fake_critical_depth = 0;
thread_local FakeTask* current = nullptr;
std::atomic<unsigned> calls{0}, delay_calls{0};
std::atomic<bool> hold_notification{false}, inside_notification{false}, waiting{false};
std::atomic<TickType_t> last_ticks{0};
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); } } while (0)
void Require(bool value) { if (!value) std::abort(); }
TaskHandle_t xTaskGetCurrentTaskHandle() { Require(fake_critical_depth == 0); return current; }
void xTaskNotifyGive(TaskHandle_t task) {
    Require(fake_critical_depth == 0);
    ++calls; inside_notification = true;
    while (hold_notification.load()) std::this_thread::yield();
    { std::lock_guard<std::mutex> guard(task->mutex); Require(task->alive); ++task->notifications; }
    task->cv.notify_one();
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) {
    Require(fake_critical_depth == 0 && current);
    last_ticks = ticks; waiting = true;
    std::unique_lock<std::mutex> guard(current->mutex);
    if (ticks == portMAX_DELAY) current->cv.wait(guard, [] { return current->notifications != 0; });
    else if (ticks && !current->notifications) current->cv.wait_for(guard, std::chrono::milliseconds(ticks * 10));
    const unsigned count = current->notifications;
    if (clear) current->notifications = 0;
    else if (count) --current->notifications;
    return count;
}
void vTaskDelay(TickType_t) {
    Require(fake_critical_depth == 0);
    ++delay_calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
void Await(const std::atomic<bool>& condition) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition.load()) {
        Require(std::chrono::steady_clock::now() < until);
        std::this_thread::yield();
    }
}
int main() {
    agentlink::detail::WorkerWake wake;
    wake.wake(); CHECK(calls == 0);
    FakeTask task; current = &task; wake.attach();
    // An event between checking the work queue and sleeping stays latched.
    wake.wake(); wake.wake(); CHECK(task.notifications == 2);
    wake.wait(UINT32_MAX); CHECK(task.notifications == 0 && last_ticks == portMAX_DELAY);
    const uint32_t milliseconds[] = {0, 1, 9, 10, 11, 50, 1000, UINT32_MAX - 1};
    const TickType_t expected[] = {0, 1, 1, 1, 2, 5, 100, 429496730};
    for (unsigned i = 0; i < sizeof(milliseconds) / sizeof(milliseconds[0]); ++i) {
        wake.wake(); wake.wait(milliseconds[i]); CHECK(last_ticks == expected[i]);
    }
    wake.detach(); task.alive = false;
    const unsigned before = calls; wake.wake(); CHECK(calls == before);

    // A blocked worker is awakened immediately without a periodic timeout.
    std::atomic<bool> ready{false}, done{false}; waiting = false;
    FakeTask blocked;
    std::thread worker([&] {
        current = &blocked; wake.attach(); ready = true;
        wake.wait(UINT32_MAX); wake.detach(); blocked.alive = false; done = true;
    });
    Await(ready); Await(waiting); CHECK(!done.load()); wake.wake(); worker.join(); CHECK(done.load());

    // Teardown may race an already-issued wake, but cannot retire its TaskHandle
    // until the notifier has returned. New wakes see the closed lease gate.
    FakeTask retiring; current = &retiring; wake.attach();
    hold_notification = true; inside_notification = false; delay_calls = 0;
    std::thread notifier([&] { wake.wake(); });
    Await(inside_notification);
    std::atomic<bool> retired{false};
    std::thread closer([&] { wake.detach(); retiring.alive = false; retired = true; });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!delay_calls.load()) { Require(std::chrono::steady_clock::now() < until); std::this_thread::yield(); }
    CHECK(!retired.load());
    const unsigned leased_calls = calls; wake.wake(); CHECK(calls == leased_calls);
    hold_notification = false; notifier.join(); closer.join(); CHECK(retired.load());
    wake.wake(); CHECK(calls == leased_calls);
    std::printf("Worker wake lifecycle tests: %u assertions passed.\n", checks);
}
