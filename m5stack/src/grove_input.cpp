// Grove controller input: Joystick / Joystick2 Unit (Port A, I2C) and the
// Dual Button Unit (Port B, GPIO).
//
// Runs as a small task on core 0, like the UDP receiver: the frame loop on
// core 1 only ever reads one atomic byte, so a slow or absent I2C device can
// never stall emulation.

#include <M5Unified.h>
#include <atomic>

#include "config.h"
#include "grove_input.h"

// Joystick2 register map (m5stack/M5Unit-JoyStick2).
static constexpr uint8_t JOY2_REG_OFFSET_8BIT = 0x60;   // int8 x, int8 y (centred on 0)
static constexpr uint8_t JOY2_REG_BUTTON = 0x20;   // 0 = pressed

enum class JoyKind : uint8_t { None, Joy1, Joy2 };

static std::atomic<uint8_t> g_groveBits{0};
static JoyKind g_joyKind = JoyKind::None;

uint8_t groveInputBits() { return g_groveBits.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------- joystick

// Address probe: a bare START/STOP that only checks for an ACK.
static bool probeAddr(uint8_t addr) {
    const bool acked = M5.Ex_I2C.start(addr, false, GROVE_I2C_FREQ);
    M5.Ex_I2C.stop();
    return acked;
}

static JoyKind probeJoystick() {
    if (probeAddr(JOY2_I2C_ADDR)) return JoyKind::Joy2;
    if (probeAddr(JOY1_I2C_ADDR)) return JoyKind::Joy1;
    return JoyKind::None;
}

// Map a centred stick position to D-pad bits. `x`/`y` are signed with 0 at
// rest; positive x = right, positive y = up (after the invert flags).
static uint8_t directionBits(int x, int y) {
    if (JOY_INVERT_X) x = -x;
    if (JOY_INVERT_Y) y = -y;
    uint8_t bits = 0;
    if (x > JOY_DEADZONE) bits |= NES_BTN_RIGHT;
    else if (x < -JOY_DEADZONE) bits |= NES_BTN_LEFT;
    if (y > JOY_DEADZONE) bits |= NES_BTN_UP;
    else if (y < -JOY_DEADZONE) bits |= NES_BTN_DOWN;
    return bits;
}

// Joystick (U024): a raw 3-byte read — x, y (0..255, centre ~128), button.
static bool readJoy1(uint8_t* bits) {
    uint8_t raw[3];
    const bool ok = M5.Ex_I2C.start(JOY1_I2C_ADDR, true, GROVE_I2C_FREQ) && M5.Ex_I2C.read(raw, sizeof(raw));
    M5.Ex_I2C.stop();
    if (!ok) return false;
    *bits = directionBits((int)raw[0] - 128, (int)raw[1] - 128);
    const bool pressed = JOY1_BTN_ACTIVE_HIGH ? raw[2] != 0 : raw[2] == 0;
    if (pressed) *bits |= NES_BTN_START;
    return true;
}

// Joystick2 (U024-C): registered access; the 8-bit offset registers are already
// calibrated around 0 by the unit's firmware.
static bool readJoy2(uint8_t* bits) {
    uint8_t off[2], btn;
    const bool ok = M5.Ex_I2C.readRegister(JOY2_I2C_ADDR, JOY2_REG_OFFSET_8BIT, off, sizeof(off), GROVE_I2C_FREQ) &&
                    M5.Ex_I2C.readRegister(JOY2_I2C_ADDR, JOY2_REG_BUTTON, &btn, 1, GROVE_I2C_FREQ);
    if (!ok) return false;
    *bits = directionBits((int8_t)off[0], (int8_t)off[1]);
    if (btn == 0) *bits |= NES_BTN_START;
    return true;
}

// ------------------------------------------------------------------- task

static void groveTask(void*) {
    uint32_t lastProbeMs = 0;
    uint8_t joyBits = 0;
    int joyFails = 0;
    for (;;) {
        uint8_t bits = 0;

        // Dual Button Unit: signal lines are shorted to GND while pressed.
        if (digitalRead(DUAL_BTN_PIN_RED) == LOW) bits |= NES_BTN_A;
        if (digitalRead(DUAL_BTN_PIN_BLUE) == LOW) bits |= NES_BTN_B;

        if (g_joyKind == JoyKind::None) {
            // Nothing answered at boot (or it was unplugged): re-probe about
            // once a second so plugging the stick in later just works.
            const uint32_t now = millis();
            if (now - lastProbeMs >= JOY_REPROBE_MS) {
                lastProbeMs = now;
                g_joyKind = probeJoystick();
                if (g_joyKind != JoyKind::None) {
                    joyFails = 0;
                    Serial.printf("GROVE: joystick%s detected\n", g_joyKind == JoyKind::Joy2 ? "2 @0x63" : " @0x52");
                }
            }
        } else {
            uint8_t fresh = 0;
            const bool ok = g_joyKind == JoyKind::Joy2 ? readJoy2(&fresh) : readJoy1(&fresh);
            if (ok) {
                joyFails = 0;
                joyBits = fresh;
            } else if (++joyFails >= JOY_READ_FAIL_LIMIT) {
                // Actually unplugged: drop back to probing rather than keep
                // hammering a dead address, and release any held direction.
                g_joyKind = JoyKind::None;
                joyBits = 0;
                Serial.println("GROVE: joystick lost");
            }
            // Below the limit the last good state stands: a single failed read
            // is a bus glitch, and releasing a held direction for one poll
            // would stutter the character mid-run.
            bits |= joyBits;
        }

        g_groveBits.store(bits, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(GROVE_POLL_MS));
    }
}

void groveInputInit() {
    // INPUT_PULLUP as a safety net: the unit has its own pull-ups, but this way
    // an empty PORT.C just reads "released" instead of floating.
    pinMode(DUAL_BTN_PIN_BLUE, INPUT_PULLUP);
    pinMode(DUAL_BTN_PIN_RED, INPUT_PULLUP);

    // Bind the external I2C bus to PORT.B's pins instead of PORT.A's, keeping
    // the port number M5Unified reserved for it (I2C_NUM_0 on the CoreS3 — the
    // internal bus holds the other controller).
    M5.Ex_I2C.begin(M5.Ex_I2C.getPort(), JOY_I2C_SDA, JOY_I2C_SCL);

    g_joyKind = probeJoystick();
    switch (g_joyKind) {
    case JoyKind::Joy2: Serial.println("GROVE: joystick2 @0x63"); break;
    case JoyKind::Joy1: Serial.println("GROVE: joystick @0x52"); break;
    case JoyKind::None: Serial.println("GROVE: no joystick (will re-probe)"); break;
    }

    // Same core as the UDP receiver, below its priority: input polling must
    // never delay packet reception, and both stay off the emulation core.
    xTaskCreatePinnedToCore(groveTask, "grove", 3072, nullptr, 3, nullptr, 0);
}
