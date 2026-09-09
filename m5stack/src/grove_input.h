#pragma once

#include <cstdint>

// Local controllers: the Joystick / Joystick2 Unit and the Dual Button Unit on
// any Grove port, plus the Faces 3 Gamepad Panel on the internal I2C bus (see
// faces_input.h). All of them are polled by one task on core 0, mirroring the
// UDP receiver, so I2C latency stays out of the frame loop.

// Probe every local controller, configure the Grove pins and start the polling
// task. Call once from setup(), after M5.begin().
void groveInputInit();

// Current pad-1 bits from all local controllers, OR'd together (NES_BTN_*
// layout). Written by the polling task, safe to call from the emulation loop
// every frame.
uint8_t groveInputBits();
