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

static constexpr int ZONE_COUNT = 3;

static bool g_present = false;
static uint32_t g_lastPollMs = 0;
// 前回ポーリング時に各ゾーンが触れられていたか。立ち上がりエッジ (新しく
// 触れられたゾーン) の検出に使う。
static bool g_prevZone[ZONE_COUNT] = {};
// 指が最後に「新しく」触れたゾーン。距離の起点で、-1 は未接触または
// 掌などで複数ゾーンが同時に立って起点を決められなかった状態。
static int g_lastZone = -1;
static uint32_t g_lastMoveMs = 0;
// 撫でた距離の累積 (ゾーン境界のまたぎ回数)。BSP の「3 ゾーン全部が時間窓内に
// 順に着火したらスワイプ」判定は使っていない: あちらは 1 ストロークを 1 回と
// 数える形で、指を離さず往復する撫で方を検出できないため。隣のゾーンへ移る
// たびに 1 加算し、HEAD_TOUCH_TRAVEL_TO_MENU に達したら発火する。
static int g_travel = 0;

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

bool headTouchSwiped() {
    if (!g_present) return false;

    // レートリミット。フレーム予算 16.6ms に対し 100kHz で 1 バイト読むだけでも
    // ~0.3ms かかるので、指がゾーンをまたぐ動き (最速でも数十 ms) を取れる
    // 最低限の分解能まで落とす。
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

    // 動きが途絶えたら数え直す。指が完全に離れているかどうかは見ない:
    // 乗せたまま止めても、離して間を置いても、等しく「撫で終わり」。
    const bool idle = g_travel > 0 && now - g_lastMoveMs > HEAD_TOUCH_TRAVEL_RESET_MS;
    if (idle) {
        g_travel = 0;
        g_lastZone = -1;
    }

    // このポーリングで新しく触れられたゾーン (立ち上がりエッジ) を拾う。
    int rose = -1;
    int roseCount = 0;
    for (int i = 0; i < ZONE_COUNT; i++) {
        const bool isRising = zone[i] && !g_prevZone[i];
        if (isRising) {
            rose = i;
            roseCount++;
        }
        g_prevZone[i] = zone[i];
    }
    if (roseCount == 0) return false;

    // 同時に複数ゾーンが立つのは指の移動ではなく掌などの面接触。距離には
    // 数えず、起点も無効化する — 直後の release→rise を移動と誤認しないため。
    if (roseCount > 1) {
        g_lastZone = -1;
        g_lastMoveMs = now;
        return false;
    }

    // 隣のゾーンへ移った時だけ距離が伸びる。離れたゾーンへの跳び (一度離して
    // 別の場所に置き直した) は移動ではないので、起点を置き直すだけにする。
    const bool moved = g_lastZone >= 0 && (rose - g_lastZone == 1 || g_lastZone - rose == 1);
    if (moved) g_travel++;
    g_lastZone = rose;
    g_lastMoveMs = now;
    if (!moved || g_travel < HEAD_TOUCH_TRAVEL_TO_MENU) {
        if (moved) Serial.printf("HEAD: travel %d/%d\n", g_travel, HEAD_TOUCH_TRAVEL_TO_MENU);
        return false;
    }

    g_travel = 0;
    g_lastZone = -1;
    Serial.println("HEAD: pet -> menu");
    return true;
}
