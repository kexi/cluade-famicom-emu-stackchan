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
// パネルが最後に返したバイト。KEY レジスタはイベントが無いあいだ「最後に
// 送ったバイト」をそのまま返し続ける (実測: 型番を読んだ直後は 0x03、I2C
// アドレスを読んだ直後は 0x08 が KEY として返る)。パネルはキーの変化ごとに
// 1 フレームだけ出すので、前回と同じバイトは新しい情報を持たない。
static uint8_t g_lastByte = 0xFF;

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

    // ファーム版を記録 (0xFE)。残骸の出方が版で違う可能性があるので、報告に
    // 添えられるようログに残す。読めなくても動作には関係ない。
    uint8_t fw = 0;
    const bool fwOk = M5.In_I2C.readRegister(FACES_I2C_ADDR, FACES_REG_FW_VERSION, &fw, 1, FACES_I2C_FREQ);

    // 送信バッファの残骸を取り込んでおく。KEY はイベントが無いあいだ「最後に
    // 送ったバイト」を返し続けるので (facesInputPoll() 参照)、直前に読んだ
    // レジスタの値がそのまま返ってくる。以前はこれをボタン状態として
    // 通していたため、0x03 (型番) が「LEFT+RIGHT+A+B+SELECT+START 同時押し」
    // に化けて起動直後のメニューで先頭 ROM が勝手に選ばれていた。ここで 1 回
    // 読んで g_lastByte に据えれば、以降の同じ値はポーリング側で捨てられる。
    uint8_t stale = 0;
    if (M5.In_I2C.readRegister(FACES_I2C_ADDR, FACES_REG_KEY, &stale, 1, FACES_I2C_FREQ)) g_lastByte = stale;

    g_present = true;
    Serial.printf("FACES: gamepad panel v3.0 fw=%s%02X\n", fwOk ? "" : "?", fw);
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

    // KEY はレベルではなくイベント。パネルはボタン状態が変わるたびに「その時点の
    // 全ボタン状態」を 1 フレーム出し、次の変化までは送信バッファに残った
    // 最後のバイトを返し続ける (実測、fw 03: 型番を読んだ直後は 0x03 が KEY と
    // して返る)。前回と同じバイトは新しい情報を持たないので捨てる。押しっぱなし
    // は、押した瞬間のフレームで立てたビットが離すフレーム (0xFF) まで残ること
    // で表現されるので、ここで捨てても押下が抜けることはない。
    if (raw == g_lastByte) return;
    g_lastByte = raw;

    // INT ピンを使わない盲ポーリングでは、パネルの V03/V04 ファームが送信
    // バッファに残った 0x00 をそのまま返すことがある (本家 M5Faces_Gamepad3
    // の update() が同じ値を弾いている)。active-low で 0x00 は「8 ボタン同時
    // 押し」を意味してしまい、そのまま通すと全方向 + A + B + START が一斉に
    // 立つので、新しいサンプルではないものとして捨てる。
    if (raw == 0x00) return;

    g_facesBits.store(toNesBits(raw), std::memory_order_relaxed);
}
