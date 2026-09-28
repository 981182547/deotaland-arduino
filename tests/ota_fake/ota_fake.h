#pragma once
// Host-only deterministic ESP-IDF / FreeRTOS boundary. Production ota_service.cpp
// is compiled unchanged against these API declarations and scheduler controls.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include <algorithm>
typedef int32_t esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM 2
#define MBEDTLS_VERSION_MAJOR 3
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffeu
#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_8BIT 2u
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1u
#define pdMS_TO_TICKS(ms) (ms)
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
struct esp_partition_t { uint32_t size; };
struct esp_app_desc_t { char version[32]; };
typedef uint32_t esp_ota_handle_t;
enum esp_ota_img_states_t { ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID };
struct mbedtls_sha256_context { size_t count; };
struct FakeQueue { size_t limit, item_bytes; std::deque<std::vector<uint8_t>> items; };
struct FakeStream { size_t limit; std::deque<uint8_t> data; };
struct FakeTask { void (*function)(void*); void* context; uint32_t notifications; };
using QueueHandle_t=FakeQueue*;
using StreamBufferHandle_t=FakeStream*;
using TaskHandle_t=FakeTask*;
namespace fake {
static unsigned allocation_failure=0, stream_creates=0, queue_creates=0, chunk_creates=0, task_creates=0;
static uint32_t ticks=0;
static esp_partition_t target{1024u*1024u}, previous{1024u*1024u};
static const esp_partition_t* next_partition=&target;
static const esp_partition_t* boot_partition=&previous;
static FakeTask* task=nullptr;
static size_t receive_limit=SIZE_MAX;
static size_t begin_size=0;
static unsigned begin_calls=0, end_calls=0, abort_calls=0, restarts=0, receive_calls=0;
static unsigned write_failure=0;
static bool begin_failure=false, end_failure=false, bad_hash=false;
static std::vector<std::vector<uint8_t>> writes;
static void (*delay_hook)()=nullptr;
static void (*receive_hook)()=nullptr;
static void (*write_hook)()=nullptr;
struct IdleWait {};
}
inline StreamBufferHandle_t xStreamBufferCreate(size_t capacity,size_t) {
    ++fake::stream_creates;
    return fake::allocation_failure==1 ? nullptr : new FakeStream{capacity,{}};
}
inline QueueHandle_t xQueueCreate(size_t capacity,size_t item_bytes) {
    ++fake::queue_creates;
    return fake::allocation_failure==2 ? nullptr : new FakeQueue{capacity,item_bytes,{}};
}
inline void* heap_caps_malloc(size_t bytes,unsigned flags) {
    ++fake::chunk_creates;
    if (flags!=(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)) std::abort();
    return fake::allocation_failure==3 ? nullptr : std::malloc(bytes);
}
inline BaseType_t xTaskCreate(void (*function)(void*),const char*,uint32_t,void* context,UBaseType_t,TaskHandle_t* out) {
    ++fake::task_creates;
    if (fake::allocation_failure==4) return 0;
    fake::task=new FakeTask{function,context,0}; *out=fake::task; return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t task) { ++task->notifications; }
inline uint32_t ulTaskNotifyTake(BaseType_t,TickType_t) {
    if (!fake::task->notifications) throw fake::IdleWait{};
    const uint32_t notifications=fake::task->notifications; fake::task->notifications=0; return notifications;
}
inline void vTaskDelay(TickType_t ticks) { fake::ticks+=ticks; if (fake::delay_hook) fake::delay_hook(); }
inline TickType_t xTaskGetTickCount() { return fake::ticks; }
inline BaseType_t xStreamBufferReset(StreamBufferHandle_t buffer) { buffer->data.clear(); return pdPASS; }
inline BaseType_t xQueueReset(QueueHandle_t queue) { queue->items.clear(); return pdPASS; }
inline size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t buffer) { return buffer->limit-buffer->data.size(); }
inline size_t xStreamBufferSend(StreamBufferHandle_t buffer,const void* ptr,size_t n,TickType_t) {
    const uint8_t* bytes=static_cast<const uint8_t*>(ptr);
    const size_t take=std::min(n,xStreamBufferSpacesAvailable(buffer));
    buffer->data.insert(buffer->data.end(),bytes,bytes+take); return take;
}
inline size_t xStreamBufferReceive(StreamBufferHandle_t buffer,void* ptr,size_t n,TickType_t ticks) {
    ++fake::receive_calls; if (fake::receive_hook) fake::receive_hook();
    const size_t take=std::min(std::min(n,buffer->data.size()),fake::receive_limit);
    uint8_t* bytes=static_cast<uint8_t*>(ptr);
    for (size_t i=0;i<take;++i) { bytes[i]=buffer->data.front(); buffer->data.pop_front(); }
    if (!take) fake::ticks+=ticks;
    return take;
}
inline BaseType_t xQueueSend(QueueHandle_t queue,const void* ptr,TickType_t) {
    if (queue->items.size()==queue->limit) return 0;
    const uint8_t* bytes=static_cast<const uint8_t*>(ptr);
    queue->items.emplace_back(bytes,bytes+queue->item_bytes); return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t queue,void* out,TickType_t) {
    if (queue->items.empty()) return 0;
    std::memcpy(out,queue->items.front().data(),queue->item_bytes); queue->items.pop_front(); return pdTRUE;
}
inline const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) { return fake::next_partition; }
inline const esp_partition_t* esp_ota_get_boot_partition() { return fake::boot_partition; }
inline const esp_partition_t* esp_ota_get_running_partition() { return &fake::previous; }
inline esp_err_t esp_ota_begin(const esp_partition_t*,size_t bytes,esp_ota_handle_t* out) {
    ++fake::begin_calls; fake::begin_size=bytes; *out=17;
    return fake::begin_failure ? ESP_FAIL : ESP_OK;
}
inline esp_err_t esp_ota_write(esp_ota_handle_t,const void* ptr,size_t bytes) {
    if (fake::write_hook) fake::write_hook();
    const uint8_t* data=static_cast<const uint8_t*>(ptr);
    fake::writes.emplace_back(data,data+bytes);
    return fake::write_failure==fake::writes.size() ? ESP_FAIL : ESP_OK;
}
inline esp_err_t esp_ota_abort(esp_ota_handle_t) { ++fake::abort_calls; return ESP_OK; }
inline esp_err_t esp_ota_end(esp_ota_handle_t) { ++fake::end_calls; return fake::end_failure ? ESP_FAIL : ESP_OK; }
inline esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition) { fake::boot_partition=partition; return ESP_OK; }
inline void esp_restart() { ++fake::restarts; }
inline const esp_app_desc_t* esp_app_get_description() { static const esp_app_desc_t app={{'1','.','0',0}}; return &app; }
inline esp_err_t esp_ota_get_state_partition(const esp_partition_t*,esp_ota_img_states_t* out) { *out=ESP_OTA_IMG_VALID; return ESP_OK; }
inline esp_err_t esp_ota_mark_app_valid_cancel_rollback() { return ESP_OK; }
inline void mbedtls_sha256_init(mbedtls_sha256_context* ctx) { ctx->count=0; }
inline int mbedtls_sha256_starts(mbedtls_sha256_context*,int) { return 0; }
inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx,const uint8_t*,size_t n) { ctx->count+=n; return 0; }
inline int mbedtls_sha256_finish(mbedtls_sha256_context*,uint8_t* out) { std::memset(out,fake::bad_hash ? 1 : 0,32); return 0; }
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
