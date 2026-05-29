#include "wifi_mgr.h"

#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoJson.h>

static constexpr const char* NVS_NS  = "wifi";
static constexpr const char* NVS_KEY = "nets";
static constexpr uint8_t     MAX_NETS = 5;

// Per-attempt timeout. ESP32 STA usually decides within 8s; 15s catches slow
// captive portals and weak signal handshakes.
static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
// Re-scan cadence when stuck in NO_KNOWN/NO_SAVED.
static constexpr uint32_t IDLE_RESCAN_MS     = 30000;
// Recovery delay after losing a connection before restarting scan.
static constexpr uint32_t RECOVERY_DELAY_MS  = 2000;

static WifiState   _state         = WIFI_BOOT;
static String      _connectedSsid = "";
static uint32_t    _stateAt       = 0;
static JsonDocument _nets;

// Per-scan ranked match list. SavedIdx points into the _nets JSON array.
struct Match { uint8_t savedIdx; int32_t rssi; };
static Match  _matches[MAX_NETS];
static uint8_t _matchCount  = 0;
static uint8_t _matchAttempt = 0;

// ── NVS persistence ──────────────────────────────────────────────────────

static void loadNets() {
  Preferences prefs;
  prefs.begin(NVS_NS, true);
  String json = prefs.getString(NVS_KEY, "[]");
  prefs.end();
  _nets.clear();
  DeserializationError err = deserializeJson(_nets, json);
  if (err || !_nets.is<JsonArray>()) {
    _nets.clear();
    _nets.to<JsonArray>();
  }
}

static void saveNets() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  String out;
  serializeJson(_nets, out);
  prefs.putString(NVS_KEY, out);
  prefs.end();
}

// ── Credential API ───────────────────────────────────────────────────────

bool wifiAddNetwork(const char* ssid, const char* pass) {
  if (!ssid || !*ssid) return false;
  JsonArray arr = _nets.as<JsonArray>();
  // Remove any existing entry with the same SSID (updating credentials).
  for (size_t i = 0; i < arr.size(); i++) {
    if (arr[i]["ssid"] == ssid) {
      arr.remove(i);
      break;
    }
  }
  // Cap list at MAX_NETS by dropping the oldest entry.
  while (arr.size() >= MAX_NETS) {
    arr.remove(arr.size() - 1);
  }
  // Newest entries go to the front (last-added wins ties when picking).
  arr.add<JsonObject>();
  for (size_t i = arr.size() - 1; i > 0; i--) {
    arr[i] = arr[i - 1];
  }
  JsonObject obj = arr[0].to<JsonObject>();
  obj["ssid"] = ssid;
  obj["pass"] = pass ? pass : "";
  saveNets();
  return true;
}

bool wifiDelNetwork(const char* ssid) {
  JsonArray arr = _nets.as<JsonArray>();
  for (size_t i = 0; i < arr.size(); i++) {
    if (arr[i]["ssid"] == ssid) {
      arr.remove(i);
      saveNets();
      return true;
    }
  }
  return false;
}

void wifiForgetAll() {
  _nets.clear();
  _nets.to<JsonArray>();
  saveNets();
}

uint8_t wifiSavedCount() {
  return _nets.as<JsonArray>().size();
}

const char* wifiSavedSsidAt(uint8_t idx) {
  JsonArray arr = _nets.as<JsonArray>();
  if (idx >= arr.size()) return nullptr;
  return arr[idx]["ssid"];
}

// ── State accessors ──────────────────────────────────────────────────────

WifiState   wifiState() { return _state; }
IPAddress   wifiIp()    { return WiFi.localIP(); }
const char* wifiSsid()  { return _connectedSsid.c_str(); }
int         wifiRssi()  { return WiFi.RSSI(); }

const char* wifiStateLabel() {
  switch (_state) {
    case WIFI_BOOT:       return "boot";
    case WIFI_SCANNING:   return "scan";
    case WIFI_CONNECTING: return "conn";
    case WIFI_CONNECTED:  return "ok";
    case WIFI_NO_KNOWN:   return "none";
    case WIFI_NO_SAVED:   return "cfg?";
  }
  return "?";
}

// ── Lifecycle ────────────────────────────────────────────────────────────

static void enterState(WifiState s) {
  _state   = s;
  _stateAt = millis();
}

static void startScan() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true);  // async; results polled via scanComplete()
  enterState(WIFI_SCANNING);
}

