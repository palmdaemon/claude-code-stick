#include "serial_cli.h"
#include "wifi_mgr.h"
#include "mic.h"
#include "asr_client.h"
#include "character.h"
#include "recorder.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <math.h>

static String _buf;

static void printHelp() {
  Serial.println(F("commands:"));
  Serial.println(F("  wifi add <ssid> <password>   add/update saved network"));
  Serial.println(F("  wifi del <ssid>              forget one network"));
  Serial.println(F("  wifi list                    list saved networks"));
  Serial.println(F("  wifi forget                  forget all networks"));
  Serial.println(F("  wifi scan                    rescan + reconnect now"));
  Serial.println(F("  wifi status                  show current connection"));
  Serial.println(F("  mic test [seconds]           record + analyze level (default 2s)"));
  Serial.println(F("  mic stream [seconds]         record + POST to Mac bridge LAN ASR (default 3s)"));
  Serial.println(F("  mic gain <0..7>              set ES8311 PGA gain (0=0dB, 7=30dB)"));
  Serial.println(F("  asr endpoint                 show current Mac bridge ip:port"));
  Serial.println(F("  character <state>            switch buddy state (idle/happy/working_typing/...)"));
  Serial.println(F("  character list               list all available states"));
  Serial.println(F("  help                         this"));
}

