#include "asr_client.h"

#include <Arduino.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// ── Endpoint state (set via BLE init handshake) ─────────────────────────
static char     _mac_ip[40] = {0};
static uint16_t _mac_port   = 0;

void asrSetMacEndpoint(const char* ip, uint16_t port) {
  if (!ip || !*ip || port == 0) return;
  strncpy(_mac_ip, ip, sizeof(_mac_ip) - 1);
  _mac_ip[sizeof(_mac_ip) - 1] = 0;
  _mac_port = port;
  Serial.printf("[asr] endpoint set: %s:%u\n", _mac_ip, (unsigned)_mac_port);
}

void asrClearMacEndpoint() {
  _mac_ip[0] = 0;
  _mac_port = 0;
}

bool        asrHasEndpoint()  { return _mac_ip[0] != 0 && _mac_port != 0; }
const char* asrEndpointIp()   { return _mac_ip; }
uint16_t    asrEndpointPort() { return _mac_port; }

// ── Upload ──────────────────────────────────────────────────────────────

AsrResult asrLanUpload(const int16_t* pcm, size_t samples,
                       uint32_t sample_rate) {
  AsrResult out{false, "", "", 0, ""};
  if (!asrHasEndpoint()) {
    snprintf(out.error, sizeof(out.error), "no endpoint (bridge not ready)");
    return out;
  }
  if (!pcm || samples == 0) {
    snprintf(out.error, sizeof(out.error), "no audio");
    return out;
  }

  size_t body_bytes = samples * sizeof(int16_t);
  Serial.printf("[asr] POST http://%s:%u/audio  %u bytes raw PCM @ %uHz\n",
                _mac_ip, (unsigned)_mac_port,
                (unsigned)body_bytes, (unsigned)sample_rate);
  uint32_t t0 = millis();

  WiFiClient client;
  client.setTimeout(10);   // seconds, mostly affects connect/read
  if (!client.connect(_mac_ip, _mac_port)) {
    snprintf(out.error, sizeof(out.error), "tcp connect %s:%u fail",
             _mac_ip, (unsigned)_mac_port);
    return out;
  }

  // Plain HTTP/1.1 with Content-Length. No TLS so no mbedtls.
  char hdr[256];
  int hlen = snprintf(hdr, sizeof(hdr),
      "POST /audio HTTP/1.1\r\n"
      "Host: %s:%u\r\n"
      "X-Sample-Rate: %u\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Content-Length: %u\r\n"
      "Connection: close\r\n\r\n",
      _mac_ip, (unsigned)_mac_port,
      (unsigned)sample_rate, (unsigned)body_bytes);
  if (client.write((const uint8_t*)hdr, hlen) != (size_t)hlen) {
    snprintf(out.error, sizeof(out.error), "header write fail");
    client.stop();
    return out;
  }

  // Body in 4KB chunks with backpressure-aware retry. Even without TLS,
  // PSRAM source bounces through internal RAM in the WiFi stack; if very
  // large bodies fail, tighten chunk size — but try 4KB first.
  constexpr size_t CHUNK = 4096;
  size_t off = 0;
  uint32_t stall_start = millis();
  while (off < body_bytes) {
    size_t k = body_bytes - off;
    if (k > CHUNK) k = CHUNK;
    size_t wrote = client.write(((const uint8_t*)pcm) + off, k);
    if (wrote > 0) {
      off += wrote;
      stall_start = millis();
    } else {
      if (millis() - stall_start > 10000) {
        snprintf(out.error, sizeof(out.error),
                 "body write stall at %u/%u",
                 (unsigned)off, (unsigned)body_bytes);
        client.stop();
        return out;
      }
      delay(5);
    }
  }
  Serial.printf("[asr] body sent in %ums; awaiting response\n",
                (unsigned)(millis() - t0));

  // Status line.
  String status_line = client.readStringUntil('\n');
  int status = 0;
  if (sscanf(status_line.c_str(), "HTTP/%*s %d", &status) != 1) {
    snprintf(out.error, sizeof(out.error), "bad status: %s",
             status_line.c_str());
    client.stop();
    return out;
  }
  out.http_status = status;

  // Drain headers (empty line ends them).
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line.length() <= 1) break;   // \r alone
  }

  // Parse JSON body.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, client);
  client.stop();
  if (err) {
    snprintf(out.error, sizeof(out.error), "json: %s", err.c_str());
    return out;
  }

  if (status != 200) {
    const char* msg = doc["error"] | doc["detail"] | "(no detail)";
    strncpy(out.error, msg, sizeof(out.error) - 1);
    out.error[sizeof(out.error) - 1] = 0;
    return out;
  }

  const char* text = doc["text"] | "";
  const char* emo  = doc["emotion"] | "";
  strncpy(out.text, text, sizeof(out.text) - 1);
  out.text[sizeof(out.text) - 1] = 0;
  strncpy(out.emotion, emo, sizeof(out.emotion) - 1);
  out.emotion[sizeof(out.emotion) - 1] = 0;
  out.ok = (out.text[0] != 0);
  Serial.printf("[asr] %ums total; text=%s\n",
                (unsigned)(millis() - t0), out.text);
  return out;
}