void wifiInit() {
  WiFi.persistent(false);   // we manage credentials in NVS ourselves
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // our own reconnect logic
  loadNets();
  if (wifiSavedCount() == 0) {
    enterState(WIFI_NO_SAVED);
  } else {
    startScan();
  }
}

void wifiReconnect() {
  WiFi.disconnect(true, false);  // disconnect + erase last AP, keep NVS
  _connectedSsid = "";
  if (wifiSavedCount() == 0) {
    enterState(WIFI_NO_SAVED);
  } else {
    startScan();
  }
}

// Find saved networks visible in the latest scan; rank by RSSI (strongest
// first). Drops scan results when done.
static void rankMatches(int16_t scanCount) {
  _matchCount   = 0;
  _matchAttempt = 0;
  JsonArray arr = _nets.as<JsonArray>();
  for (size_t s = 0; s < arr.size() && _matchCount < MAX_NETS; s++) {
    const char* savedSsid = arr[s]["ssid"];
    if (!savedSsid) continue;
    int32_t bestRssi = INT32_MIN;
    bool    found    = false;
    for (int i = 0; i < scanCount; i++) {
      if (WiFi.SSID(i) == savedSsid && WiFi.RSSI(i) > bestRssi) {
        bestRssi = WiFi.RSSI(i);
        found    = true;
      }
    }
    if (found) {
      _matches[_matchCount++] = { (uint8_t)s, bestRssi };
    }
  }
  // Insertion sort by RSSI descending. MAX_NETS is small.
  for (uint8_t i = 1; i < _matchCount; i++) {
    Match key = _matches[i];
    int8_t j = i - 1;
    while (j >= 0 && _matches[j].rssi < key.rssi) {
      _matches[j + 1] = _matches[j];
      j--;
    }
    _matches[j + 1] = key;
  }
  WiFi.scanDelete();
}

static void tryConnectMatch(uint8_t i) {
  JsonArray arr = _nets.as<JsonArray>();
  const char* ssid = arr[_matches[i].savedIdx]["ssid"];
  const char* pass = arr[_matches[i].savedIdx]["pass"];
  _connectedSsid = ssid;
  WiFi.begin(ssid, pass);
  enterState(WIFI_CONNECTING);
}

void wifiTick() {
  uint32_t now = millis();
  switch (_state) {
    case WIFI_SCANNING: {
      int16_t n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) return;
      if (n == WIFI_SCAN_FAILED) {
        // Restart scan after a moment.
        if (now - _stateAt > RECOVERY_DELAY_MS) startScan();
        return;
      }
      rankMatches(n);
      if (_matchCount == 0) {
        enterState(WIFI_NO_KNOWN);
        _connectedSsid = "";
        return;
      }
      tryConnectMatch(0);
      break;
    }

    case WIFI_CONNECTING: {
      wl_status_t s = WiFi.status();
      if (s == WL_CONNECTED) {
        // Modem sleep: AP keeps a unicast queue while the radio dozes
        // between DTIM beacons, cutting WiFi idle draw from ~80mA to
        // ~15mA. BLE notifies that arrive while we're dozing wake the
        // radio at the next beacon — adds ~100-300ms latency, fine for
        // our heartbeat/permission/char_state messages.
        WiFi.setSleep(true);
        enterState(WIFI_CONNECTED);
        return;
      }
      if (now - _stateAt < CONNECT_TIMEOUT_MS) return;
      // Fall through to next ranked match.
      _matchAttempt++;
      WiFi.disconnect(true, false);
      if (_matchAttempt >= _matchCount) {
        enterState(WIFI_NO_KNOWN);
        _connectedSsid = "";
        return;
      }
      tryConnectMatch(_matchAttempt);
      break;
    }

    case WIFI_CONNECTED: {
      if (WiFi.status() != WL_CONNECTED) {
        _connectedSsid = "";
        enterState(WIFI_NO_KNOWN);  // brief stop; rescan triggered below
      }
      break;
    }

    case WIFI_NO_KNOWN:
    case WIFI_NO_SAVED: {
      // Periodically rescan in case a saved network came online or user
      // added one via serial CLI.
      if (wifiSavedCount() > 0 && now - _stateAt > IDLE_RESCAN_MS) {
        startScan();
      }
      break;
    }

    default:
      break;
  }
}
