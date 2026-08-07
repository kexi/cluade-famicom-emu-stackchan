// Grove controller input: Joystick / Joystick2 Unit and the Dual Button Unit,
// each pluggable into ANY of the K151 base's three Grove ports.
//
// The joystick is hunted for by re-binding the external I2C bus to one port's
// pin pair at a time; every port not holding the joystick is treated as a Dual
// Button port and read as GPIO. Runs as a small task on core 0, like the UDP
// receiver: the frame loop on core 1 only ever reads one atomic byte, so a
// slow or absent I2C device can never stall emulation.

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
// Which port the joystick answered on, or -1 while searching. The I2C bus is
// bound to this port's pins exactly while >= 0.
static int g_joyPort = -1;

uint8_t groveInputBits() { return g_groveBits.load(std::memory_order_relaxed); }

// ------------------------------------------------------------------- ports

// Return a port's pins to plain inputs. INPUT_PULLUP as a safety net: the
// button unit has its own pull-ups, but this way an empty port just reads
// "released" instead of floating — and a port that briefly served as the I2C
// probe target goes back to being a valid button port.
static void portToGpio(int port) {
    pinMode(GROVE_PORT_PIN1[port], INPUT_PULLUP);
    pinMode(GROVE_PORT_PIN2[port], INPUT_PULLUP);
}

// Bind the external I2C bus to a port's pins (pin1=SDA, pin2=SCL — the same
// positions PORT.A uses natively). The port number stays the one M5Unified
// reserved for Ex_I2C (I2C_NUM_0 on the CoreS3; the internal bus holds the
// other controller).
static void bindI2C(int port) {
    M5.Ex_I2C.release();
    M5.Ex_I2C.begin(M5.Ex_I2C.getPort(), GROVE_PORT_PIN1[port], GROVE_PORT_PIN2[port]);
}

// ---------------------------------------------------------------- joystick

// Presence probe. A bare START/STOP "ACK check" is not enough here: on a port
// with nothing attached it reports success (observed on hardware as the
// joystick being "detected" on every empty port in turn), so the probe demands
// a full, verified data read instead.
static JoyKind probeJoystick() {
    uint8_t tmp;
    if (M5.Ex_I2C.readRegister(JOY2_I2C_ADDR, JOY2_REG_BUTTON, &tmp, 1, GROVE_I2C_FREQ)) return JoyKind::Joy2;
    uint8_t raw[3];
    const bool ok = M5.Ex_I2C.start(JOY1_I2C_ADDR, true, GROVE_I2C_FREQ) && M5.Ex_I2C.read(raw, sizeof(raw));
    M5.Ex_I2C.stop();
    if (ok) return JoyKind::Joy1;
    return JoyKind::None;
}

// Try one port for a joystick. On a hit the bus stays bound there; on a miss
// the pins go straight back to button duty.
static bool probePort(int port) {
    bindI2C(port);
    g_joyKind = probeJoystick();
    const bool found = g_joyKind != JoyKind::None;
    if (found) {
        g_joyPort = port;
        Serial.printf("GROVE: joystick%s on PORT.%c\n", g_joyKind == JoyKind::Joy2 ? "2 @0x63" : " @0x52",
                      GROVE_PORT_NAME[port]);
    } else {
        M5.Ex_I2C.release();
        portToGpio(port);
    }
    return found;
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
    int probeNext = 0;
    uint8_t joyBits = 0;
    int joyFails = 0;
    for (;;) {
        uint8_t bits = 0;

        // Dual Button Unit: every port the joystick is not on. A held button
        // shorts its line to GND. With no joystick found yet all ports are
        // read — a plugged-but-undetected joystick just idles both lines high,
        // so it cannot fake a press.
        for (int port = 0; port < GROVE_PORT_COUNT; port++) {
            if (port == g_joyPort) continue;
            if (digitalRead(GROVE_PORT_PIN1[port]) == LOW) bits |= NES_BTN_A;   // red
            if (digitalRead(GROVE_PORT_PIN2[port]) == LOW) bits |= NES_BTN_B;   // blue
        }

        if (g_joyPort < 0) {
            // Searching: try the next port about once a second, so plugging
            // the stick into any connector at any time just works. The probe
            // is transient — a miss returns the pins to button duty above.
            const uint32_t now = millis();
            if (now - lastProbeMs >= JOY_REPROBE_MS) {
                lastProbeMs = now;
                if (probePort(probeNext)) joyFails = 0;
                probeNext = (probeNext + 1) % GROVE_PORT_COUNT;
            }
        } else {
            uint8_t fresh = 0;
            const bool ok = g_joyKind == JoyKind::Joy2 ? readJoy2(&fresh) : readJoy1(&fresh);
            if (ok) {
                joyFails = 0;
                joyBits = fresh;
            } else if (++joyFails >= JOY_READ_FAIL_LIMIT) {
                // Actually unplugged: free the port and resume the search
                // rather than keep hammering a dead address, and release any
                // held direction.
                M5.Ex_I2C.release();
                portToGpio(g_joyPort);
                g_joyPort = -1;
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
    for (int port = 0; port < GROVE_PORT_COUNT; port++) portToGpio(port);

    // One full sweep up front so a stick plugged in before boot is live from
    // the first frame; the task keeps sweeping for anything plugged in later.
    bool found = false;
    for (int port = 0; port < GROVE_PORT_COUNT && !found; port++) found = probePort(port);
    if (!found) Serial.println("GROVE: no joystick (searching)");

    // Same core as the UDP receiver, below its priority: input polling must
    // never delay packet reception, and both stay off the emulation core.
    xTaskCreatePinnedToCore(groveTask, "grove", 3072, nullptr, 3, nullptr, 0);
}
