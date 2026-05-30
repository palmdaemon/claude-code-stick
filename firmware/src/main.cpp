// StickS3 port of anthropics/claude-desktop-buddy
// Hardware diffs from StickC Plus:
//   Display : ST7789P3 same res (135x240), different init — M5Unified handles it
//   IMU     : BMI270 (vs MPU6886) — M5Unified handles it
//   Power   : M5PM1 (vs AXP192)  — M5Unified handles it via M5.Power
//   Audio   : ES8311 codec + AW8737 amp — M5.Speaker handles it
//   LED     : No user-controllable LED on StickS3 — removed
//   Buttons : Same logical KEY1(A)/KEY2(B) layout, different GPIO (G11/G12)
//             Power button via M5.BtnPWR (M5Unified)

#include <M5Unified.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "stats.h"      // must precede data.h (data.h calls statsOnBridgeTokens)
#include "character.h"  // must precede data.h (Palette struct)
#include "buddy.h"
#include "data.h"
#include "wifi_mgr.h"
#include "serial_cli.h"
#include "asr_client.h"
#include "recorder.h"

// M5GFX sprite (LGFX_Sprite replaces TFT_eSprite from TFT_eSPI)
LGFX_Sprite spr(&M5.Display);

static char btName[16] = "Echo";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Echo-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

const int W = 135, H = 240;
const int CX = W / 2;
const int CY_BASE = 120;
// LED_PIN removed — StickS3 has no user LED

const uint16_t HOT   = 0xFA20;
const uint16_t PANEL = 0x2104;

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

bool    menuOpen    = false;
uint8_t menuSel     = 0;
// 0..4 maps to setBrightness 50..250 (stepped by 50)
uint8_t brightLevel = 4;
bool    btnALong    = false;
bool    btnBLong    = false;
// After voice recording finishes and text is pasted to host, a short KEY1
// press within this window pushes Enter via the bridge (option C: paste +
// optional one-button send). Outside the window, KEY1 short cycles display.
uint32_t sendPendingUntil = 0;
static const char* _moodLabel = nullptr;  // updated each loop by characterIdleMoodTick; nullptr = no burst

// ── Boot status UI ──────────────────────────────────────────────────────
// Stages cleanly advance: wifi → ble → init → done. BOOT_BLE has an 8s
// timeout: if no bridge client connects (Mac asleep/lid closed/out of range),
// we drop to BOOT_OFFLINE — buddy keeps living, just no permission/recording.
// If BLE later comes up, OFFLINE promotes back to BOOT_INIT.
// The UI is a mid-lower banner (y=128-206) that NEVER blocks character render
// — clawd (135x120) lives in y=0-120, so banner doesn't overlap.
enum BootStage : uint8_t {
  BOOT_WIFI,        // wifi connecting / no saved network
  BOOT_BLE,         // wifi ok, awaiting BLE client (bridge.py scan/connect)
  BOOT_INIT,        // BLE client linked, awaiting hello/init handshake
  BOOT_DONE,        // everything ready — buddy live + bridge linked
  BOOT_OFFLINE,     // BLE timeout — buddy live, no bridge (degraded mode)
};
static BootStage _bootStage = BOOT_WIFI;
static uint32_t  _bootDoneAt = 0;    // millis when we entered DONE/OFFLINE (for brief banner flash)
static uint32_t  _bleStartedAt = 0;  // millis when we entered BOOT_BLE (for 8s timeout)
static const uint32_t BLE_WAIT_MS    = 8000;  // 8s then fall to OFFLINE
static const uint32_t BANNER_HOLD_MS = 5000;  // keep banner 5s after stable state reached

enum DisplayMode { DISP_NORMAL, DISP_INFO };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = true;   // Phase 1: always ASCII
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;

static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

