#pragma once
#include <stdint.h>

// Hold-to-record voice capture: blocks while KEY1 (BtnA) is held, streams
// audio to Dashscope Qwen3-ASR-Flash-Realtime over WSS, sends final text
// to the Mac bridge over BLE NUS as {"cmd":"asr","text":"..."}.
//
// Cancel: short-press KEY2 (BtnB) while recording.
//
// Returns:
//   1 = text sent successfully (caller may show send-enter hint)
//   0 = cancelled / no text captured
//  -1 = error (no wifi / no key / asr fail)
int recorderRun();

// Latest transcribed text (UTF-8). Empty until recorderRun() succeeds.
const char* recorderLastText();
