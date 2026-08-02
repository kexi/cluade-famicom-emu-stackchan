// StackChan 頭頂部タッチセンサー (Si12T) のドライバとスワイプ判定。
//
// レジスタマップ・初期化列・スワイプの時間窓は M5Stack 公式 BSP
// (https://github.com/m5stack/StackChan-BSP, MIT License,
//  src/drivers/Si12T/ と src/utils/touch_sensor/) から移植した。
// Copyright (c) M5Stack Technology CO LTD — SPDX-License-Identifier: MIT
//
// 本家の Si12T クラス (m5::I2C_Device 派生) と TouchSensor_Class は使わず、
// 必要な部分だけをフラットな関数に落としてある。理由は 2 つ:
// BSP 全体を lib_deps に足すと LovyanGFX の別バージョンやサーボ依存まで
// 引き込むこと、そして本家 update() が毎回 3 ゾーンぶんの状態と millis() の
// 生ログを持つのに対し、ここで要るのは「撫でられたか」の 1bit だけであること。

#include <M5Unified.h>

#include "config.h"
#include "head_touch.h"

// Si12T レジスタマップ (BSP の Si12T.h より)。7bit アドレスは SEL=GND の 0x68。
static constexpr uint8_t SI12T_ADDR = 0x68;
static constexpr uint8_t SI12T_REG_SENSITIVITY1 = 0x02;   // 0x02-0x07 が ch 感度
static constexpr uint8_t SI12T_REG_CTRL1 = 0x08;
static constexpr uint8_t SI12T_REG_CTRL2 = 0x09;
static constexpr uint8_t SI12T_REG_REF_RST1 = 0x0A;   // 0x0A-0x0F が ch 有効化
static constexpr uint8_t SI12T_REG_OUTPUT1 = 0x10;   // 3 ゾーンぶんが 2bit ずつ

// 感度設定値。BSP が TouchSensor_Class::begin() で使う
// (SI12T_Type_High, SI12T_Sensitivity_Level_4) の組がこの 0xCC に当たる。
// High タイプは上位ニブルが 0x8 + レベル、Low タイプは 0x0 + レベル、
// それを 2ch ぶん同じ値で 1 バイトに詰める仕様なので 0xC が 2 つ並ぶ。
static constexpr uint8_t SI12T_SENSITIVITY_VALUE = 0xCC;
// CTRL1: Auto モード / FTC=01 / 割り込みは Middle+High / レスポンス 4。
static constexpr uint8_t SI12T_CTRL1_VALUE = 0x22;
// CTRL2: S/W リセットを一度立ててから通常動作 + スリープ有効に落とす。
static constexpr uint8_t SI12T_CTRL2_RESET = 0x0F;
static constexpr uint8_t SI12T_CTRL2_RUN = 0x07;

// 内部 I2C はスピーカーの AW88298 と同居する。m5::I2C_Class はトランザクション
// 内でバスロックを取るので、素の Wire ではなくこの API 経由で読むこと。
static constexpr uint32_t SI12T_I2C_FREQ = 100000;

// スワイプの時間窓 (BSP の touch_sensor.cpp と同値)。ゾーン間の着火時刻差が
// この範囲に収まっていれば「なぞった」とみなす。下限はノイズ (3 ゾーンを同時に
// 掌で覆っただけ) を弾き、上限は指を置いたまま考えていた場合を弾く。
static constexpr int32_t SWIPE_MIN_INTERVAL_MS = 30;
static constexpr int32_t SWIPE_MAX_INTERVAL_MS = 400;

static constexpr int ZONE_COUNT = 3;

static bool g_present = false;
// 各ゾーンが今回の接触サイクルで一度でも触れられたか、とその最初の時刻。
// 全ゾーンが離れた時点でリセットされる。
static bool g_touched[ZONE_COUNT] = {};
static uint32_t g_touchStartMs[ZONE_COUNT] = {};
// 1 回の接触サイクルで 1 度だけ発火させるためのラッチ。BSP の _in_gesture と
// 同じ役割で、撫でた指がまだ乗っている間の再発火を止める。
static bool g_inGesture = false;
static uint32_t g_lastPollMs = 0;
// なぞり回数のカウント。HEAD_TOUCH_STROKES_TO_MENU 回目で発火し、前回のなぞり
// から HEAD_TOUCH_STROKE_PAIR_MS 空いたら 1 回目から数え直す。
static int g_strokeCount = 0;
static uint32_t g_lastStrokeMs = 0;

