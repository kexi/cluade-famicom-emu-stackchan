#pragma once

#include <cstdint>

// USB_PAD_DEBUG が下の宣言を出し分けるため。grove_input.h と違って定数表に
// 依存するのは、デバッグ用アクセサの有無を config.h 側の 1 スイッチで決めたい
// から (include 順に頼るとインクルードする側の書き順で壊れる)。
#include "config.h"

// USB-C 直結の Nintendo Switch Pro Controller。CoreS3 の USB-C ポートを ESP-IDF
// の usb_host スタックでホストモードに切り替え、有線プロコンをパッド 1 の入力
// ソースとして読む。Grove ユニットや UDP パッドとは OR 合成される。
//
// ポーリングは core 0 のタスクが行い、フレームループは atomic を 1 バイト読む
// だけ — Grove と同じ構成で、USB の列挙やハンドシェイクの待ち時間がエミュ
// レーションに乗らないようにしている。

// USB ホストスタックを立ち上げ、常駐タスクを起動する。setup() から一度だけ
// 呼ぶ。
//
// 重要: ESP32-S3 の USB PHY は 1 つしかないので、この呼び出し以降 USB
// Serial/JTAG は使えなくなる — PC へのシリアルログは出ず、pio の自動書き込みも
// 効かない。書き込むときはプロコンを抜いてリセットボタン長押しで DL モードへ。
// そのため setup() の最後、起動ログを全部吐き終えてから呼ぶ。
void usbPadInit();

// 現在のプロコンのボタン (NES_BTN_* レイアウト)。ポーリングタスクが書き、
// フレームループから毎フレーム読んで構わない。未接続や切断中は 0。
uint8_t usbPadBits();

#if USB_PAD_DEBUG
// デバッグオーバーレイ用の状態文字列 ("WAIT" / "ENUM" / "HS" / "RUN")。状態機械
// が持つ enum を外に晒さないための、表示専用の軽量アクセサ。
const char* usbPadDebugStatus();
#endif