static bool isFaceDown() {
  float ax, ay, az;
  // M5Unified IMU API: getAccel (vs StickCPlus getAccelData)
  M5.Imu.getAccel(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

// StickS3: M5.Display.setBrightness(0-255) replaces M5.Axp.ScreenBreath(7-100)
static void applyBrightness() {
  M5.Display.setBrightness(50 + brightLevel * 50);  // 50/100/150/200/250
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Display.wakeup();
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}

bool responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  // StickS3: M5.Speaker replaces M5.Beep; ES8311 initialized by M5.begin()
  if (settings().sound) M5.Speaker.tone(freq, dur);
}

// Externally-linked so recorder.cpp can send ASR text via the same BLE TX.
void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

const uint8_t INFO_PAGES = 5;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 4;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  spr.fillSprite(0x0000);
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "transcript", "clock rot", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 7;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "factory reset", "back" };
const uint8_t RESET_N = 2;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0: brightLevel = (brightLevel + 1) % 5; applyBrightness(); return;
    case 1: s.sound = !s.sound; break;
    case 2: s.hud = !s.hud; break;
    case 3: s.clockRot = (s.clockRot + 1) % 3; break;
    case 4: nextPet(); return;
    case 5: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 6: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;
  if (idx == 1) { resetOpen = false; return; }  // back
  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }
  beep(800, 200);
  // factory reset: NVS + filesystem + BLE bonds
  _prefs.begin("buddy", false);
  _prefs.clear();
  _prefs.end();
  LittleFS.format();
  bleClearBonds();
  delay(300);
  ESP.restart();
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, my + 8 + i * 14);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/5", brightLevel + 1);
    } else if (i >= 1 && i <= 2) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 3) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 4) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: M5.Power.powerOff(); break;  // StickS3: M5.Power.powerOff()
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Clock — StickS3 has RTC via M5PM1, accessible through M5Unified M5.Rtc
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
static m5::rtc_datetime_t _rtcDt;   // M5Unified RTC struct
uint32_t               _clkLastRead = 0;
static bool            _onUsb       = false;

static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  // StickS3: M5.Power.isCharging() / getBatteryVoltage() replace AXP checks
  _onUsb = M5.Power.isCharging();
  M5.Rtc.getDateTime(&_rtcDt);
}

static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static uint8_t clockDow() { return _rtcDt.date.weekDay % 7; }

static void clockUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccel(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _rtcDt.time.hours, _rtcDt.time.minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _rtcDt.time.seconds);
  uint8_t mi = (_rtcDt.date.month >= 1 && _rtcDt.date.month <= 12) ? _rtcDt.date.month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _rtcDt.date.date);

  if (clockOrient == 0) {
    paintedOrient = 0;
    spr.fillRect(0, 90, W, H - 90, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 140);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 175);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 200);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  M5.Display.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Display.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  if (repaint || _rtcDt.time.seconds != lastSec) {
    lastSec = _rtcDt.time.seconds;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _rtcDt.date.date);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _rtcDt.time.seconds);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.setTextSize(3); M5.Display.setTextColor(p.text, p.bg);    M5.Display.drawString(hm, 170, 42);
    M5.Display.setTextSize(2); M5.Display.setTextColor(p.textDim, p.bg); M5.Display.drawString(ssl, 170, 72);
                                                                          M5.Display.drawString(wdl, 170, 102);
    M5.Display.setTextDatum(TL_DATUM);
    M5.Display.setTextSize(1);
  }

  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    if (buddyMode) {
      M5.Display.fillRect(0, 0, 115, 90, p.bg);
      buddyRenderTo(&M5.Display, activeState);
    }
  }
  M5.Display.setRotation(0);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;
}

