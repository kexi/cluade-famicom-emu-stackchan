# stackchan — 実機操作 CLI

M5Stack CoreS3 上で動くファミコンエミュレータを、コマンドラインから操作する。
ROM の書き込み・入れ替え・起動、SD 管理、本体制御、内部状態の観測、
コントローラ入力までを 1 つのバイナリで扱う。

実機は HTTP を持たず、UDP :5555 の独自プロトコルだけを喋る。この CLI は
そのプロトコルを直接話すので、`just serve` (Python の中継サーバ) は要らない。

## なぜ CLI か

これまで実機を操作する手段はブラウザの Web UI だけで、`just serve` を起動して
`?device=` を付けて画面をクリックする必要があった。スクリプトや AI から
自動化できる入り口が無かった、というのがこの CLI を作った理由。

`--json` を付けると全コマンドが機械可読な JSON を返すので、`jq` で読める。

## インストール

```sh
just cli-build                       # cli/target/release/stackchan ができる
```

リリースバイナリは GitHub Releases から取れる (macOS arm64 / Linux x64 / Linux arm64)。
**Linux 版は Pro コントローラ対応 (HID) を落としてある** — `hidapi` が要求する
`libudev` が静的リンクできないため。`input procon` と `input list` だけが使えず、
他は全部動く。自前でビルドすれば Linux でも HID は使える。

## 接続先の指定

`--host` に IP か mDNS 名を渡す。実機は起動時に画面へ両方を出す。

```sh
export STACKCHAN_HOST=stackchan-df5968.local   # 以降 --host を省略できる
```

| 指定方法 | 例 |
|---|---|
| 環境変数 | `STACKCHAN_HOST=stackchan-df5968.local` |
| オプション | `--host 192.168.1.177` / `-H stackchan-df5968.local` |

mDNS 名を勧める。DHCP でアドレスが変わっても同じ名前で届く。名前は
`stackchan-<MAC 下位 3 バイト>` で、機体ごとに違う。

**探索:**

```sh
stackchan discover
# stackchan-df5968.local  port 5555  192.168.1.177
```

`discover` だけは `--host` が要らない。LAN 上の `_nes._udp` を列挙する。

## コマンド

### 探索

```sh
stackchan discover                   # LAN 上の機体を列挙
stackchan discover --timeout 5       # 遠い機体を待つ (既定 2 秒)
```

### SD カードの ROM 管理

```sh
stackchan sd ls                      # 一覧
stackchan sd ls --json | jq .        # 機械可読
stackchan sd df                      # 空き容量
stackchan sd load smb.nes            # 起動する
stackchan sd mv old.nes new.nes      # 改名
stackchan sd rm unwanted.nes         # 削除
```

`sd load` はメニューを閉じてゲームを起動する。**メニューが出たままだと
`ctrl` / `pins` / `debug` が効かない** ので (下記「よくある詰まり」参照)、
実機がメニュー表示中なら最初にこれを撃つとよい。

### ROM を送る

```sh
stackchan rom send game.nes                          # 送って起動 (SD に残さない)
stackchan rom send game.nes --save game.nes          # SD に保存して起動
stackchan rom send game.nes --save game.nes --progress
curl -fsSL "$URL" | stackchan rom send - --save dl.nes
```

`-` で標準入力から読む。CLI 自身は HTTP を話さない — TLS を抱えると 3 ターゲットの
クロスビルドが壊れるので、ダウンロードは `curl` に任せる設計にしてある。

上限は 1MB (`ROM_MAX_SIZE`)。超えるものは読み切る前に断る。

### 本体制御

```sh
stackchan ctrl reset                 # リセット (ワーク RAM は保持)
stackchan ctrl volume 64             # 音量 0-255
stackchan ctrl menu                  # ROM 選択メニューを開く
```

いずれも実機は ACK を返さない。CLI は送ったら即 exit 0 で返るので、
**効いたかどうかは画面か `debug snapshot` で確かめる**。

### 端子破壊

カートリッジコネクタの 60 ピンを実行時に壊せる。ピン番号は 1〜60。

```sh
stackchan pins break 25              # ピン 25 を壊す (画面が壊れる)
stackchan pins break 25,29           # 複数
stackchan pins fix 25                # 直す
stackchan pins set all               # 全部繋ぐ (リセットも送る)
stackchan pins set none              # 全部壊す
stackchan pins set 0xfffffffffffffff # マスクを直接指定
```

**`break` / `fix` は「全ピン健全」からの差分**であって、現在値からの
積み上げではない。実機はピンマスクを読み出す API を持たないので、CLI は
今どのピンが壊れているかを知りえない。積み上げたいときは `pins set` に
マスクを渡す。

### 内部状態を覗く

```sh
stackchan debug snapshot             # CPU/APU レジスタ、PC 位置のコード
stackchan debug snapshot --json | jq .
stackchan debug snapshot --waves     # APU の波形も
stackchan debug wram --offset 0x300 --length 16
```

`snapshot` は 1 回きりで再送しない。取り直しても「さっきの状態」は返らないため。

### コントローラ入力

**スクリプトから (HID 不要):**

```sh
stackchan input send A B START             # 各 100ms 押して離す
stackchan input send --hold 500 A+RIGHT    # 同時押しを 500ms
stackchan input send NONE                  # 全部離す

echo "10 A
20 B
30 A+RIGHT" | stackchan input send --script -
```

`--script` は `tools/scenario-sample.txt` と同じ「フレーム番号 + ボタン」書式なので、
`just verify` 用に書いたシナリオをそのまま実機で流せる。

