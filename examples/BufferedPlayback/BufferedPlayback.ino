/* 进阶播放缓冲：展示部分接收、背压和真实水位，不含板级扬声器驱动。
 * 默认驱动不消费 PCM，因此运行本例不会出声；请替换驱动再测试。
 * 内置 BLE 的 TTS 需要 CoC；缓冲容量不能宣称大于真实 RAM/DMA 容量。
 */
#include <Deotaland.h>
#include <cstring>

namespace {

// PCM16 mono at 16 kHz uses 32 bytes per millisecond. This fixed application
// queue holds 256 ms. Its storage and the SDK's PCM staging are separate.
constexpr size_t kQueueBytes = 8192;
constexpr size_t kHardwareChunkBytes = 512;
int16_t pcmRing[kQueueBytes / sizeof(int16_t)] = {};
size_t head = 0;
size_t used = 0;
bool started = false;
bool playbackReady = false;
esp_err_t startError = ESP_OK, supportError = ESP_ERR_INVALID_STATE, playbackError = ESP_OK;
uint32_t lastDiagnostic = 0;
bool diagnosticPending = true;

// Inspect these in a debugger; there is no per-packet Serial output in loop().
volatile uint32_t shortAccepts = 0;
volatile uint32_t invalidDriverReturns = 0;
volatile uint32_t receivedEnds = 0;

size_t boardWritePcmNonBlocking(const uint8_t* pcm, size_t bytes) {
  // Replace with your board's I2S/DMA enqueue using ZERO timeout. Return only
  // the even number of bytes actually accepted; 0 means try again next loop.
  // Copy accepted bytes into fixed driver-owned DMA buffers before returning:
  // the ring storage may then be reused. Do not leave DMA pointing at this
  // borrowed ring memory after return. Initialize the driver/buffers in setup.
  // Never wait for playback, allocate a buffer, or use a blocking I2S write.
  (void)pcm;
  (void)bytes;
  // No board driver is connected. Deliberate rejection fills the ring and
  // exercises real SDK backpressure; this example does NOT play any sound.
  return 0;
}

size_t boardQueuedPcmBytes() {
  // Return unplayed PCM bytes already accepted by the board driver, INCLUDING
  // DMA buffers still being played. Read a bounded/atomic driver snapshot.
  // There are no DMA buffers in this unconnected example, so the value is 0.
  return 0;
}

void boardCancelPcmNonBlocking() {
  // When adding hardware, cancel/discard the old session's queued DMA here
  // without waiting. Disconnect must not leave old audio for the next peer.
}

size_t acceptPcm(const uint8_t* pcm, size_t bytes, void*) {
  const size_t available = kQueueBytes - used;
  const size_t accepted = (bytes < available ? bytes : available) & ~size_t(1);
  if (accepted < bytes) shortAccepts = shortAccepts + 1;
  if (!accepted) return 0;
  uint8_t* ring = reinterpret_cast<uint8_t*>(pcmRing);
  const size_t tail = (head + used) % kQueueBytes;
  const size_t contiguous = kQueueBytes - tail;
  const size_t first = accepted < contiguous ? accepted : contiguous;
  std::memcpy(ring + tail, pcm, first);
  if (first < accepted) std::memcpy(ring, pcm + first, accepted - first);
  used += accepted;
  // The SDK retains and retries the unaccepted suffix on a later poll().
  return accepted;
}

size_t queuedPcm(void*) {
  // Count each byte once: application-owned ring plus driver-owned/DMA bytes.
  // Saturation also protects the sum if a driver reports an unknown/full queue.
  const size_t hardware = boardQueuedPcmBytes();
  return hardware > SIZE_MAX - used ? SIZE_MAX : used + hardware;
}

void onAudioEnd(void*) {
  receivedEnds = receivedEnds + 1;
  // END means the complete PCM samples were accepted into the application.
  // Do not clear the ring here: it and the DMA queue may still contain sound.
  // Physical playback is empty only when queuedPcm(nullptr) reaches zero.
}

void onState(agent_state_t state, void*) {
  if (state != AGENT_STATE_READY) {
    head = used = 0;
    boardCancelPcmNonBlocking();
  }
}

void serviceSpeaker() {
  if (!used) return;
  size_t bytes = kQueueBytes - head;
  if (bytes > used) bytes = used;
  if (bytes > kHardwareChunkBytes) bytes = kHardwareChunkBytes;
  const uint8_t* ring = reinterpret_cast<const uint8_t*>(pcmRing);
  const size_t accepted = boardWritePcmNonBlocking(ring + head, bytes);
  if (accepted > bytes || (accepted & 1u)) {
    invalidDriverReturns = invalidDriverReturns + 1;  // Never advance past data.
    return;
  }
  head = (head + accepted) % kQueueBytes;
  used -= accepted;
}

void printDiagnostic() {
  Serial.println("BufferedPlayback：播放背压骨架，未接扬声器驱动，默认不会出声；? 可重看诊断。");
  if (startError != ESP_OK) {
    Serial.print("启动失败: "); Serial.println(Deotaland.errorName(startError));
    Serial.println(Deotaland.errorHint(startError));
  } else if (supportError == ESP_ERR_NOT_SUPPORTED) {
    Serial.println("当前后端不支持 CoC，TTS 下行不可用，已停止播放任务。请检查 BLE 目标及本库自带的 NimBLE 配置，见 docs/BLE-COC.zh-CN.md。");
  } else if (supportError != ESP_OK) {
    Serial.print("CoC 能力检查失败: "); Serial.println(Deotaland.errorName(supportError));
    Serial.println(Deotaland.errorHint(supportError));
  } else if (playbackError != ESP_OK) {
    Serial.print("播放配置失败: "); Serial.println(Deotaland.errorName(playbackError));
    Serial.println(Deotaland.errorHint(playbackError));
  } else {
    Serial.print("控制状态: "); Serial.println(Deotaland.stateName(Deotaland.state()));
    Serial.print("待消费 PCM 字节: "); Serial.println(used);
    Serial.println("缓冲满后将产生背压；真实播放须实现 boardWritePcmNonBlocking、boardQueuedPcmBytes 和取消驱动。");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  agent_output_cb_t output = {};
  output.on_audio_end = onAudioEnd;
  startError = Deotaland.begin("Buffered-Playback", AGENT_CAP_SPEAKER, &output, onState);
  started = startError == ESP_OK;
  if (!started) return;
  agent_stream_status_t status = {};
  // 本例使用内置 BLE：AUDIO 与 TTS 共用 CoC 编译能力。此检查不是 CUSTOM 后端的通用 TTS 探测。
  // 只看返回码，连接前的 UNAVAILABLE 状态不代表后端缺少能力。
  supportError = Deotaland.streamStatus(AGENT_STREAM_AUDIO, &status);
  if (supportError != ESP_OK) return;
  agent_playback_config_t playback = {};
  playback.write = acceptPcm;
  playback.queued = queuedPcm;
  playbackError = Deotaland.configurePlayback(&playback);
  if (playbackError != ESP_OK) {
    Deotaland.end();
    started = false;
    return;
  }
  Deotaland.setPlaybackBufferMs(kQueueBytes / 32);
  // The declared capacity is conservative. Add actual DMA capacity / 32 only
  // after implementing the driver, while queuedPcm continues to include it.
  playbackReady = true;
}

void loop() {
  if (started) Deotaland.poll(4, 1000);
  const uint32_t now = millis();
  for (uint8_t n = 0; n < 8 && Serial.available(); ++n)
    if (Serial.read() == '?') diagnosticPending = true;
  // 连续输入 ? 也限频，避免串口日志挤占 poll。
  if ((diagnosticPending && uint32_t(now - lastDiagnostic) >= 500) ||
      uint32_t(now - lastDiagnostic) >= 10000) {
    diagnosticPending = false; lastDiagnostic = now;
    printDiagnostic();
  }
  if (!playbackReady) return;
  serviceSpeaker();  // One bounded attempt per loop; no while(queue-not-empty).
  // poll's budget cannot preempt your callbacks or hardware driver. Each must
  // return promptly. The ring is touched only on this loop thread, not an ISR.
}
