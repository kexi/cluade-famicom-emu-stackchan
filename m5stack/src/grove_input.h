#pragma once

#include <cstdint>

// Local controllers on the Grove ports: Joystick / Joystick2 Unit on Port A
// (I2C) and the Dual Button Unit on Port B (GPIO). Polled by a task on core 0,
// mirroring the UDP receiver, so I2C latency stays out of the frame loop.

// Probe the units, configure the Port B pins and start the polling task.
// Call once from setup(), after M5.begin().
void groveInputInit();

// Current pad-1 bits from the Grove controllers (NES_BTN_* layout). Written by
// the polling task, safe to call from the emulation loop every frame.
uint8_t groveInputBits();