```sh
stackchan input test-pattern         # 11 個の固定パターンで疎通確認
```

**対話 (端末が要る):**

```sh
stackchan input keys                 # z=B x=A 矢印=十字 Enter=Start Space=Select
stackchan input keys --two-button    # A/B のみ (Dual Button Unit の再現)
```

Ctrl-C で抜ける。抜けるときに必ずボタンを離す。

**Pro コントローラ (USB, macOS 向け):**

```sh
stackchan input list                 # 繋がっているか確認
stackchan input procon               # 流し始める。HOME で ROM メニュー
```

Linux のリリースバイナリでは使えない (上記「インストール」参照)。

## 終了コード

| code | 意味 |
|---|---|
| 0 | 成功 |
| 1 | 実機が明示的に断った (存在しないファイルの削除など)、またはローカルの失敗 |
| 2 | 使用法エラー (引数が変、`--host` が無い) |
| 3 | 実機が時間内に答えなかった |
| 4 | **送ったが結果が判らない。実行されたかもしれない** |

3 と 4 を 1 と分けているのが要点。

- **3 (無応答)** は冪等な操作でだけ出る。再試行して構わない
- **4 (結果不明)** は `sd rm` / `sd mv` で無応答だったとき。これらは
  **再送してはいけない** — 実機に「同じ要求を 2 回受けた」ことを覚える仕組みが
  無いので、2 回目の削除は「存在しない」、2 回目の改名は「既にある」を返し、
  成功が失敗に化ける。`sd ls` で実際どうなったか確かめるのが正しい対応

```sh
stackchan sd rm foo.nes
case $? in
  0) echo "消えた" ;;
  1) echo "実機が断った (元から無かった等)" ;;
  4) echo "判らない。sd ls で確認せよ" ;;
esac
```

## グローバルオプション

| オプション | 環境変数 | 意味 |
|---|---|---|
| `-H, --host` | `STACKCHAN_HOST` | 接続先 (IP または mDNS 名) |
| `-p, --port` | `STACKCHAN_PORT` | UDP ポート (既定 5555) |
| `-t, --timeout` | `STACKCHAN_TIMEOUT` | 1 回の応答を待つ秒数 |
| `--json` | | 機械可読出力 (エラーも JSON) |
| `-q, --quiet` | | 成功時の出力を抑える |
| `-v, --verbose` | | 送受信をトレース。`-vv` で 16 進ダンプ |

`--timeout` が置き換えるのは **1 回の応答をどれだけ待つか**だけで、再送間隔や
BUSY を待つ上限には触らない。ROM の 0.3 秒と SD の BUSY 12 秒は意味が違うので、
1 つの数字で全部を殴らせない設計にしてある。

## よくある詰まり

### `ctrl` / `pins` / `debug` が効かないのに `sd` は動く

**実機が ROM 選択メニューを表示している。** `sd load <名前>` でゲームを起動すれば直る。

理由は実機側の処理経路の違いにある。SD の要求は受信タスクの中で完結するが、
`ctrl` / `pins` / `debug` はフレーム境界で処理される。メニュー表示中は
`loop()` が先に return してフレーム処理に到達しないので、この 3 つだけが
まとめて効かなくなる (`m5stack/src/main.cpp` の `loop()` 冒頭)。

実機は SD に ROM が 1 本以上あると起動時にメニューを出すので、電源を入れた直後や
書き込み直後はこの状態になっている。

### `discover` に何も出ない

- 実機がまだ WiFi に繋がっていない (起動から数秒待つ)
- firmware が mDNS 広告を持たない版 (`MDNS.addService("_nes", "_udp", ...)` が要る)
- Mac と実機が別のネットワークにいる

IP が判っているなら `--host 192.168.1.177` で直接叩ける。

### mDNS 名だと最初の 1 回だけ数秒待たされる

macOS の `getaddrinfo` は `.local` を 1 回引くのに約 5 秒かかる (IP なら 0ms)。
CLI は解決結果を 5 秒だけ持ち回すので、この待ちは**コマンドごとに 1 回**で済む。

急ぐ場面では IP を直接渡せばこの待ちは消える。`discover` が IP も表示する。

```sh
stackchan discover                   # stackchan-xxxxxx.local  port 5555  192.168.1.177
export STACKCHAN_HOST=192.168.1.177  # 待ちなし。ただし DHCP で変わりうる
```

**ROM 転送が極端に遅い場合** (48KB に数分) は、解決結果が持ち回されていない
古いビルドの可能性がある。実測で 194 秒 → 5.9 秒に変わった修正が入っている。

### `input keys` が `needs a terminal` と言って断る

パイプやリダイレクト経由では動かない。スクリプトから入力を送るなら
`input send` か `input send --script` を使う。

## 開発

```sh
just cli-build                       # リリースビルド
just cli-test                        # テスト (HID の有無 両方の構成で走る)
just cli-clippy                      # 静的解析
just format                          # rustfmt を含む整形
```

プロトコル定数の正本は `m5stack/src/config.h`。`cli/src/proto/constants.rs` は
その写しで、各定数のドキュメントに対応する C++ 側の名前を書いてある。
食い違いを疑ったときはそちらを引くこと。

実機が要らない検証はモックデバイス (`cli/tests/mock_*.rs`) が担う。パケットロス、
BUSY、順序ずれ、部分欠落といった実機で故意に起こしにくい状況は、そちらのほうが
確実に踏める。
