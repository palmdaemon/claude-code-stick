#include "character.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <AnimatedGIF.h>
#include <esp_heap_caps.h>

// ── State name table (must mirror enum order in character.h) ───────────
static const char* STATE_NAMES[CHAR_STATE_COUNT] = {
  "idle", "sleeping", "notification", "happy", "disconnected",
  "going_away", "dizzy",
  "working_typing", "working_thinking", "working_debugger",
  "working_building", "working_conducting", "working_wizard",
  "working_beacon", "working_sweeping", "working_juggling",
  "working_overheated", "working_confused", "static_base",
};

const char* characterStateName(CharacterState s) {
  if (s >= CHAR_STATE_COUNT) return "?";
  return STATE_NAMES[s];
}

// ── Module state ───────────────────────────────────────────────────────
static AnimatedGIF    _gif;
static bool           _gif_open    = false;
static bool           _loaded      = false;
static char           _theme[24]   = "";
static CharacterState _state       = CHAR_IDLE;
static CharacterState _requested   = CHAR_IDLE;
static uint32_t       _next_frame_at = 0;

// Each state's GIF is loaded into a PSRAM buffer and played via
// AnimatedGIF::open(buffer, size, drawCb) — file-callback path
// (LittleFS read/seek) was unreliable and only ever decoded frame 0.
static uint8_t*       _gif_buf     = nullptr;
static size_t         _gif_buf_len = 0;

// Render target — set per-call before AnimatedGIF invokes the draw callback.
static LovyanGFX* _tgt   = nullptr;
static int        _tgt_x = 0;
static int        _tgt_y = 0;

// ── Per-row draw callback ─────────────────────────────────────────────
// Known-working baseline: write every pixel (transparent index → bg color).
// Includes per-frame diagnostic to see what AnimatedGIF actually hands us.
static constexpr uint16_t GIF_BG = 0x0000;
static void gif_draw_cb(GIFDRAW* d) {
  if (!_tgt) return;
  // Diagnostic: log first row of each frame so we can see if library is
  // advancing internally or stuck on frame 0. _last_first_byte sticks
  // across calls; same_as_prev=1 N times ⇒ library frame pointer stuck.
  // (verbose per-frame diagnostic removed; root cause was AnimatedGIF +
  // LittleFS file-callback incompatibility; PSRAM buffer path works.)
  int y      = _tgt_y + d->iY + d->y;
  int x_base = _tgt_x + d->iX;
  int width  = d->iWidth;
  if (width > 135) width = 135;
  uint16_t* pal = d->pPalette;
  uint8_t*  src = d->pPixels;
  uint16_t  row[135];

  if (d->ucHasTransparency) {
    uint8_t trans = d->ucTransparent;
    for (int x = 0; x < width; x++) {
      row[x] = (src[x] == trans) ? GIF_BG : pal[src[x]];
    }
  } else {
    for (int x = 0; x < width; x++) row[x] = pal[src[x]];
  }
  _tgt->pushImage(x_base, y, width, 1, row);
}

// ── Asset opening ─────────────────────────────────────────────────────
static String pathForState(CharacterState s) {
  String p = "/characters/";
  p += _theme;
  p += "/";
  p += STATE_NAMES[s];
  p += ".gif";
  return p;
}