// LAN HTTP upload: record N seconds locally, POST raw PCM to Mac bridge.
// Mirrors recorder.cpp's hot path so this CLI command tests the production
// data path end-to-end. No TLS on the device side — bridge calls Dashscope.
static void handleMicStream(const String& args) {
  uint32_t secs = 3;
  String a = args; a.trim();
  if (a.length() > 0) {
    long n = a.toInt();
    if (n >= 1 && n <= 30) secs = (uint32_t)n;
  }

  if (wifiState() != WIFI_CONNECTED) {
    Serial.println(F("[stream] no wifi; use 'wifi status'"));
    return;
  }
  if (!asrHasEndpoint()) {
    Serial.println(F("[stream] no endpoint — start bridge.py and wait for init"));
    return;
  }

  Serial.printf("[stream] endpoint=%s:%u\n",
                asrEndpointIp(), (unsigned)asrEndpointPort());

  constexpr uint32_t SR = 16000;
  size_t total = SR * secs;
  int16_t* buf = (int16_t*)heap_caps_malloc(total * sizeof(int16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { Serial.println(F("alloc fail")); return; }

  if (!micBegin(SR)) {
    Serial.println(F("mic begin fail"));
    free(buf);
    return;
  }

  Serial.printf("\n>>> SPEAK NOW for %us (starting in 1.5s)... <<<\n", (unsigned)secs);
  Serial.flush();
  delay(1500);
  Serial.println(F(">>> GO! <<<"));
  Serial.flush();

  uint32_t t0 = millis();
  bool ok = micRecord(buf, total);
  uint32_t rec_ms = millis() - t0;
  micEnd();
  if (!ok) {
    Serial.println(F("[stream] mic record failed"));
    free(buf);
    return;
  }
  MicStats s = micAnalyze(buf + 500, total - 500);
  Serial.printf("[stream] recorded %ums; peak=%u rms=%u\n",
                (unsigned)rec_ms, (unsigned)s.peak, (unsigned)s.rms);
  if (s.peak < 500) {
    Serial.println(F("[stream] !! audio very quiet — louder/closer or 'mic gain N'"));
  }

  AsrResult r = asrLanUpload(buf, total, SR);
  free(buf);

  if (!r.ok) {
    Serial.printf("[stream] FAIL http=%d err=%s\n", r.http_status, r.error);
  } else {
    Serial.printf("[stream] OK: \"%s\"  emotion=%s\n", r.text, r.emotion);
  }
}

static void handleMicTest(const String& args) {
  uint32_t secs = 2;
  String a = args; a.trim();
  if (a.length() > 0) {
    long n = a.toInt();
    if (n >= 1 && n <= 10) secs = (uint32_t)n;
  }
  constexpr uint32_t SR = 16000;
  size_t samples = SR * secs;
  size_t bytes   = samples * sizeof(int16_t);

  int16_t* buf = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
  if (!buf) {
    Serial.printf("alloc %u bytes (PSRAM) failed\n", (unsigned)bytes);
    return;
  }

  if (!micBegin(SR)) {
    Serial.println(F("mic begin failed (ES8311 not init? speaker still on?)"));
    free(buf);
    return;
  }

  Serial.printf("[mic] recording %us @ %uHz (%u samples)...\n",
                (unsigned)secs, (unsigned)SR, (unsigned)samples);
  uint32_t t0 = millis();
  bool ok = micRecord(buf, samples);
  uint32_t took = millis() - t0;

  if (!ok) {
    Serial.println(F("mic record failed"));
  } else {
    MicStats s = micAnalyze(buf, samples);
    Serial.printf("[mic] %u samples in %ums (target %ums)\n",
                  (unsigned)samples, (unsigned)took, (unsigned)(secs * 1000));
    Serial.printf("[mic] min=%d max=%d peak=%u rms=%u zc=%u\n",
                  s.minSample, s.maxSample,
                  (unsigned)s.peak, (unsigned)s.rms, (unsigned)s.zeroCrossings);
    // RMS excluding the first 500 startup samples (ADC settling transient).
    if (samples > 1000) {
      MicStats s2 = micAnalyze(buf + 500, samples - 500);
      Serial.printf("[mic] post-startup (skip first 500): peak=%u rms=%u zc=%u\n",
                    (unsigned)s2.peak, (unsigned)s2.rms, (unsigned)s2.zeroCrossings);
    }
    // Sample mid + late chunks to see steady-state behavior over time.
    auto dump = [&](const char* label, size_t off, size_t n) {
      if (off + n > samples) return;
      Serial.printf("[mic] %s=", label);
      for (size_t i = 0; i < n; i++) Serial.printf("%d,", buf[off + i]);
      Serial.println();
    };
    dump("startup[0..15]", 0, 16);
    if (samples >= 16016) dump("mid[16000..16015]", 16000, 16);
    if (samples >= 48016) dump("late[48000..48015]", 48000, 16);
    Serial.printf("[mic] interpretation: ");
    if (s.peak < 50)         Serial.println(F("SILENT (no signal; check ES8311 init)"));
    else if (s.peak < 500)   Serial.println(F("very quiet — try speaking louder"));
    else if (s.peak < 5000)  Serial.println(F("normal speech level"));
    else if (s.peak < 28000) Serial.println(F("loud — should ASR fine"));
    else                     Serial.println(F("CLIPPING — too loud, attenuate"));
  }

  micEnd();
  free(buf);
}

static void printWifiStatus() {
  Serial.printf("state: %s  ssid: \"%s\"  ip: %s  rssi: %d  saved: %u\n",
    wifiStateLabel(),
    wifiSsid(),
    wifiIp().toString().c_str(),
    wifiRssi(),
    (unsigned)wifiSavedCount());
}

// Split "wifi add MyNet my pass with spaces" on the FIRST space after the
// sub-command, taking the second token (SSID) as one word and the rest as
// the password. Passwords with spaces work; SSIDs with spaces don't.
static void handleWifiAdd(const String& args) {
  int sp = args.indexOf(' ');
  if (sp <= 0 || sp >= (int)args.length() - 1) {
    Serial.println(F("usage: wifi add <ssid> <password>"));
    return;
  }
  String ssid = args.substring(0, sp);
  String pass = args.substring(sp + 1);
  ssid.trim();
  if (ssid.length() == 0) {
    Serial.println(F("ssid empty"));
    return;
  }
  if (wifiAddNetwork(ssid.c_str(), pass.c_str())) {
    Serial.printf("added: %s\n", ssid.c_str());
    wifiReconnect();
  } else {
    Serial.println(F("add failed"));
  }
}

static void handleWifi(const String& rest) {
  int sp = rest.indexOf(' ');
  String sub  = (sp < 0) ? rest : rest.substring(0, sp);
  String args = (sp < 0) ? ""   : rest.substring(sp + 1);
  sub.toLowerCase();

  if (sub == "" || sub == "status") {
    printWifiStatus();
  } else if (sub == "add") {
    handleWifiAdd(args);
  } else if (sub == "del") {
    String ssid = args; ssid.trim();
    if (ssid.length() == 0) {
      Serial.println(F("usage: wifi del <ssid>"));
      return;
    }
    if (wifiDelNetwork(ssid.c_str())) {
      Serial.printf("deleted: %s\n", ssid.c_str());
    } else {
      Serial.println(F("not found"));
    }
  } else if (sub == "list") {
    uint8_t n = wifiSavedCount();
    Serial.printf("saved networks (%u):\n", (unsigned)n);
    for (uint8_t i = 0; i < n; i++) {
      Serial.printf("  %u. %s\n", (unsigned)(i + 1), wifiSavedSsidAt(i));
    }
  } else if (sub == "forget") {
    wifiForgetAll();
    Serial.println(F("all networks forgotten"));
    wifiReconnect();
  } else if (sub == "scan") {
    wifiReconnect();
    Serial.println(F("rescanning..."));
  } else {
    Serial.println(F("usage: wifi [add|del|list|forget|scan|status]"));
  }
}

static void dispatch(const String& line) {
  if (line.length() == 0) return;
  int sp = line.indexOf(' ');
  String cmd  = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? ""   : line.substring(sp + 1);
  cmd.toLowerCase();

  if (cmd == "wifi") {
    handleWifi(rest);
  } else if (cmd == "mic") {
    int sp = rest.indexOf(' ');
    String sub  = (sp < 0) ? rest : rest.substring(0, sp);
    String args = (sp < 0) ? ""   : rest.substring(sp + 1);
    sub.toLowerCase();
    if (sub == "test")        handleMicTest(args);
    else if (sub == "stream") handleMicStream(args);
    else if (sub == "gain") {
      String a = args; a.trim();
      if (a.length() == 0) {
        Serial.printf("[mic.gain] current=%u (applied at every micBegin)\n",
                      micGetPgaGain());
        return;
      }
      long n = a.toInt();
      if (n < 0 || n > 7) {
        Serial.println(F("usage: mic gain <0..7>  (PGA: 0=0dB, 1=6dB, ... 7≈+30dB)"));
      } else {
        micSetPgaGain((uint8_t)n);
        Serial.printf("[mic.gain] set to %ld (applies on next mic test/stream)\n", n);
      }
    }
    else Serial.println(F("usage: mic [test|stream|gain]"));
  } else if (cmd == "record") {
    // Simulates KEY1 long-press: enters recorderRun. User must be PRESSING
    // KEY1 throughout for the loop to consider it "held"; otherwise it'll
    // exit immediately on release-check.
    Serial.println("[cli] invoking recorderRun (hold KEY1 to record!)");
    int rc = recorderRun();
    Serial.printf("[cli] recorderRun rc=%d\n", rc);
  } else if (cmd == "asr") {
    String sub = rest; sub.trim(); sub.toLowerCase();
    if (sub == "endpoint" || sub == "") {
      if (asrHasEndpoint()) {
        Serial.printf("endpoint: %s:%u\n",
                      asrEndpointIp(), (unsigned)asrEndpointPort());
      } else {
        Serial.println(F("endpoint: (not set — bridge has not sent init)"));
      }
    } else {
      Serial.println(F("usage: asr endpoint"));
    }
  } else if (cmd == "character" || cmd == "char") {
    String name = rest; name.trim();
    if (name.length() == 0 || name == "list") {
      Serial.println(F("available character states:"));
      for (uint8_t i = 0; i < CHAR_STATE_COUNT; i++) {
        Serial.printf("  %s\n", characterStateName((CharacterState)i));
      }
      Serial.printf("current: %s  loaded=%d\n",
                    characterStateName(characterCurrentState()),
                    (int)characterLoaded());
    } else {
      bool matched = false;
      for (uint8_t i = 0; i < CHAR_STATE_COUNT; i++) {
        if (name == characterStateName((CharacterState)i)) {
          characterSetState((CharacterState)i);
          Serial.printf("[char] set state -> %s\n", name.c_str());
          matched = true;
          break;
        }
      }
      if (!matched) Serial.printf("[char] unknown state '%s' (try 'character list')\n",
                                  name.c_str());
    }
  } else if (cmd == "help" || cmd == "?") {
    printHelp();
  } else {
    Serial.println(F("unknown command. try 'help'"));
  }
}

void serialCliInit() {
  _buf.reserve(256);
  Serial.println();
  Serial.println(F("[serial-cli] ready. type 'help' for commands."));
}

void serialCliTick() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = _buf;
      _buf = "";
      line.trim();
      dispatch(line);
    } else if (_buf.length() < 240) {
      _buf += c;
    }
  }
}
