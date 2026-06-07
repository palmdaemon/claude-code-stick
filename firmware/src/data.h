#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "xfer.h"
#include "asr_client.h"
#include "character.h"
#include "wifi_mgr.h"

// Defined in main.cpp; respects settings().sound. Lets bridge fire a
// completion chime via {"cmd":"chime","agent":"claude"}.
extern void beep(uint16_t freq, uint16_t dur);

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[8][92];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[44];
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Claude connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (xferCommand(doc)) { _lastLiveMs = millis(); return; }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components including weekday.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt; gmtime_r(&local, &lt);
    m5::rtc_datetime_t rdt;
    rdt.time.hours   = lt.tm_hour;
    rdt.time.minutes = lt.tm_min;
    rdt.time.seconds = lt.tm_sec;
    rdt.date.weekDay = lt.tm_wday;
    rdt.date.month   = lt.tm_mon + 1;
    rdt.date.date    = lt.tm_mday;
    rdt.date.year    = lt.tm_year + 1900;
    M5.Rtc.setDateTime(rdt);
    extern uint32_t _clkLastRead;
    _clkLastRead = 0;   // force re-read so _clkDt and _rtcValid agree
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char* m = doc["msg"];
  if (m) { strncpy(out->msg, m, sizeof(out->msg)-1); out->msg[sizeof(out->msg)-1]=0; }
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      strncpy(out->lines[n], s ? s : "", 91); out->lines[n][91]=0;
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
    strncpy(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool)-1); out->promptTool[sizeof(out->promptTool)-1]=0;
    strncpy(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint)-1); out->promptHint[sizeof(out->promptHint)-1]=0;
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }

  // Character state push from bridge: {"char_state":"working_typing"}.
  // Maps the state name to CharacterState enum and switches the active GIF.
  const char* cs = doc["char_state"];
  if (cs && characterLoaded()) {
    for (uint8_t i = 0; i < CHAR_STATE_COUNT; i++) {
      if (strcmp(cs, characterStateName((CharacterState)i)) == 0) {
        characterSetState((CharacterState)i);
        break;
      }
    }
  }

  // LAN ASR endpoint init handshake. Bridge replies to our {"cmd":"hello",
  // "my_ip":"..."} with {"cmd":"init","mac_ip":"...","mac_port":...}.
  const char* initCmd = doc["cmd"];
  // Completion chime: {"cmd":"chime","agent":"claude"|"codex"}.
  // Two-tone "ding-dong" (perfect 5th up). Claude rides the A5/E6 pair,
  // Codex sits an octave lower at E5/B5 — same character, distinct register
  // so they're distinguishable when both agents are active.
  // Volume bumped to 220 for presence, restored to 128 (mic.cpp's canonical
  // post-recording level) after. tone() is async; isPlaying() waits for
  // the queue to drain so the two notes don't overlap or clip.
  if (initCmd && !strcmp(initCmd, "chime")) {
    const char* agent = doc["agent"] | "claude";
    M5.Speaker.setVolume(220);
    if (!strcmp(agent, "codex")) {
      beep(660, 150);
      while (M5.Speaker.isPlaying()) delay(5);
      beep(990, 200);
    } else {
      beep(880, 150);
      while (M5.Speaker.isPlaying()) delay(5);
      beep(1320, 200);
    }
    while (M5.Speaker.isPlaying()) delay(5);
    M5.Speaker.setVolume(128);
    _lastLiveMs = millis();
    return;
  }
  if (initCmd && !strcmp(initCmd, "init")) {
    const char* mip = doc["mac_ip"];
    uint16_t    mp  = doc["mac_port"] | 0;
    if (mip && mp) asrSetMacEndpoint(mip, mp);
  }
  // WiFi provisioning from bridge: {"cmd":"wifi_add","ssid":"...","pass":"..."}
  // After save we kick a reconnect so the new network is tried immediately.
  // Open networks: pass empty string (firmware treats "" as no password).
  if (initCmd && !strcmp(initCmd, "wifi_add")) {
    const char* ssid = doc["ssid"];
    const char* pass = doc["pass"] | "";
    if (ssid && *ssid) {
      if (wifiAddNetwork(ssid, pass)) {
        Serial.printf("[wifi] added %s via BLE; reconnecting…\n", ssid);
        wifiReconnect();
      } else {
        Serial.printf("[wifi] add %s failed (storage full?)\n", ssid);
      }
    }
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf<1024> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  _usbLine.feed(Serial, out);
  // BLE ring buffer is drained manually since it's not a Stream.
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