static bool openStateGif(CharacterState s) {
  if (_gif_open) {
    _gif.close();
    _gif_open = false;
  }
  // Free old in-memory buffer.
  if (_gif_buf) {
    free(_gif_buf);
    _gif_buf = nullptr;
    _gif_buf_len = 0;
  }

  String path = pathForState(s);
  if (!LittleFS.exists(path)) {
    if (s != CHAR_IDLE && LittleFS.exists(pathForState(CHAR_IDLE))) {
      path = pathForState(CHAR_IDLE);
      s = CHAR_IDLE;
    } else {
      Serial.printf("[char] asset missing: %s\n", path.c_str());
      return false;
    }
  }

  // Load entire GIF into PSRAM to bypass LittleFS file callbacks (which
  // are the prime suspect for frames-not-advancing).
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("[char] open fail: %s\n", path.c_str());
    return false;
  }
  _gif_buf_len = f.size();
  _gif_buf = (uint8_t*)heap_caps_malloc(_gif_buf_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!_gif_buf) {
    Serial.printf("[char] psram alloc fail (%u bytes)\n", (unsigned)_gif_buf_len);
    f.close();
    return false;
  }
  size_t n = f.read(_gif_buf, _gif_buf_len);
  f.close();
  if (n != _gif_buf_len) {
    Serial.printf("[char] short read %u/%u\n", (unsigned)n, (unsigned)_gif_buf_len);
    free(_gif_buf); _gif_buf = nullptr; _gif_buf_len = 0;
    return false;
  }
  Serial.printf("[char] loaded %s into PSRAM (%u bytes)\n", path.c_str(), (unsigned)_gif_buf_len);

  if (!_gif.open(_gif_buf, (int)_gif_buf_len, gif_draw_cb)) {
    Serial.printf("[char] openFLASH fail: %s\n", path.c_str());
    free(_gif_buf); _gif_buf = nullptr; _gif_buf_len = 0;
    return false;
  }
  _gif_open = true;
  _state    = s;
  _next_frame_at = millis();
  // No characterInvalidate() here — that would set the dirty flag, causing
  // main.cpp's characterTakeDirty() to wipe the sprite on the NEXT frame.
  // While the new GIF waits for _next_frame_at to advance (idle.gif: 260ms),
  // characterRenderTo returns without drawing → pushSprite ships pure black
  // = visible "flash" on switch. Callers needing a wipe-on-switch (e.g.
  // PersonaState transitions in main.cpp) should fillSprite themselves
  // around the characterSetState call — that wipe + first-frame render
  // happen in the same frame and there's no flicker.
  return true;
}

// ── Public API ─────────────────────────────────────────────────────────
bool characterInit(const char* theme) {
  if (!theme || !*theme) theme = "clawd";
  if (!LittleFS.begin(true)) {
    Serial.println("[char] LittleFS mount failed");
    return false;
  }
  strncpy(_theme, theme, sizeof(_theme) - 1);
  _theme[sizeof(_theme) - 1] = '\0';

  // BIG_ENDIAN_PIXELS — LovyanGFX/M5GFX on ST7789 expects RGB565 in
  // network byte order over SPI; LITTLE_ENDIAN_PIXELS yields swapped
  // colors (orange→cyan-blue).
  _gif.begin(BIG_ENDIAN_PIXELS);

  // Verify at least one expected asset is present.
  String idle_path = pathForState(CHAR_IDLE);
  if (!LittleFS.exists(idle_path)) {
    Serial.printf("[char] theme '%s' missing %s\n", _theme, idle_path.c_str());
    return false;
  }
  _loaded = true;
  Serial.printf("[char] theme=%s ready\n", _theme);
  _state = CHAR_IDLE;
  _requested = CHAR_IDLE;
  return openStateGif(CHAR_IDLE);
}

bool characterLoaded() { return _loaded; }

void characterSetState(CharacterState state) {
  if (state >= CHAR_STATE_COUNT) return;
  if (state == _state) return;
  _requested = state;
}

CharacterState characterCurrentState() { return _state; }

void characterTick() {
  if (!_loaded) return;
  // Service state change: switch GIF at frame boundary.
  if (_requested != _state) {
    openStateGif(_requested);
  }
}

// ── Idle mood randomizer ─────────────────────────────────────────────
//
// Idle is the default state when nothing's happening. To keep clawd from
// looking frozen, we briefly switch to a random non-working state for 5s
// every 8-15s (random jitter), then revert. Skipped when the caller says
// the screen is off / inPrompt / recording etc — no point burning a GIF
// load when nothing's visible.

