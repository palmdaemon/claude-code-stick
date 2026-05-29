#pragma once
// Phase 2: GIF character packs. Assets live on LittleFS under
//   /characters/<theme>/<state>.gif
// Each state corresponds to a Claude Code activity (idle, working_typing,
// notification, error, etc.). theme is "clawd" for now.
#include <stdint.h>
#include <M5Unified.h>

struct Palette {
  uint16_t body, bg, text, textDim, ink;
};

inline const Palette& characterPalette() {
  static const Palette _p = {
    0x2945,  // body:    teal accent (unused once GIF replaces ASCII)
    0x0000,  // bg:      black (matches transparent-bg GIF compositing)
    0xFFFF,  // text:    white
    0x8410,  // textDim: dim gray
    0x0000,  // ink:     black
  };
  return _p;
}

// ── State catalog ──────────────────────────────────────────────
// Names match asset filenames under /characters/<theme>/<name>.gif.
// Keep CHAR_STATE_COUNT in sync with the table in character.cpp.
enum CharacterState : uint8_t {
  CHAR_IDLE = 0,
  CHAR_SLEEPING,
  CHAR_NOTIFICATION,
  CHAR_HAPPY,
  CHAR_DISCONNECTED,
  CHAR_GOING_AWAY,
  CHAR_DIZZY,
  CHAR_WORKING_TYPING,
  CHAR_WORKING_THINKING,
  CHAR_WORKING_DEBUGGER,
  CHAR_WORKING_BUILDING,
  CHAR_WORKING_CONDUCTING,
  CHAR_WORKING_WIZARD,
  CHAR_WORKING_BEACON,
  CHAR_WORKING_SWEEPING,
  CHAR_WORKING_JUGGLING,
  CHAR_WORKING_OVERHEATED,
  CHAR_WORKING_CONFUSED,
  CHAR_STATIC_BASE,
  CHAR_STATE_COUNT
};

// theme name like "clawd". Returns true if at least one asset loaded.
bool        characterInit(const char* theme);
bool        characterLoaded();
void        characterSetState(CharacterState state);
const char* characterStateName(CharacterState state);
CharacterState characterCurrentState();

// Advance GIF animation if the frame's display time has elapsed.
void characterTick();

// Idle "mood burst": while clawd is in plain idle, every 8-15s briefly
// (5s) switch to a random non-working state so the buddy doesn't feel
// frozen. Returns a short ASCII label while a burst is active (e.g.
// "hmm?"), nullptr otherwise — caller can use it in the HUD line.
// Pass skip=true to suppress (e.g. screenOff/inPrompt/napping/recording).
const char* characterIdleMoodTick(bool skip);

// Composite the current frame onto a LovyanGFX target at (x, y). The
// canvas is 135×120 by convention (preserves transparency through alpha
// blending — caller's background shows through).
void characterRenderTo(LovyanGFX* tgt, int x, int y);

void characterClose();
void characterSetPeek(bool peek);  // reserved

// Full-screen invalidation flag — overlays (menu / settings) cover area
// outside the character's clear strip; closing them needs a full sprite
// wipe + redraw on the next frame. Function-static storage to avoid
// C++17 inline-variable requirement.
inline bool& _characterDirtyRef() { static bool flag = false; return flag; }
inline void characterInvalidate() { _characterDirtyRef() = true; }
inline bool characterTakeDirty()  {
  bool& f = _characterDirtyRef();
  bool d = f;
  f = false;
  return d;
}

// Phase 3 placeholders (over-the-air theme transfer).
inline bool xferActive()      { return false; }
inline uint32_t xferProgress(){ return 0; }
inline uint32_t xferTotal()   { return 0; }