// PersonaState (main-loop semantic) → CharacterState (GIF asset key).
// ASCII buddy reads activeState directly; GIF clawd has its own _state in
// character.cpp and needs this bridge — without it, shake/busy/celebrate
// never reach the GIF and clawd looks stuck in idle.
// P_HEART falls back to CHAR_HAPPY (clawd ships no heart.gif).
static CharacterState personaToChar(PersonaState p) {
  switch (p) {
    case P_SLEEP:     return CHAR_SLEEPING;
    case P_BUSY:      return CHAR_WORKING_TYPING;
    case P_ATTENTION: return CHAR_NOTIFICATION;
    case P_CELEBRATE: return CHAR_HAPPY;
    case P_DIZZY:     return CHAR_DIZZY;
    case P_HEART:     return CHAR_HAPPY;
    case P_IDLE:
    default:          return CHAR_IDLE;
  }
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccel(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 184); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("Echo: a Claude Code");
    ln("companion. Watches");
    ln("sessions, holds the");
    ln("mic, paste replies.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("A   approve prompt");
    ln("B   deny prompt");
    ln("hold A  voice input");
    ln("hold B  open menu");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("A   front");
    spr.setTextColor(p.textDim, p.bg); ln("    tap: next screen");
    ln("    tap: approve");
    ln("    hold: voice rec"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("B   right side");
    spr.setTextColor(p.textDim, p.bg); ln("    tap: next page");
    ln("    tap: deny");
    ln("    hold: open menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("PWR side power");
    spr.setTextColor(p.textDim, p.bg); ln("    tap: power off");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "LINK", infoPage);
    spr.setTextColor(p.text, p.bg);
    ln("BLE");
    spr.setTextColor(p.textDim, p.bg);
    ln("  %s", btName);
    ln("  %s", !bleConnected() ? "no client"
              : bleSecure() ? "encrypted" : "open");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    if (bleConnected()) ln("  last msg  %lus", (unsigned long)age);
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("WIFI");
    spr.setTextColor(p.textDim, p.bg);
    if (wifiState() == WIFI_CONNECTED) {
      ln("  %s", wifiSsid());
      ln("  rssi %ddBm", wifiRssi());
      ln("  ip %s", wifiIp().toString().c_str());
    } else {
      ln("  %s", wifiStateLabel());
    }
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("CLAUDE CODE");
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions %u  run %u",
       tama.sessionsTotal, tama.sessionsRunning);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    // StickS3: M5.Power replaces M5.Axp for battery info
    int vBat_mV = M5.Power.getBatteryVoltage();
    int iBat_mA = M5.Power.getBatteryCurrent();
    bool usb = M5.Power.isCharging();
    int pct = (vBat_mV - 3200) / 10;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(usb ? GREEN : p.textDim, p.bg);
    spr.setCursor(60, y + 4);
    spr.print(usb ? "charging" : "battery");
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB int", ESP.getFreeHeap() / 1024);
    ln("  psram    %uKB", ESP.getFreePsram() / 1024);
    ln("  bright   %u/5", brightLevel + 1);

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("upstream");
    y += 2;
    spr.setTextColor(p.text, p.bg);
    ln("anthropics/claude-");
    ln("desktop-buddy");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("buddy theme");
    y += 2;
    spr.setTextColor(p.text, p.bg);
    ln("clawd-tank (MIT)");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("ASR");
    y += 2;
    spr.setTextColor(p.text, p.bg);
    ln("qwen3-asr-flash");
    ln("aliyun dashscope");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 2;
    spr.setTextColor(p.text, p.bg);
    ln("M5Stack StickS3");
  }
}

