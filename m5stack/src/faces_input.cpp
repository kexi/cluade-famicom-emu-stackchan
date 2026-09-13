// Faces 3 Gamepad Panel v3.0 のドライバ。
//
// レジスタマップ・ビット定義・極性・後述の 0x00 対策は M5Stack 公式ライブラリ
// (https://github.com/m5stack/M5Faces, MIT License, src/M5Faces_Gamepad3.{hpp,cpp}
//  と src/M5FacesBase.hpp) から移植した。
// Copyright (c) M5Stack Technology CO LTD — SPDX-License-Identifier: MIT
//
// 本家の M5Faces_Gamepad3 クラスは使わず必要な部分だけ関数に落としてある。
// head_touch と同じ理由で、lib_deps に本家を足すと IAP (ファームウェア更新)
// 用の firmware イメージ 3 種が丸ごとフラッシュに乗ってしまい、ここで要るのは
// 「今押されている 8bit」だけだから。

#include <M5Unified.h>
#include <atomic>

#include "config.h"
#include "faces_input.h"

// パネルのビット位置 (0..7 昇順で UP/DOWN/LEFT/RIGHT/A/B/SELECT/START) から
// NES のビットへ。NES_BTN_* とは並びが違うのでシフトでは移せず、この表を引く。
static constexpr uint8_t FACES_TO_NES[8] = {
    NES_BTN_UP, NES_BTN_DOWN, NES_BTN_LEFT, NES_BTN_RIGHT, NES_BTN_A, NES_BTN_B, NES_BTN_SELECT, NES_BTN_START,
};

static std::atomic<uint8_t> g_facesBits{0};
static bool g_present = false;

uint8_t facesInputBits() { return g_facesBits.load(std::memory_order_relaxed); }

bool facesInputPresent() { return g_present; }

// パネルの押下バイトを NES のパッドビットへ。active-low なので、まず反転して
// 「押されている = 1」にしてから並べ替える。
static uint8_t toNesBits(uint8_t raw) {
    const uint8_t pressed = (uint8_t)~raw;
    uint8_t bits = 0;
    for (int i = 0; i < 8; i++) {
        if (pressed & (1 << i)) bits |= FACES_TO_NES[i];
    }
    return bits;
}

bool facesInputInit() {
    // MODEL_ID まで読んで型番を確かめる。0x08 は Faces3 ベースの共通アドレスで、
    // キーボードや電卓パネルも同じ所に居る。それらのキーコードを押下ビットとして
    // 解釈すると出鱈目な方向入力になるので、Gamepad 以外は掴まない。
    uint8_t model = 0;
    const bool ok = M5.In_I2C.readRegister(FACES_I2C_ADDR, FACES_REG_MODEL_ID, &model, 1, FACES_I2C_FREQ);
    if (!ok) {
        Serial.println("FACES: no gamepad panel");
        return false;
    }
    if (model != FACES_MODEL_GAMEPAD3) {
        Serial.printf("FACES: panel @0x%02X is model 0x%02X, not gamepad — ignored\n", FACES_I2C_ADDR, model);
        return false;
    }

    g_present = true;
    Serial.println("FACES: gamepad panel v3.0");
    return true;
}

void facesInputPoll() {
    if (!g_present) return;

    uint8_t raw = 0;
    if (!M5.In_I2C.readRegister(FACES_I2C_ADDR, FACES_REG_KEY, &raw, 1, FACES_I2C_FREQ)) {
        // 読み損ねは 1 回ならバスのグリッチとして無視し、直前の状態を維持する。
        // grove_input の joyFails と同じ判断で、押しっぱなしの方向が 1 ポーリング
        // だけ抜けると走行中のキャラが引っかかるため。パネルは M-Bus 直結で
        // 抜き差しされないので、Grove のような「見失ったら探索に戻る」経路は要らない。
        return;
    }

    // INT ピンを使わない盲ポーリングでは、パネルの V03/V04 ファームが送信
    // バッファに残った 0x00 をそのまま返すことがある (本家 M5Faces_Gamepad3
    // の update() が同じ値を弾いている)。active-low で 0x00 は「8 ボタン同時
    // 押し」を意味してしまい、そのまま通すと全方向 + A + B + START が一斉に
    // 立つので、新しいサンプルではないものとして捨てる。
    if (raw == 0x00) return;

    g_facesBits.store(toNesBits(raw), std::memory_order_relaxed);
}
