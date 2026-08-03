#pragma once

#include <cstdint>

#include "config.h"

// Boot-time ROM picker, and where a BtnC hold returns to during play.
//
// Draws with ordinary M5.Display primitives, which is only safe because no band
// DMA is outstanding while the menu is up — the caller guarantees that by going
// through stopVideoAudio() before menuEnter(). Everything here runs on core 1
// for the same reason the SD layer does: it reads the card.

struct MenuResult {
    enum class Action : uint8_t {
        None = 0,   // still browsing
        LaunchEmbedded = 1,   // the ROM built into the firmware
        LaunchSd = 2,   // sdName below
    };
    Action action;
    char sdName[SD_ROM_NAME_MAX];
};

// Rescan the card and repaint the whole screen. Call once on entry; the tick
// below only repaints what changed.
void menuEnter();

// One frame of the menu. `navBits` is the merged pad state in the NES_BTN_*
// layout — edges and auto-repeat are derived here, so the caller passes the raw
// level and does not have to track them.
MenuResult menuTick(uint8_t navBits);

// Replace the status line with a message (an SdStatus text, typically). Sticks
// until the next navigation redraws it, so a failed load is still readable when
// the user looks up.
void menuShowError(const char* text);
