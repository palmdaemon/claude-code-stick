#pragma once
#include <stdint.h>
#include <stddef.h>

// LAN-proxy ASR client.
//
// ESP32 = wireless microphone; Mac bridge.py runs an HTTP server, receives
// raw int16 PCM via POST /audio, and proxies to Dashscope qwen3-asr-flash.
// No TLS on the device side — no mbedtls fragment / TLS context / internal
// RAM contention issues we hit when ESP32 talked to dashscope directly.
//
// Endpoint discovery: the bridge tells us its IP+port over BLE NUS via
// {"cmd":"init","mac_ip":"...","mac_port":...}. data.h calls
// asrSetMacEndpoint when it sees that message.

// Configure the upload endpoint. Cleared (port=0) on disconnect to avoid
// reuse of a stale IP from a previous Mac.
void asrSetMacEndpoint(const char* mac_ip, uint16_t mac_port);
void asrClearMacEndpoint();
bool asrHasEndpoint();
const char* asrEndpointIp();     // last-known mac_ip (read-only, may be "")
uint16_t    asrEndpointPort();   // last-known port (0 if not set)

struct AsrResult {
  bool ok;
  char text[512];
  char emotion[24];
  int  http_status;     // HTTP status from Mac bridge (0 on transport error)
  char error[128];
};

// Blocking upload. `pcm` is 16-bit signed little-endian mono PCM at
// `sample_rate` Hz. Sends raw PCM bytes (no WAV header, no base64) to
// the Mac bridge; bridge wraps WAV and calls Dashscope.
AsrResult asrLanUpload(const int16_t* pcm, size_t samples,
                       uint32_t sample_rate = 16000);
