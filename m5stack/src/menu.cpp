#include "menu.h"

#include <M5Unified.h>

#include <cstdio>
#include <cstring>

#include "sd_rom.h"

// Row 0 is always the embedded image, so the menu is never empty and a card
// that fails to mount still offers something to play. SD entries follow.
static SdRomEntry g_entries[SD_ROM_MAX_FILES];
static int g_sdCount = 0;
static int g_cursor = 0;
static int g_scrollTop = 0;
// Held-direction repeat state. Tracked here rather than by the caller so the
// caller can keep passing the raw pad level, which is all it has.
static uint8_t g_prevNav = 0;
static uint32_t g_repeatAtMs = 0;
static uint8_t g_repeatBit = 0;
// Set when the whole list needs repainting rather than just the two rows the
// cursor moved between. Full repaints are ~7 filled rectangles plus text, i.e.
// tens of milliseconds on a blocking push, so they are worth avoiding on every
// cursor step.
static bool g_needFullDraw = true;
static char g_status[48] = {};

static int rowCount() { return g_sdCount + 1; }

// ------------------------------------------------------------------ drawing

static void drawStatus() {
    M5.Display.fillRect(0, 224, M5.Display.width(), 16, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(4, 228);
    M5.Display.print(g_status);
}

static void drawRow(int index) {
    const int slot = index - g_scrollTop;
    const bool offscreen = slot < 0 || slot >= MENU_VISIBLE_ROWS;
    if (offscreen) return;

    const int y = MENU_TOP_Y + slot * MENU_ROW_H;
    const bool selected = index == g_cursor;
    const uint16_t bg = selected ? TFT_NAVY : TFT_BLACK;
    M5.Display.fillRect(0, y, M5.Display.width(), MENU_ROW_H, bg);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(selected ? TFT_WHITE : TFT_LIGHTGREY, bg);
    M5.Display.setCursor(6, y + 4);

    const bool embeddedRow = index == 0;
    if (embeddedRow) {
        M5.Display.print("[Built-in]");
        return;
    }
    // Truncated to what fits at text size 2 (12px advance on a 320px panel,
    // less the left margin and the size column) rather than letting the driver
    // run the name off the edge and into the next row's cell.
    char line[26];
    snprintf(line, sizeof(line), "%s", g_entries[index - 1].name);
    M5.Display.print(line);

    const uint32_t kb = (g_entries[index - 1].size + 1023) / 1024;
    char sizeText[12];
    snprintf(sizeText, sizeof(sizeText), "%luK", (unsigned long)kb);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(M5.Display.width() - 40, y + 8);
    M5.Display.print(sizeText);
}

static void drawAll() {
    M5.Display.fillScreen(TFT_BLACK);

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    M5.Display.setCursor(4, 4);
    M5.Display.print("SELECT ROM");

    // Capacity on the title line, right-aligned. Shown even at 0/0 (no card):
    // "SD --" next to a list with only the built-in row is what tells the user
    // the card is the reason, rather than leaving them to guess.
    uint64_t total = 0, freeBytes = 0;
    sdRomSpace(&total, &freeBytes);
    char cap[32];
    if (total > 0) {
        snprintf(cap, sizeof(cap), "SD %lluM/%lluM", (unsigned long long)(freeBytes / (1024 * 1024)),
                 (unsigned long long)(total / (1024 * 1024)));
    } else {
        snprintf(cap, sizeof(cap), "SD: not found");
    }
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(sdRomMounted() ? TFT_CYAN : TFT_ORANGE, TFT_BLACK);
    M5.Display.setCursor(M5.Display.width() - 6 * (int)strlen(cap) - 4, 8);
    M5.Display.print(cap);

    for (int i = 0; i < rowCount(); i++) drawRow(i);
    drawStatus();
    g_needFullDraw = false;
}

// ------------------------------------------------------------------- input

// Move the cursor by one row and repaint only what changed.
//
// Scrolling forces the full repaint because every visible row's content shifts;
// a plain step within the window only dirties two rows, which is the case worth
// optimising since it is what holding a direction does dozens of times.
static void moveCursor(int delta) {
    const int rows = rowCount();
    const int previous = g_cursor;
    g_cursor += delta;
    // Wrap rather than clamp: a 60-entry card is otherwise a long hold to get
    // from the last row back to the built-in one.
    if (g_cursor < 0) g_cursor = rows - 1;
    if (g_cursor >= rows) g_cursor = 0;
    if (g_cursor == previous) return;

    const bool above = g_cursor < g_scrollTop;
    const bool below = g_cursor >= g_scrollTop + MENU_VISIBLE_ROWS;
    if (above || below) {
        g_scrollTop = g_cursor - (below ? MENU_VISIBLE_ROWS - 1 : 0);
        if (g_scrollTop < 0) g_scrollTop = 0;
        g_needFullDraw = true;
        return;
    }
    drawRow(previous);
    drawRow(g_cursor);
}

// Reduce a held direction to a stream of steps: one on the press edge, then
// repeats after MENU_REPEAT_DELAY_MS. Returns the bit that should act this
// tick, or 0.
static uint8_t navStep(uint8_t navBits) {
    const uint8_t directions = navBits & (NES_BTN_UP | NES_BTN_DOWN);
    const uint8_t pressed = (uint8_t)(directions & ~g_prevNav);
    g_prevNav = directions;

    const uint32_t now = millis();
    if (pressed) {
        g_repeatBit = pressed;
        g_repeatAtMs = now + MENU_REPEAT_DELAY_MS;
        return pressed;
    }
    // Both directions at once, or the held one released: drop the repeat rather
    // than firing it for a key that is no longer down.
    const bool stillHeld = g_repeatBit != 0 && (directions & g_repeatBit) != 0;
    if (!stillHeld) {
        g_repeatBit = 0;
        return 0;
    }
    const bool due = (int32_t)(now - g_repeatAtMs) >= 0;
    if (!due) return 0;
    g_repeatAtMs = now + MENU_REPEAT_MS;
    return g_repeatBit;
}

// A tap on a list row selects it. Returns the row, or -1.
//
// Only the release edge counts, and only inside the list band: the CoreS3's
// BtnA/BtnB/BtnC are themselves touch zones under the panel, so reacting to a
// press anywhere would make the footer's own buttons select rows.
static int touchedRow() {
    const bool noTouch = M5.Touch.getCount() == 0;
    if (noTouch) return -1;
    const auto& t = M5.Touch.getDetail();
    if (!t.wasClicked()) return -1;

    const int listBottom = MENU_TOP_Y + MENU_VISIBLE_ROWS * MENU_ROW_H;
    const bool outsideList = t.y < MENU_TOP_Y || t.y >= listBottom;
    if (outsideList) return -1;

    const int slot = (t.y - MENU_TOP_Y) / MENU_ROW_H;
    const int row = g_scrollTop + slot;
    const bool pastEnd = row >= rowCount();
    if (pastEnd) return -1;
    return row;
}

// ------------------------------------------------------------------- public

void menuEnter() {
    // Rescanned on every entry rather than cached from boot: the card may have
    // gained files over UDP, or been swapped, since the last time the menu was
    // up. The scan is the one place this costs anything and it happens once.
    g_sdCount = sdRomScan(g_entries, SD_ROM_MAX_FILES);

    // The cursor is not carried across entries. Coming back to the menu from a
    // game means the previous choice is the one thing the user is least likely
    // to want, and preserving an index into a list that may have been rescanned
    // into a different order would point at an arbitrary row anyway.
    g_cursor = 0;
    g_scrollTop = 0;
    // Cleared so a direction still held from the game that just exited does not
    // count as a fresh press and step the cursor on the first tick.
    g_prevNav = 0;
    g_repeatBit = 0;
    snprintf(g_status, sizeof(g_status), "A/START: run   BtnC(hold): resume");
    drawAll();
}

void menuShowError(const char* text) {
    snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
    drawStatus();
}

MenuResult menuTick(uint8_t navBits) {
    MenuResult result = {};
    result.action = MenuResult::Action::None;
    result.sdName[0] = '\0';

    if (g_needFullDraw) drawAll();

    const uint8_t step = navStep(navBits);
    if (step & NES_BTN_UP) moveCursor(-1);
    if (step & NES_BTN_DOWN) moveCursor(1);
    if (g_needFullDraw) drawAll();

    // Decide on the press edge of A or START. Level-triggered would launch the
    // game and then hand it the same still-held button on its first frame.
    static uint8_t prevConfirm = 0;
    const uint8_t confirmNow = navBits & (NES_BTN_A | NES_BTN_START);
    const bool confirmEdge = confirmNow != 0 && prevConfirm == 0;
    prevConfirm = confirmNow;

    int chosen = -1;
    if (confirmEdge) chosen = g_cursor;
    const int tapped = touchedRow();
    if (tapped >= 0) {
        // A tap both moves the cursor and confirms, so the row the user hit is
        // visibly the one that launches.
        const int previous = g_cursor;
        g_cursor = tapped;
        drawRow(previous);
        drawRow(g_cursor);
        chosen = tapped;
    }
    if (chosen < 0) return result;

    const bool embedded = chosen == 0;
    if (embedded) {
        result.action = MenuResult::Action::LaunchEmbedded;
        return result;
    }
    result.action = MenuResult::Action::LaunchSd;
    snprintf(result.sdName, sizeof(result.sdName), "%s", g_entries[chosen - 1].name);
    return result;
}
