#include "recorder.h"

#include <M5Unified.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "wifi_mgr.h"
#include "asr_client.h"
#include "mic.h"
#include "character.h"

// Last transcript exposed to callers (e.g. screen display, debug).
static char _last_text[512] = "";
const char* recorderLastText() { return _last_text; }

extern LGFX_Sprite spr;
extern void sendCmd(const char* json);

// ── REC UI: bottom half of 240px screen ─────────────────────────────────
static constexpr int REC_Y0 = 130;
static constexpr int REC_H  = 110;
static constexpr int W      = 135;

static void drawRecUI(uint32_t elapsed_ms, uint32_t rms,
                      const char* status_label,
                      const char* preview) {
  spr.fillRect(0, REC_Y0, W, REC_H, 0x0000);

  spr.setTextSize(1);
  spr.setTextColor(0xF800, 0x0000);
  spr.setCursor(4, REC_Y0 + 2);
  spr.printf("\xe2\x97\x8f %s %us", status_label,
             (unsigned)(elapsed_ms / 1000));

  // Audio level bar.
  int bar_w = (int)((rms > 2000 ? 2000 : rms) * 127 / 2000);
  spr.fillRect(4, REC_Y0 + 16, 127, 6, 0x2104);
  uint16_t bar_color = (rms < 200) ? 0xFC60
                     : (rms < 1500) ? 0x07E0
                     : 0xF800;
  spr.fillRect(4, REC_Y0 + 16, bar_w, 6, bar_color);

  spr.setTextColor(0xC618, 0x0000);
  spr.setCursor(4, REC_Y0 + 28);
  if (preview && preview[0]) spr.print(preview);

  spr.setTextColor(0x8410, 0x0000);
  spr.setCursor(4, REC_Y0 + REC_H - 10);
  spr.print("B: cancel");

  spr.pushSprite(0, 0);
}

// Max recording: 30 sec at 16kHz mono 16-bit = 960KB. Easily fits PSRAM.
// Hard cap matches Dashscope 5-min limit and our practical workflow.
static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr uint32_t MAX_SECONDS = 30;
static constexpr size_t   MAX_SAMPLES = SAMPLE_RATE * MAX_SECONDS;

int recorderRun() {
  _last_text[0] = 0;

  if (wifiState() != WIFI_CONNECTED) {
    Serial.println("[rec] no wifi");
    drawRecUI(0, 0, "WAIT", "no wifi");
    delay(1200);
    return -1;
  }
  // advisor: race guard. The Mac bridge may not have sent its init
  // handshake yet when the user mashes KEY1 immediately after BLE comes
  // up. Bail with a screen message instead of failing silently in the
  // upload path.
  if (!asrHasEndpoint()) {
    Serial.println("[rec] no asr endpoint (bridge not ready)");
    drawRecUI(0, 0, "WAIT", "bridge not ready");
    delay(1200);
    return -1;
  }

  drawRecUI(0, 0, "REC", "");
  // Buddy goes into "juggling" while we record — visual cue that the mic
  // is hot. After upload completes, the bridge pushes an emotion-mapped
  // state (or the 6s tool decay restores idle) so we don't snap back here.
  characterSetState(CHAR_WORKING_JUGGLING);
  Serial.println("[rec] mic begin…");
  if (!micBegin(SAMPLE_RATE)) {
    Serial.println("[rec] mic begin fail");
    characterSetState(CHAR_IDLE);
    return -1;
  }

  // Allocate one large PSRAM buffer for the whole recording.
  int16_t* buf = (int16_t*)heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    Serial.println("[rec] psram buf alloc fail");
    micEnd();
    return -1;
  }

  constexpr size_t CHUNK = 1600;   // 100ms
  size_t total_samples = 0;
  uint32_t t0 = millis();
  uint32_t last_ui = 0;
  uint32_t last_rms = 0;
  bool cancelled = false;

  while (true) {
    M5.update();
    if (M5.BtnB.wasPressed()) {
      cancelled = true;
      Serial.println("[rec] KEY2 cancel");
      break;
    }
    if (!M5.BtnA.isPressed()) break;          // released → commit
    if (total_samples + CHUNK > MAX_SAMPLES) break;  // 30s cap

    if (!micRecord(buf + total_samples, CHUNK)) {
      Serial.println("[rec] mic record fail");
      cancelled = true;
      break;
    }
    int64_t ss = 0;
    for (size_t i = 0; i < CHUNK; i++) {
      int16_t v = buf[total_samples + i];
      ss += (int32_t)v * v;
    }
    last_rms = (uint32_t)sqrtf((float)ss / (float)CHUNK);
    total_samples += CHUNK;

    uint32_t now = millis();
    if (now - last_ui > 100) {
      drawRecUI(now - t0, last_rms, "REC", "");
      last_ui = now;
    }
  }

  micEnd();

  if (cancelled || total_samples == 0) {
    Serial.printf("[rec] aborted, samples=%u\n", (unsigned)total_samples);
    free(buf);
    characterSetState(CHAR_IDLE);
    return 0;
  }

  uint32_t dur_ms = (total_samples * 1000U) / SAMPLE_RATE;
  Serial.printf("[rec] captured %u samples (%ums); uploading…\n",
                (unsigned)total_samples, (unsigned)dur_ms);
  drawRecUI(dur_ms, 0, "SEND", "");

  AsrResult r = asrLanUpload(buf, total_samples, SAMPLE_RATE);
  free(buf);

  if (!r.ok) {
    Serial.printf("[rec] asr fail http=%d err=%s\n", r.http_status, r.error);
    drawRecUI(dur_ms, 0, "FAIL", r.error);
    characterSetState(CHAR_DIZZY);   // sad-face on failure
    delay(1500);
    characterSetState(CHAR_IDLE);
    return -1;
  }
  Serial.printf("[rec] FINAL: \"%s\" emotion=%s\n", r.text, r.emotion);
  drawRecUI(dur_ms, 0, "OK", r.text);
  strncpy(_last_text, r.text, sizeof(_last_text) - 1);
  // Don't set a character state here — bridge pushes the emotion-mapped
  // state via BLE during our blocking asrLanUpload, and that notify is
  // queued in rxBuf. main loop's dataPoll will apply it once we return.
  return 1;
}
