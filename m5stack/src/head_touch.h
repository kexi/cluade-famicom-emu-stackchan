#pragma once

// M5Stack 公式 StackChan の頭頂部タッチセンサー (Si12T)。頭を前後に撫でると
// ゲーム中でも ROM 選択メニューに戻れる。
//
// センサーは StackChan ボディが M-Bus 経由で CoreS3 の内部 I2C
// (SDA=12 / SCL=11) に載せているので、Grove の外部 I2C を使う grove_input とは
// 競合しない。ボディが無い個体では初期化に失敗し、以降は何もしない。

// 0x68 をプローブし、居れば初期化する。setup() から一度だけ呼ぶ。
// 戻り値はセンサーを検出できたかどうか (呼び出し側は無視してよい)。
bool headTouchInit();

// 撫でジェスチャーを 1 回検出したフレームだけ true。Game モードの loop から
// 毎フレーム呼ぶ想定で、内部でポーリング周期を絞っている。
// 前→後 / 後→前 のどちらの向きでも true を返す。
bool headTouchSwiped();