static bool writeReg(uint8_t reg, uint8_t value) {
    return M5.In_I2C.writeRegister8(SI12T_ADDR, reg, value, SI12T_I2C_FREQ);
}

// アドレスプローブ: ACK だけを見る空の START/STOP。grove_input の probeAddr と
// 同じ手口で、レジスタを読むより先にセンサーの有無を確定させる。
static bool probeSensor() {
    const bool acked = M5.In_I2C.start(SI12T_ADDR, false, SI12T_I2C_FREQ);
    M5.In_I2C.stop();
    return acked;
}

// BSP の Si12T::begin() 相当。本家はチャネル有効化・CTRL・感度を書いたあと
// 同じレジスタを読み返しているが、戻り値をどこにも使っていない純粋なデバッグ
// 読みなので省いた。起動時の I2C トランザクションが 6 回ぶん減る。
static void configureSensor() {
    // ch 1-9 の有効化と基準値キャリブレーション。REF_RST / CH_HOLD / CAL_HOLD の
    // 6 レジスタが 0x0A から連続していて、いずれも 0 が「有効」を意味する。
    for (uint8_t reg = SI12T_REG_REF_RST1; reg <= SI12T_REG_REF_RST1 + 5; reg++) writeReg(reg, 0x00);

    // CTRL2 が先。CTRL1 の設定が S/W リセットで流れないようにする (BSP と同順)。
    writeReg(SI12T_REG_CTRL2, SI12T_CTRL2_RESET);
    writeReg(SI12T_REG_CTRL2, SI12T_CTRL2_RUN);
    writeReg(SI12T_REG_CTRL1, SI12T_CTRL1_VALUE);

    for (uint8_t i = 0; i < 5; i++) writeReg(SI12T_REG_SENSITIVITY1 + i, SI12T_SENSITIVITY_VALUE);
}

bool headTouchInit() {
    g_present = probeSensor();
    if (!g_present) {
        Serial.println("HEAD: not found");
        return false;
    }
    configureSensor();
    Serial.println("HEAD: si12t detected");
    return true;
}

// 1 ゾーンぶんの接触強度を 3 ゾーンぶんのフラグに落とす。
//
// OUTPUT1 の 1 バイトに 3 ゾーンが 2bit ずつ (NONE/LOW/MID/HIGH) 入っている。
// OUTPUT2/3 は残りのチャネル用で StackChan の頭には繋がっていないので読まない
// — 1 ポーリングあたりの I2C を 1 レジスタ読みに抑えるのが狙い。
//
// ビット順は BSP の parse_touch_result() と逆にしてある。本家は
// point_type[0] が最下位ビットで、それを update() 側で
// `_intensities[2 - i]` と反転して前→後に並べ直していた。ここでは 2 段構えに
// する意味がないので、この場で index 0 = 前 になるよう詰める。
static void parseZones(uint8_t raw, bool out[ZONE_COUNT]) {
    for (int i = 0; i < ZONE_COUNT; i++) {
        const uint8_t intensity = (raw >> (2 * (ZONE_COUNT - 1 - i))) & 0x03;
        out[i] = intensity > 0;
    }
}

