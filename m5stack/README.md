# M5Stack CoreS3 NES エミュレータ

`core/` の NES コアを M5Stack CoreS3 (ESP32-S3) 上で動かす PlatformIO プロジェクト。
コントローラ入力は Mac 側から UDP で送る。

## セットアップ

`pio` は `nix develop` または direnv 経由で入る前提。

```sh
cd m5stack
./scripts/fetch_rom.sh              # data/game.nes を取得
cp src/secrets.h.example src/secrets.h
$EDITOR src/secrets.h               # WIFI_SSID / WIFI_PASS を設定
pio run -t upload
```

`src/secrets.h` は `.gitignore` 済み。ROM (`*.nes`) もコミットされない。

## 起動シーケンス

1. `Connecting to <SSID>...` を表示して WiFi 接続
2. 接続できたら自 IP を約 2 秒表示 (この IP を Mac 側ツールに渡す)
3. 埋め込み ROM をロードしてゲーム開始

WiFi に失敗してもゲームは起動する (入力なし、画面下部に `WiFi: failed` を表示)。
ROM のロードに失敗した場合はエラー表示のまま停止する。

## Mac 側ツール

```sh
uv run tools/procon_udp.py --host <CoreS3 の IP>
```

Switch Pro Controller の入力を UDP で送信する。
`uv` は `nix develop` で入り、依存 (pygame) はスクリプト内の
PEP 723 メタデータから uv が解決する。pip install は不要。

## UDP プロトコル

ポート 5555 / 8 バイト固定長。

| offset | 内容 |
| --- | --- |
| 0-1 | magic `'N'`, `'P'` |
| 2 | version = 1 |
| 3 | reserved |
| 4-5 | seq (uint16 LE, 現状未使用) |
| 6 | pad1 ボタンビット |
| 7 | pad2 ボタンビット |

ボタンビット: `bit0:A 1:B 2:Select 3:Start 4:Up 5:Down 6:Left 7:Right`

magic / version が一致しないパケットは破棄する。
500 ms 以上パケットが途絶えると両パッドを離した状態に戻す。

## タスク構成

- **Core 1** (Arduino `loop()`) — エミュレーション + 音声 + 描画
- **Core 0** (`udp` タスク) — `recvfrom` でブロックし、ボタン状態の更新のみ行う

## 既知の制約

- 表示は約 30fps 程度に留まる見込み (エミュレーション自体は 60fps を目指してペーシング)
- WiFi の SSID / パスワードはコンパイル時に埋め込む (実行時設定 UI はなし)
- 描画は 256x240 等倍・x=32 で中央配置 (パネルは 320x240)
- `pushImageDMA` の転送中もエミュレータがフレームバッファを更新するため、
  テアリングが発生しうる (内部 SRAM にダブルバッファを置く余裕がないため許容)
- RGB565 のバイト順は実機で確認するまで不定。`src/config.h` の
  `DISPLAY_SWAP_BYTES` で切り替えられる

## 設定

`src/config.h` に UDP ポート、音量、`PERF_LOG`、バイトスワップなどの定数がある。
`PERF_LOG` を有効にすると 1 秒ごとに fps と emulation / push の平均 µs をシリアルに出力する。
