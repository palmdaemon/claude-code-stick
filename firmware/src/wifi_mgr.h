#pragma once
#include <stdint.h>
#include <IPAddress.h>

// Multi-network WiFi manager.
// Stores up to MAX_NETS network credentials in NVS Preferences. On boot or
// reconnect, scans visible APs, picks the saved network with strongest RSSI,
// and connects. Falls back across remaining matches on connect failure.

enum WifiState : uint8_t {
  WIFI_BOOT,        // setup not run yet
  WIFI_SCANNING,    // looking for known networks
  WIFI_CONNECTING,  // mid connect attempt
  WIFI_CONNECTED,
  WIFI_NO_KNOWN,    // none of the saved networks are in range
  WIFI_NO_SAVED,    // no networks saved at all
};

void wifiInit();
void wifiTick();

WifiState   wifiState();
IPAddress   wifiIp();
const char* wifiSsid();    // currently connected SSID ("" if not)
int         wifiRssi();
const char* wifiStateLabel();  // short label for UI

// Credential storage (persisted in NVS).
bool        wifiAddNetwork(const char* ssid, const char* pass);
bool        wifiDelNetwork(const char* ssid);
void        wifiForgetAll();
uint8_t     wifiSavedCount();
const char* wifiSavedSsidAt(uint8_t idx);

// Force a fresh scan + connect cycle (call after credential changes).
void wifiReconnect();