// 3 ゾーンの着火順を見て、順方向・逆方向どちらかのスワイプなら true。
static bool detectSwipe() {
    const bool allZonesTouched = g_touched[0] && g_touched[1] && g_touched[2];
    if (!allZonesTouched) return false;

    // 符号付きで引く: millis() の巻き戻りは 49 日先なので実害はないが、
    // 逆方向の差分を負の値として素直に扱いたい。
    const int32_t front = (int32_t)g_touchStartMs[0];
    const int32_t middle = (int32_t)g_touchStartMs[1];
    const int32_t back = (int32_t)g_touchStartMs[2];

    // 前→後 (0→1→2) と後→前 (2→1→0)。中央を挟んで両区間とも窓に入って
    // いることを要求するので、両端を同時に触っただけでは成立しない。
    const int32_t forwardA = middle - front, forwardB = back - middle;
    const int32_t backwardA = middle - back, backwardB = front - middle;

    const bool inWindowForward = forwardA > SWIPE_MIN_INTERVAL_MS && forwardB > SWIPE_MIN_INTERVAL_MS &&
                                 forwardA < SWIPE_MAX_INTERVAL_MS && forwardB < SWIPE_MAX_INTERVAL_MS;
    const bool inWindowBackward = backwardA > SWIPE_MIN_INTERVAL_MS && backwardB > SWIPE_MIN_INTERVAL_MS &&
                                  backwardA < SWIPE_MAX_INTERVAL_MS && backwardB < SWIPE_MAX_INTERVAL_MS;
    // 向きは問わない: どちら向きに撫でてもメニューに戻る、という 1 つの操作。
    return inWindowForward || inWindowBackward;
}

bool headTouchSwiped() {
    if (!g_present) return false;

    // レートリミット。フレーム予算 16.6ms に対し 100kHz で 1 バイト読むだけでも
    // ~0.3ms かかるので、スワイプ判定に必要な最低限の分解能まで落とす。
    // SWIPE_MIN_INTERVAL_MS より短くしておけば、区間の下限判定は鈍らない。
    const uint32_t now = millis();
    const bool tooSoon = now - g_lastPollMs < HEAD_TOUCH_POLL_MS;
    if (tooSoon) return false;
    g_lastPollMs = now;

    uint8_t raw = 0;
    const bool ok = M5.In_I2C.readRegister(SI12T_ADDR, SI12T_REG_OUTPUT1, &raw, 1, SI12T_I2C_FREQ);
    // 読めなければ今回は何も起きなかったことにする。grove_input と違って
    // 抜き差しを想定しない (M-Bus 直結なので途中で消えるのは異常) から、
    // 失敗回数を数えて無効化するところまではしない。
    if (!ok) return false;

    bool zone[ZONE_COUNT];
    parseZones(raw, zone);

    bool anyTouched = false;
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (!zone[i]) continue;
        anyTouched = true;
        const bool firstContact = !g_touched[i];
        if (firstContact) {
            g_touched[i] = true;
            g_touchStartMs[i] = now;
        }
    }

    // 全ゾーンが離れたら次のジェスチャーに備えて畳む。ここでしかフラグを
    // 落とさないので、指が乗っている限り着火時刻は保持される。
    if (!anyTouched) {
        g_inGesture = false;
        for (int i = 0; i < ZONE_COUNT; i++) g_touched[i] = false;
        return false;
    }

    // 判定済みの接触サイクル。指が乗ったままでも再発火させない。
    if (g_inGesture) return false;

    const bool swiped = detectSwipe();
    if (!swiped) return false;
    g_inGesture = true;

    // 1 回のスワイプでは開かない: 窓の内に規定回数なぞられて初めて発火する。
    // 窓を過ぎた古いなぞりは数え直し — 「2 回目が遅すぎた」は誤爆ではなく
    // 単発のなぞりが 2 つあっただけ、と解釈する。
    const bool expired = g_strokeCount > 0 && now - g_lastStrokeMs > HEAD_TOUCH_STROKE_PAIR_MS;
    if (expired) g_strokeCount = 0;
    g_strokeCount++;
    g_lastStrokeMs = now;
    if (g_strokeCount < HEAD_TOUCH_STROKES_TO_MENU) {
        Serial.printf("HEAD: stroke %d/%d\n", g_strokeCount, HEAD_TOUCH_STROKES_TO_MENU);
        return false;
    }
    g_strokeCount = 0;
    Serial.println("HEAD: stroke -> menu");
    return true;
}
