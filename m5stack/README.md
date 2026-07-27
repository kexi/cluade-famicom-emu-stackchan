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

ポート 5555。オフセット 3 が **type** で、パケット種別を表す。
旧クライアントはここを `reserved = 0` として送っていたため、
type 0 (パッド) は従来のパケットとバイト単位で互換。

### type 0: パッド (8 バイト)

| offset | 内容 |
| --- | --- |
| 0-1 | magic `'N'`, `'P'` |
| 2 | version = 1 |
| 3 | type = 0 |
| 4-5 | seq (uint16 LE, 現状未使用) |
| 6 | pad1 ボタンビット |
| 7 | pad2 ボタンビット |

ボタンビット: `bit0:A 1:B 2:Select 3:Start 4:Up 5:Down 6:Left 7:Right`

500 ms 以上パケットが途絶えると両パッドを離した状態に戻す。

### type 1: カセット端子状態 (14 バイト)

| offset | 内容 |
| --- | --- |
| 0-1 | magic `'N'`, `'P'` |
| 2 | version = 1 |
| 3 | type = 1 |
| 4-5 | seq (uint16 LE) |
| 6-13 | pin mask (uint64 LE) |

mask の **bit(n-1) = ピン n が導通**。60 本すべて 1 なら正常に挿さった状態。
変化したときだけコアに適用し、そのつどシリアルへ `PINS: %016llx` を出力する。

**端子状態はパッドと違いタイムアウトしない。** 接触不良は物理的な状態であり、
送信側が止まっても勝手に直るものではないため。復帰はブラウザの「まっすぐ挿す」
(= 全ビット 1 のマスク送信) で明示的に行う。

### type 2: コンソール制御 (8 バイト)

| offset | 内容 |
| --- | --- |
| 0-1 | magic `'N'`, `'P'` |
| 2 | version = 1 |
| 3 | type = 2 |
| 4-5 | seq (uint16 LE) |
| 6 | cmd (bit0 = RESET, bit1 = 音量設定) |
| 7 | 音量 0-255 (bit1 のとき有効) |

RESET は実機の RESET ボタンと同じ意味論で、ワーク RAM は保持したまま
リセットベクタから起動し直す。シリアルへ `RESET: console reset` を出力する。

音量は Web 設定ダイアログのマスターボリュームをミラーしたもの。
Web の 1.0 が `SPEAKER_VOLUME_BASE` (=128) に対応し、スライダーの 0..1.5 は
0..192 に写る。ミュート中は 0。適用時にシリアルへ `VOL: <n>` を出力する。
一度も受信していない間は `src/config.h` の `SPEAKER_VOLUME` のまま。
M5Unified は出力タスクの DMA ブロックごとに音量を読み直すため、
`setVolume()` は再生中のストリームにも即座に効く。

CPU バス系のピンを抜くとエミュレート中のプログラムが暴走することがあり、
**端子を戻すだけでは復帰しない** (リセットベクタを読み直す必要がある)。
そのためブラウザの「まっすぐ挿す」は、ピンマスク送信の直後にこの RESET も送る。

magic / version が一致しないパケットは破棄する。

## ブラウザから端子をいじる

Web 版のカセット端子 UI を実機にミラーできる。サイコロ (接触不良の判定) は
ブラウザ側で振り、その結果を実機へ送るので、画面とパネルが同じカセットを映す。

```sh
just serve                       # web/ を配信しつつ UDP 中継も行う
# ブラウザで http://localhost:8000/?device=<CoreS3 の IP>
```

`?device=` は `localStorage` に保存されるので次回以降は省略できる
(クエリを付けた場合はそちらが優先)。ブラウザは UDP を送れないため、
同一オリジンの `POST /api/pins` を `tools/serve_web.py` が type 1 パケットへ
中継する。中継サーバーは 127.0.0.1 のみで待ち受ける。

## タスク構成

- **Core 1** (Arduino `loop()`) — エミュレーション + 音声 + 描画
- **Core 0** (`udp` タスク) — `recvfrom` でブロックし、ボタン状態の更新のみ行う

## 既知の制約

- 表示は約 30fps 程度に留まる見込み (エミュレーション自体は 60fps を目指してペーシング)
- WiFi の SSID / パスワードはコンパイル時に埋め込む (実行時設定 UI はなし)
- 描画は 256x240 等倍・x=32 で中央配置 (パネルは 320x240)
- `pushImageDMA` の転送は描画しないフレームの裏で走らせている。フレームバッファは
  内部 SRAM に 1 枚しか置けないため、次に描画するフレームの直前で転送完了を待つ
- RGB565 はコアのパレット LUT 側でバイトスワップ済み (`DISPLAY_SWAP_BYTES = false`)。
  ドライバ側でスワップさせるとラインごとにバウンスバッファを経由し、
  ゼロコピー DMA ができなくなるため
- CPU バス系のピン (2-13, 32-44) を抜いた場合、CPU はオープンバスを読む。
  読む瞬間のバス状態に依存するため、Web 版と実機で挙動が一致するとは限らない
  (PPU 系ピンは決定的で、両者ビット単位で一致する)

## 設定

`src/config.h` に UDP ポート、音量、`PERF_LOG`、バイトスワップなどの定数がある。
`PERF_LOG` を有効にすると 1 秒ごとに fps と emulation / push の平均 µs をシリアルに出力する。