static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  spr.setCursor(4, H - AREA + (toolLen <= 10 ? 14 : 18));
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(4, H - AREA + 34);
  spr.printf("%.21s", tama.promptHint);
  if (hlen > 21) {
    spr.setCursor(4, H - AREA + 42);
    spr.printf("%.21s", tama.promptHint + 21);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("A:ok  B:deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}


static void _bootTick() {
  switch (_bootStage) {
    case BOOT_WIFI:
      if (wifiState() == WIFI_CONNECTED) {
        _bootStage = BOOT_BLE;
        _bleStartedAt = millis();
      }
      break;
    case BOOT_BLE:
      if (bleConnected()) {
        _bootStage = BOOT_INIT;
      } else if (millis() - _bleStartedAt > BLE_WAIT_MS) {
        // No bridge in range (Mac asleep/closed/away) → degrade gracefully.
        _bootStage = BOOT_OFFLINE;
        _bootDoneAt = millis();
      }
      break;
    case BOOT_INIT:
      if (asrHasEndpoint()) {
        _bootStage = BOOT_DONE;
        _bootDoneAt = millis();
      }
      break;
    case BOOT_DONE:
      break;
    case BOOT_OFFLINE:
      // BLE came online later (e.g. Mac woke up) → resume online init.
      // _bootDoneAt reset so the next stable transition re-flashes banner.
      if (bleConnected()) {
        _bootStage = BOOT_INIT;
        _bootDoneAt = 0;
      }
      break;
  }
}

// Mid-lower banner: y=128-206 (78px high). Lives in the gap between
// clawd GIF (y=0-120) and HUD status strip (y=228-240) — physically can't
// collide with character render. No pushSprite here: caller controls
// when sprite hits the display so banner composites on top of clawd
// in a single batched push.
static void drawBootBanner() {
  const Palette& p = characterPalette();
  const int BX = 4, BW = W - 8;
  const int BY = 128, BH = 78;

  // Panel background + border
  spr.fillRoundRect(BX, BY, BW, BH, 3, PANEL);
  spr.drawRoundRect(BX, BY, BW, BH, 3, p.textDim);

  spr.setTextSize(1);
  bool wifiOk  = wifiState() == WIFI_CONNECTED;
  bool bleOk   = bleConnected();
  bool initOk  = asrHasEndpoint();
  bool offline = (_bootStage == BOOT_OFFLINE);
  bool done    = (_bootStage == BOOT_DONE);

  int y = BY + 5;

  // Title (left) + state hint (right)
  spr.setTextColor(done ? GREEN : (offline ? HOT : p.text), PANEL);
  spr.setCursor(BX + 4, y);
  spr.print(done ? "READY" : offline ? "OFFLINE" : "STARTING");
  y += 11;

  // WIFI line
  spr.setTextColor(wifiOk ? GREEN : p.textDim, PANEL);
  spr.setCursor(BX + 4, y);
  spr.printf("%s WIFI", wifiOk ? "[ok]" : "[..]");
  y += 11;

  // BLE line — special handling for OFFLINE
  if (offline) {
    spr.setTextColor(HOT, PANEL);
    spr.setCursor(BX + 4, y);
    spr.print("[--] BLE no host");
  } else {
    spr.setTextColor(bleOk ? GREEN : p.textDim, PANEL);
    spr.setCursor(BX + 4, y);
    spr.printf("%s BLE", bleOk ? "[ok]" : "[..]");
  }
  y += 11;

  // BRIDGE line
  spr.setTextColor(initOk ? GREEN : p.textDim, PANEL);
  spr.setCursor(BX + 4, y);
  spr.printf("%s BRIDGE", initOk ? "[ok]" : "[..]");
  y += 13;

  // Footer hint
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(BX + 4, y);
  if (done)         spr.print("bridge linked");
  else if (offline) spr.print("buddy live solo");
  else              spr.print("waiting...");
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  const int LH   = 8;
  const int AREA = 12;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);
  spr.setCursor(4, H - LH - 2);

  // Mood burst: show short ASCII label (e.g. "hmm?", "*sweep*"). The
  // burst itself is driven by characterIdleMoodTick in the main loop;
  // here we just reflect the current label.
  if (_moodLabel) {
    spr.setTextColor(p.text, p.bg);
    spr.print(_moodLabel);
    return;
  }

  // Status strip: BLE± WiFi± bat XX%
  // Plus/minus picked over filled-circle bullets because + and - are
  // unambiguous on a 6x8 ASCII font (●/○ render as identical squares).
  bool ble = bleConnected();
  bool wifi = wifiState() == WIFI_CONNECTED;
  int vBat_mV = M5.Power.getBatteryVoltage();
  int pct = (vBat_mV - 3200) / 10;
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;

  spr.setTextColor(ble ? GREEN : p.textDim, p.bg);
  spr.printf("BLE%c", ble ? '+' : '-');
  spr.setTextColor(wifi ? GREEN : p.textDim, p.bg);
  spr.printf(" WiFi%c", wifi ? '+' : '-');
  spr.setTextColor(p.text, p.bg);
  spr.printf(" bat %d%%", pct);
}

void setup() {
  // M5Unified auto-detects StickS3 hardware (BMI270, ST7789P3, ES8311, M5PM1)
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  M5.Display.setRotation(0);
  M5.Speaker.begin();
  M5.Speaker.setVolume(128);

  startBt();
  applyBrightness();
  lastInteractMs = millis();

  if (!LittleFS.begin(true)) {
    Serial.println("[fs] LittleFS mount failed, formatted");
  }

  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();
  // Phase 2: try GIF character pack first; fall back to ASCII buddy.
  bool gif_ok = characterInit("clawd");
  buddyMode    = !gif_ok;       // ASCII buddy only when GIF missing
  gifAvailable = gif_ok;
  applyDisplayMode();

  spr.createSprite(W, H);

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  wifiInit();
  serialCliInit();

  Serial.println("[buddy] StickS3 ready");
}

void loop() {
  M5.update();
  wifiTick();
  serialCliTick();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);

  // LAN ASR endpoint handshake. While BLE is up and WiFi has an IP, retry
  // hello every 2s until bridge replies with init (data.h calls
  // asrSetMacEndpoint then). On BLE drop, clear endpoint so a reconnect
  // can't use a previous Mac's stale IP.
  {
    static bool _ble_was_up = false;
    static uint32_t _last_hello_ms = 0;
    bool ble_now = bleConnected();
    if (!ble_now && _ble_was_up) asrClearMacEndpoint();
    if (ble_now && wifiState() == WIFI_CONNECTED && !asrHasEndpoint()
        && (now - _last_hello_ms > 2000)) {
      char hello[80];
      snprintf(hello, sizeof(hello),
               "{\"cmd\":\"hello\",\"my_ip\":\"%s\"}",
               wifiIp().toString().c_str());
      sendCmd(hello);
      _last_hello_ms = now;
    }
    _ble_was_up = ble_now;
  }

  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // No user LED on StickS3 — attention state shown on screen only

  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
    }
  }

  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Edge detection: when a permission prompt clears (approved/denied/timeout
  // → bridge pushes prompt={}), drawHUD's non-prompt branch only clears
  // 28px while drawApproval drew 78px → 50px of stale pixels linger.
  // Force a full redraw to wipe them.
  {
    static bool _last_in_prompt = false;
    if (_last_in_prompt && !inPrompt) characterInvalidate();
    _last_in_prompt = inPrompt;
  }

  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // M5Unified does not enable _use_pmic_button for M5PM1 (StickS3), so this
  // never fires. Screen sleep toggle via power button is disabled on StickS3.
  if (M5.BtnPWR.wasClicked()) {
    if (screenOff) {
      wake();
    } else {
      M5.Display.sleep();
      screenOff = true;
    }
  }

  // KEY1 long-press 600ms → enter voice recording mode (hold to record,
  // release to commit; KEY2 short-press while recording = cancel).
  // recorderRun() blocks until release/cancel and returns.
  // While in menu/settings/reset overlays, long-press KEY1 = close overlay.
  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen)         { resetOpen = false;    characterInvalidate(); }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else if (menuOpen)     { menuOpen = false;     characterInvalidate(); }
    else {
      // No overlay: start voice recording.
      int rc = recorderRun();
      characterInvalidate();   // wipe REC UI artifacts
      if (rc == 1) {
        sendPendingUntil = millis() + 5000;  // 5s window for Enter shortcut
      }
      // Treat the long-press release as "consumed" so the KEY1 release
      // handler below doesn't ALSO trigger a display-mode cycle.
      btnALong = true;
    }
  }
  // KEY2 long-press 600ms → open menu (replaces former KEY1 long-press behavior).
  if (M5.BtnB.pressedFor(600) && !btnBLong && !swallowBtnB) {
    btnBLong = true;
    beep(800, 60);
    if (resetOpen)         { resetOpen = false;    characterInvalidate(); }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      characterInvalidate();
    }
  }

  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else if (sendPendingUntil && (int32_t)(millis() - sendPendingUntil) < 0) {
        // Inside the post-record send-Enter window: tell bridge to press Enter.
        beep(2400, 60);
        sendCmd("{\"cmd\":\"send_enter\"}");
        sendPendingUntil = 0;  // consume
        characterInvalidate();  // clear the countdown banner from screen
      } else {
        beep(1800, 30);
        // Cycle NORMAL ↔ INFO.
        displayMode = (displayMode == DISP_NORMAL) ? DISP_INFO : DISP_NORMAL;
        applyDisplayMode();
        characterInvalidate();   // wipe stale clawd when switching pages
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  if (M5.BtnB.wasReleased()) {
    btnBLong = false;   // clear long-press latch on release
  }

  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  clockRefreshRtc();
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;

  // Idle mood randomizer: tick once per loop, store result for drawHUD.
  // Skip during anything that hides or interrupts the buddy area.
  bool _moodSkip = napping || screenOff || inPrompt
                || menuOpen || settingsOpen || resetOpen
                || displayMode != DISP_NORMAL || clocking;
  _moodLabel = characterIdleMoodTick(_moodSkip);
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);
    uint8_t h = _rtcDt.time.hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  // Boot banner: shown in mid-lower region (y=128-206) during all unstable
  // stages (WIFI/BLE/INIT) plus a brief 5s flash after reaching stable state
  // (DONE = bridge linked, OFFLINE = bridge timed out). The banner is an
  // OVERLAY — character render is no longer blocked by boot. Clawd lives
  // y=0-120, banner lives y=128-206; physically can't collide.
  _bootTick();
  bool _stable = (_bootStage == BOOT_DONE || _bootStage == BOOT_OFFLINE);
  bool _showBanner = !_stable
        || (_bootDoneAt && (int32_t)(millis() - _bootDoneAt - BANNER_HOLD_MS) < 0);

  // Banner edge-off: when banner stops showing, force a full redraw so the
  // panel pixels don't linger on screen.
  {
    static bool _last_show_banner = false;
    if (_last_show_banner && !_showBanner) {
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
    _last_show_banner = _showBanner;
  }

  if (napping || screenOff || landscapeClock) {
    // skip character render — screen's off or clock has the screen.
  } else {
    // Overlay open/close paths set characterInvalidate(); wipe sprite first.
    if (characterTakeDirty()) {
      spr.fillSprite(0x0000);
      buddyInvalidate();
    }
    // Suspend character animation while ANY info-style screen is up.
    // Includes:
    //   * Modal overlays: menu / settings / reset
    //   * Display modes that overlap the buddy area: INFO (help/about/buttons),
    //     PET (pet name editor / stats), HUD-only.
    // This prevents stale clawd pixels showing through the regions a
    // sub-fullscreen layout doesn't cover.
    bool overlay = menuOpen || settingsOpen || resetOpen
                   || displayMode == DISP_INFO;
    if (characterLoaded() && !overlay) {
      // Bridge activeState → clawd _state. Only on transition: every-frame
      // set would clobber the idle mood randomizer's 5s bursts (it also
      // calls characterSetState while burst is active).
      static PersonaState _lastPersona = (PersonaState)0xFF;
      if (activeState != _lastPersona) {
        characterSetState(personaToChar(activeState));
        _lastPersona = activeState;
      }
      characterTick();
      characterRenderTo(&spr, 0, 0);
    } else if (buddyMode && !overlay) {
      buddyTick(activeState);
    }
  }

  if (landscapeClock) {
    drawClock();
  } else if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();

    // Boot banner overlay (y=128-206). Suppress when any UI shares that
    // region: prompt (y=162+), info (y=70+), clock (y=90+), modal overlays
    // (centered), or pairing passkey (full-screen takeover).
    bool _showBootBanner = _showBanner && !blePasskey() && !inPrompt
        && !menuOpen && !settingsOpen && !resetOpen
        && displayMode == DISP_NORMAL && !clocking;
    if (_showBootBanner) drawBootBanner();

    // Post-recording send-Enter window: 5-second countdown banner so the
    // user knows a short KEY1 tap will press Enter on the desktop.
    // Suppressed on overlays/info/clock so it doesn't fight other UI.
    bool show_banner = sendPendingUntil
        && (int32_t)(millis() - sendPendingUntil) < 0
        && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
        && displayMode == DISP_NORMAL && !blePasskey() && !clocking;
    if (show_banner) {
      uint32_t left_s = (sendPendingUntil - millis() + 999) / 1000;
      spr.fillRoundRect(2, 130, W - 4, 14, 3, PANEL);
      spr.setTextSize(1);
      spr.setTextColor(GREEN, PANEL);
      spr.setCursor(6, 133);
      // ↩  U+21A9 = UTF-8 E2 86 A9
      spr.printf("\xe2\x86\xa9 A to send  %us", (unsigned)left_s);
    }
    // Banner edge-off: trigger one full redraw so the stale banner pixels
    // don't linger on screen (happens on natural 5s expiry; KEY1-consume
    // case is handled inline in the BtnA release handler above).
    static bool _last_banner = false;
    if (_last_banner && !show_banner) characterInvalidate();
    _last_banner = show_banner;

    spr.pushSprite(0, 0);
  }

  // Face-down nap
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)  { if (faceDownFrames < 20) faceDownFrames++; }
    else       { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5.Display.setBrightness(10);  // dim: replaces M5.Axp.ScreenBreath(8)
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    M5.Display.sleep();
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