struct _Mood { CharacterState state; const char* label; };
static const _Mood _MOODS[] = {
  { CHAR_DIZZY,            "whoa..." },
  { CHAR_WORKING_CONFUSED, "hmm?"    },
  { CHAR_WORKING_SWEEPING, "*sweep*" },
  { CHAR_WORKING_JUGGLING, "*juggle*"},
  { CHAR_STATIC_BASE,      "..."     },
  { CHAR_GOING_AWAY,       "brb"     },
};
static const uint8_t _MOOD_COUNT = sizeof(_MOODS) / sizeof(_Mood);

static constexpr uint32_t MOOD_DURATION_MS    = 5000;
static constexpr uint32_t MOOD_MIN_INTERVAL_MS = 8000;
static constexpr uint32_t MOOD_MAX_INTERVAL_MS = 15000;

static uint32_t _nextMoodAt   = 0;     // when to trigger next mood burst
static uint32_t _moodEndsAt   = 0;     // when current burst ends; 0 if none
static int8_t   _activeMoodIdx = -1;   // index into _MOODS, -1 if none

static void _scheduleNextMood(uint32_t now) {
  uint32_t span = MOOD_MAX_INTERVAL_MS - MOOD_MIN_INTERVAL_MS;
  _nextMoodAt = now + MOOD_MIN_INTERVAL_MS + (esp_random() % (span + 1));
}

const char* characterIdleMoodTick(bool skip) {
  if (!_loaded) return nullptr;
  uint32_t now = millis();

  // Caller says don't burst (screen off, prompt up, recording, etc).
  // Cancel any active burst and rearm scheduler for later.
  if (skip) {
    if (_activeMoodIdx >= 0) {
      characterSetState(CHAR_IDLE);
      _activeMoodIdx = -1;
      _moodEndsAt = 0;
    }
    _nextMoodAt = 0;
    return nullptr;
  }

  // Burst-in-progress: check expiry.
  if (_activeMoodIdx >= 0) {
    if ((int32_t)(now - _moodEndsAt) >= 0) {
      characterSetState(CHAR_IDLE);
      _activeMoodIdx = -1;
      _moodEndsAt = 0;
      _scheduleNextMood(now);
      return nullptr;
    }
    return _MOODS[_activeMoodIdx].label;
  }

  // No burst, current state non-idle → tool/emotion is running, reset.
  if (_state != CHAR_IDLE) {
    _nextMoodAt = 0;
    return nullptr;
  }

  // In idle, no burst. Arm timer on first entry; check fire.
  if (_nextMoodAt == 0) {
    _scheduleNextMood(now);
    return nullptr;
  }
  if ((int32_t)(now - _nextMoodAt) >= 0) {
    _activeMoodIdx = (int8_t)(esp_random() % _MOOD_COUNT);
    _moodEndsAt = now + MOOD_DURATION_MS;
    characterSetState(_MOODS[_activeMoodIdx].state);
    return _MOODS[_activeMoodIdx].label;
  }
  return nullptr;
}

void characterRenderTo(LovyanGFX* tgt, int x, int y) {
  if (!_loaded || !_gif_open || !tgt) return;
  uint32_t now = millis();
  if ((int32_t)(now - _next_frame_at) < 0) return;  // wait until frame time

  // No explicit fillRect: the draw callback writes every pixel for each
  // row (transparent index → GIF_BG), so the GIF region is fully painted
  // on every frame. An earlier fillRect-then-draw approach caused visible
  // flashing because between the wipe and the per-line draws, the sprite
  // briefly contained a black rectangle that bled into pushSprite output.

  _tgt   = tgt;
  _tgt_x = x;
  _tgt_y = y;

  int wait_ms = 0;
  int rc = _gif.playFrame(false, &wait_ms);
  _tgt = nullptr;

  if (rc < 1) {
    // End of GIF (or error) — restart.
    _gif.reset();
    _next_frame_at = now;
    return;
  }
  if (wait_ms <= 0) wait_ms = 30;
  _next_frame_at = now + (uint32_t)wait_ms;
}

void characterClose() {
  if (_gif_open) {
    _gif.close();
    _gif_open = false;
  }
  _loaded = false;
}

void characterSetPeek(bool) {}
