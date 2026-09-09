#pragma once

#include <cstdint>

// M5Stack Faces 3 Bottom Board + Gamepad Panel v3.0。8 ボタンが NES の
// パッドとそのまま 1:1 で対応する唯一の入力なので、Grove の
// ジョイスティック + Dual Button (方向 + A/B しか無い) より上位の体験になる。
//
// パネルの STM32 は M-Bus 経由で CoreS3 の内部 I2C (SDA=12 / SCL=11) の 0x08 に
// 載る。head_touch (Si12T @0x68) と同じバスで、Grove の外部 I2C を張り替える
// grove_input とは競合しない。パネルが無ければ検出に失敗して以降無効になるだけ。

// 0x08 をプローブし、Gamepad3 (MODEL_ID=0x03) なら初期化する。setup() から
// 一度だけ、groveInputInit() より前に呼ぶ。
// 戻り値はパネルを検出できたかどうか (呼び出し側は無視してよい)。
bool facesInputInit();

// Faces パネルが検出済みかどうか。ポーリングを回す価値があるかの判定用。
bool facesInputPresent();

// パネルを 1 回読み、内部の押下状態を更新する。I2C を触るので core 0 の
// Grove タスクから呼ぶこと (フレームループから呼んではいけない)。
void facesInputPoll();

// 直近のポーリングで押されていたボタン (NES_BTN_* レイアウト)。
// facesInputPoll() と別スレッドから読んでも安全。
uint8_t facesInputBits();
