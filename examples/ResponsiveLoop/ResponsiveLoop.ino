/* 进阶非阻塞循环：观察预算和最长循环间隔，演示定时停止动作。
 * 默认 GPIO=-1，无真实执行器。使用调试器观察统计，无串口逐包输出。
 * poll 的时间预算不能打断已进入的回调，所以回调/驱动自身也必须快速返回。
 */
#include <Deotaland.h>

// Keep callbacks short: store the target, then let loop() service the hardware.
// No synchronous Serial output is used in the busy loop. Inspect these counters
// with a debugger or copy them into your application's low-rate status display.
volatile uint32_t worstLoopUs=0;
volatile uint32_t worstLoopGapUs=0;
volatile uint32_t worstPollUs=0;
volatile uint32_t overBudget=0;
bool started=false;
esp_err_t startError=ESP_OK;
esp_err_t lastBatteryError=ESP_OK;  // Inspect with the timing counters in a debugger.
uint32_t lastBatteryMs=0, lastBatteryAttemptMs=0;
uint32_t hapticStartedMs=0, hapticDurationMs=0;
bool hapticActive=false;
uint32_t lastLoopBegan=0;
bool haveLoopTime=false;
// Set an ordinary GPIO LED pin for your board. -1 leaves the demo disconnected.
// Some LED_BUILTIN implementations drive addressable LEDs and can wait in a driver.
constexpr int kDemoLedPin=-1;
bool ledHigh=false;

void setup() {
  if(kDemoLedPin>=0) { pinMode(kDemoLedPin,OUTPUT); digitalWrite(kDemoLedPin,LOW); }
  agent_output_cb_t out={};
  out.on_haptic=[](uint32_t durationMs,void*) {
    hapticStartedMs=millis(); hapticDurationMs=durationMs; hapticActive=true;
  };
  startError=Deotaland.begin("Responsive-Device",AGENT_CAP_BATTERY|AGENT_CAP_HAPTIC,&out);
  started=startError==ESP_OK;
}

void loop() {
  const uint32_t began=micros();
  // Also capture scheduling gaps outside poll(), including other RTOS tasks.
  if(haveLoopTime) {
    const uint32_t gap=began-lastLoopBegan;
    if(gap>worstLoopGapUs) worstLoopGapUs=gap;
  }
  lastLoopBegan=began; haveLoopTime=true;
  if(started) {
    // Stop after four work units or 1 ms, whichever happens first. A callback
    // already executing cannot be preempted, so drivers must return promptly.
    Deotaland.poll(4,1000);
    const uint32_t now=millis();
    if(hapticActive && uint32_t(now-hapticStartedMs)>=hapticDurationMs) hapticActive=false;
    if(kDemoLedPin>=0 && ledHigh!=hapticActive) {
      ledHigh=hapticActive; digitalWrite(kDemoLedPin,ledHigh?HIGH:LOW);
    }
    if(Deotaland.isReady() && uint32_t(now-lastBatteryMs)>=5000 &&
        uint32_t(now-lastBatteryAttemptMs)>=100) {
      lastBatteryAttemptMs=now;
      lastBatteryError=Deotaland.reportBattery(80,false);
      // 队列满/超时最多每 100ms 重试；永久错误结束本轮，避免每轮刷同一个错误。
      if(!Deotaland.isRetryable(lastBatteryError)) lastBatteryMs=now;
    }
    const auto stats=Deotaland.stats();
    worstPollUs=stats.poll_max_us; overBudget=stats.poll_budget_exhaustions;
  }
  const uint32_t spent=micros()-began;
  if(spent>worstLoopUs) worstLoopUs=spent;
  // Stream producers should write a bounded chunk (e.g. 512 bytes) per loop.
  // Advance their offset only on ESP_OK; keep the same bytes on NO_MEM/TIMEOUT.
}
