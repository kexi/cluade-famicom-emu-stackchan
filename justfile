# NES エミュレータ開発タスク (nix develop / direnv 環境で実行)

# タスク一覧を表示
default:
    @just --list

# 実機へ書き込まず、ホストと Web で日常的な変更を検証する
dev frames='120':
    @just format-check
    @just tidy
    @just lint-py
    @just lint-js
    @just check
    @just test-ppu-flag
    @just verify {{frames}}
    @just build-web

# ---------------------------------------------------------------- M5Stack

# M5Stack CoreS3 向けビルド
build:
    cd m5stack && pio run -e m5stack-cores3

# ビルドして実機へ書き込み (ポートは自動検出、指定も可: just flash /dev/cu.usbmodem1101)
flash port='':
    cd m5stack && pio run -e m5stack-cores3 -t upload {{ if port == '' { '' } else { '--upload-port ' + port } }}

# 計測ビルド (NES_PROFILE 入り。毎秒 apu=/ppu=/cpu= の内訳ログを出す)
build-profile:
    cd m5stack && pio run -e m5stack-cores3-profile

# 計測ビルドを実機へ書き込み (内訳計測が要るときだけ使う。本番は just flash)
flash-profile port='':
    cd m5stack && pio run -e m5stack-cores3-profile -t upload {{ if port == '' { '' } else { '--upload-port ' + port } }}

# シリアルモニタ (Ctrl+C で終了)
monitor port='/dev/cu.usbmodem1101':
    cd m5stack && pio device monitor -b 115200 -p {{port}}

# デフォルトROM (game.nes) を取得
fetch-rom:
    ./m5stack/scripts/fetch_rom.sh

# WiFi 設定ファイルの雛形を作成 (既存なら何もしない)
secrets:
    @test -f m5stack/src/secrets.h && echo "m5stack/src/secrets.h は既にあります" || cp m5stack/src/secrets.h.example m5stack/src/secrets.h

# ------------------------------------------------------------------- 入力

# プロコン→UDP 送信 (hidapi 直読み)。host は CoreS3 起動時に画面表示される
# IP または mDNS 名 (stackchan-xxxxxx.local)
procon host:
    uv run tools/procon_udp.py --backend hid --host {{host}}

# --------------------------------------------------------------------- Web

# Web (WASM) をビルド
build-web:
    ./build.sh

# Web 版をローカル配信 + 端子状態を実機へ UDP 中継 (http://localhost:8000)
# 実機に反映するには http://localhost:8000/?device=stackchan-xxxxxx.local を開く
# (起動画面に出る IP を直接指定してもよい)
serve port='8000':
    uv run tools/serve_web.py --port {{port}}

# 実機の SD カード内の ROM 一覧を表示 (host は CoreS3 の IP または mDNS 名)
#
# 中継サーバー (just serve) が別途動いている前提。ブラウザを開かずに
# 「カードに何が入っているか」だけ見たいとき用で、UI から辿るより速い。
# 実機と直接 UDP で話さず /api/sd/list を叩くのは、type 5 の分割応答の
# 組み立てを serve_web.py が既に持っているため
sd-list host port='8000':
    curl -sS -X POST http://localhost:{{port}}/api/sd/list \
        -H 'Content-Type: application/json' \
        -d '{"host":"{{host}}"}'

# -------------------------------------------------------------------- 検証

# コアの構文チェック (Web / 組み込み両モード)
check:
    clang++ -std=c++17 -fsyntax-only core/*.cpp
    clang++ -std=c++17 -DNES_EMBEDDED -fsyntax-only core/*.cpp

# 参照/組み込みビルドの bit-exact 検証を、操作シナリオ付きで走らせる
#
# シナリオは tools/scenario-sample.txt の書式 (フレーム番号 + ボタン)。
# 無入力の verify がタイトル画面しか踏まないのに対し、実際にゲームを進めて
# 背景スクロールとスプライトが動く状態で同じ 3 系列比較をかける。
#
# 注意: 既定のサンプルシナリオでは現状 FAIL する。これは回帰ではなく、
# 無入力では踏めなかった既知の副作用が見えているもので、内訳は 2 つ:
#
#   1. fb: 600 中 4 フレーム (349/350/500/503) で、許容窓の外に差分が出る。
#      形は「連続する 3-6 本のスキャンラインがまるごと 1 行ずれる」で、
#      散らばったピクセル差ではない。組み込み側の renderScanline() が 1 ライン
#      分を dot 256 で一括描画するのに対し参照側が逐次描くという、窓内の差分と
#      同じ機構が、スクロール分割の境界で出たもの。窓 (scanline 135) が固定値
#      なのに対しスクロール分割の位置は動くので、窓では拾えない
#   2. state: frame 502 の wram だけが食い違う。これは観測点のずれ (参照は
#      sl=241 dot=4、組み込みは dot=163 で止まる) によるもので、$0314-$0318 の
#      5 バイトを書き換えている途中を別の地点で覗いているだけ。前後の frame
#      501 / 503 では wram はバイト単位で完全一致するので desync ではない
#
# 窓を広げてこれらを飲み込ませてはいない。窓は「scanline 135 の最終タイル」と
# いう狭く構造的な主張で、それを緩めると本物の desync を隠す側に倒れるため
verify-scenario scenario='tools/scenario-sample.txt' frames='600':
    @just verify {{frames}} {{scenario}}

# 参照ビルドと組み込みビルドの bit-exact 検証 (tools/verify_host.cpp)
#
# 第 2 引数にシナリオファイルを渡すと入力付きで走る (既定は無入力)。
# 入力付きで走らせたいだけなら just verify-scenario のほうが読みやすい
verify frames='600' scenario='':
    #!/usr/bin/env bash
    # 3 系列を走らせて突き合わせる:
    #   (1) 参照ビルド      (フラグなし, 毎フレーム描画)
    #   (2) 組み込みビルド  (-DNES_EMBEDDED, 毎フレーム描画)
    #   (3) 組み込みビルド  (-DNES_EMBEDDED, 4 フレームに 1 回描画 = 実機の divisor 4)
    #
    # (1)vs(2) は「lockstep と CPU 先行 catch-up が同じ結果になるか」を、
    # (2)vs(3) は「描画を間引いても CPU から見える状態が変わらないか」を見る。
    #
    # 中間ファイルはリポジトリ外の一時ディレクトリに置く (mktemp -d)。ビルド
    # 成果物を作業ツリーに落とすと .gitignore の管理対象が増えるため
    set -euo pipefail
    # 無ければ取得し、あれば中身を検査する。「あれば何もしない」で済ませると、
    # 途中で壊れた ROM や別の ROM が置かれていても検証がそのまま走る — 下の
    # 許容窓は ROM 固有なので、それは黙って誤った結論を出す道になる
    if [ -f m5stack/data/game.nes ]; then
        ./m5stack/scripts/fetch_rom.sh --check
    else
        ./m5stack/scripts/fetch_rom.sh
    fi
    # シナリオは 3 系列すべてに同じものを渡す。片方だけに入力が入れば当然
    # 食い違うので、ここで組み立てて使い回す
    scen=()
    if [ -n '{{scenario}}' ]; then
        test -f '{{scenario}}' || { echo "no such scenario: {{scenario}}" >&2; exit 2; }
        scen=(scenario '{{scenario}}')
    fi
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    echo "building (clang++ -std=c++17 -O2)..."
    # 2 つのビルドは互いに独立 (出力先が別で、入力は読むだけ) なので同時に走らせる。
    #
    # exit status は wait <pid> で 1 つずつ受ける。`cmd &` の失敗は set -e に
    # 引っかからないので、まとめて wait しただけでは片方のビルドが転けても
    # 気づかず、次の実行が「無い実行ファイル」に当たって分かりにくく落ちる
    clang++ -std=c++17 -O2 -o "$work/ref" tools/verify_host.cpp core/*.cpp &
    pid_ref=$!
    clang++ -std=c++17 -O2 -DNES_EMBEDDED -o "$work/emb" tools/verify_host.cpp core/*.cpp &
    pid_emb=$!
    build_rc=0
    wait "$pid_ref" || build_rc=1
    wait "$pid_emb" || build_rc=1
    if [ "$build_rc" -ne 0 ]; then
        echo "verify: build failed" >&2
        exit 1
    fi
    if [ -n '{{scenario}}' ]; then
        echo "running {{frames}} frames x3 (scenario: {{scenario}})..."
    else
        echo "running {{frames}} frames x3 (no input)..."
    fi
    # 3 系列も互いに独立 (それぞれ別プロセスで powerOn からやり直し、出力先も別)。
    # ビルドと同じ理由で status は個別に受ける
    "$work/ref" m5stack/data/game.nes {{frames}} all   "${scen[@]+"${scen[@]}"}" > "$work/1-ref-all.txt" &
    pid1=$!
    "$work/emb" m5stack/data/game.nes {{frames}} all   "${scen[@]+"${scen[@]}"}" > "$work/2-emb-all.txt" &
    pid2=$!
    "$work/emb" m5stack/data/game.nes {{frames}} skip4 "${scen[@]+"${scen[@]}"}" > "$work/3-emb-skip4.txt" &
    pid3=$!
    run_rc=0
    wait "$pid1" || run_rc=1
    wait "$pid2" || run_rc=1
    wait "$pid3" || run_rc=1
    if [ "$run_rc" -ne 0 ]; then
        echo "verify: a run failed" >&2
        exit 1
    fi
    # 判定に使うのは各行の '#' より前 (STATE 列) だけ。'#' 以降は観測点依存の
    # 参考列で、フレーム境界では原理的に一致しない (verify_host.cpp 冒頭を参照)
    for f in 1-ref-all 2-emb-all 3-emb-skip4; do
        sed 's/  #.*//' "$work/$f.txt" | grep -v '^#' > "$work/$f.state"
    done
    rc=0
    echo
    echo "=== (1) reference/all vs (2) embedded/all: non-framebuffer ==="
    # fb 以外は厳密比較。ここが動いたら catch-up が CPU から見える状態を
    # 変えてしまっているので、無条件に FAIL
    for f in 1-ref-all 2-emb-all; do
        sed 's/ fb=[0-9A-F-]*//' "$work/$f.state" > "$work/$f.nofb12"
    done
    if diff -u "$work/1-ref-all.nofb12" "$work/2-emb-all.nofb12" > "$work/d12.txt"; then
        echo "OK: identical on all {{frames}} frames"
    else
        echo "FAIL: $(grep -c '^-[0-9]' "$work/d12.txt" || true) differing frames"
        head -40 "$work/d12.txt"
        rc=1
    fi
    echo
    echo "=== (1) reference/all vs (2) embedded/all: framebuffer ==="
    # 既知の許容領域: scanline 135 の最終タイル (x>=248)。
    #
    # 組み込み側の PPU::renderScanline() は 1 ライン分を dot 256 でまとめて
    # 描くのに対し、参照側の renderDot() はライン内を逐次フェッチする。この
    # ため game.nes が scanline 135 で行うライン途中のレジスタ書き換えが、
    # そのラインの最後のタイルの取り込みタイミングとしてしか現れない。
    # 「ライン一括描画の既知の副作用」として許容し、件数は WARN で報告する。
    #
    # 領域が game.nes 固有である点に注意。scanline はそのゲームの分割位置に
    # 依存するので、ROM を差し替えるなら必ず引き直すこと。x>=248 の側はタイル
    # 境界なので構造的だが、scanline は完全にゲーム依存。
    #
    # 値をここに直書きせず rom.lock から読むのは、窓と ROM ハッシュを機械的に
    # 結びつけるため。同じファイルに並んでいれば、ROM を差し替えようとした人の
    # 目に窓が必ず入る
    known_line=$(sed -n 's/^KNOWN_LINE=//p' m5stack/scripts/rom.lock)
    known_x=$(sed -n 's/^KNOWN_X=//p' m5stack/scripts/rom.lock)
    if [ -z "$known_line" ] || [ -z "$known_x" ]; then
        echo "verify: KNOWN_LINE/KNOWN_X missing from m5stack/scripts/rom.lock" >&2
        exit 2
    fi
    fbdiff=$(join -j1 \
        <(awk '{print $1, $NF}' "$work/1-ref-all.state") \
        <(awk '{print $1, $NF}' "$work/2-emb-all.state") \
        | awk '$2 != $3 {print $1}')
    nfb=$(printf '%s' "$fbdiff" | grep -c . || true)
    if [ "$nfb" -eq 0 ]; then
        echo "OK: framebuffer identical on all {{frames}} frames"
    else
        # CRC が違ったフレームだけ index 列で引き直し、差分ピクセルの位置を出す。
        # 全フレームを index 列で出すと {{frames}} フレームで数十 MB になるため、
        # trace(CRC) で絞ってから dump で位置特定する 2 パスにしている。
        #
        # dump は差分フレームをまとめて 1 回で吐く。1 フレームずつ呼ぶと
        # powerOn からのフル再実行が差分フレーム数だけ繰り返され、73 フレーム
        # 差分なら 73x2 回の再実行 = O(n^2) になっていた
        # fbdiff は join の出力を辿ったものなので、この時点で既に昇順・重複なし
        # (join は整列済み入力を要求し、整列済みで出す)。framelist も下の
        # framesorted もその前提に乗る
        framenums=$(printf '%s\n' $fbdiff | sed 's/^0*//' | sed 's/^$/0/')
        framelist=$(printf '%s\n' "$framenums" | paste -sd, -)
        # 上の 3 系列と同じく独立なので並列に。status も同じく個別に受ける
        "$work/ref" m5stack/data/game.nes {{frames}} all dump "$framelist" "${scen[@]+"${scen[@]}"}" > "$work/fa.bin" &
        pid_da=$!
        "$work/emb" m5stack/data/game.nes {{frames}} all dump "$framelist" "${scen[@]+"${scen[@]}"}" > "$work/fb.bin" &
        pid_db=$!
        dump_rc=0
        wait "$pid_da" || dump_rc=1
        wait "$pid_db" || dump_rc=1
        if [ "$dump_rc" -ne 0 ]; then
            echo "verify: a dump run failed" >&2
            exit 1
        fi
        # 出力は 61440 バイト/フレームの固定長連結 (verify_host.cpp 冒頭)。
        # フレームは昇順に並ぶので、i 番目のフレーム番号を配列で持っておけば
        # バイト位置からフレームを引ける
        framesorted=($framenums)
        #
        # cmp の exit code には一切依存しない。cmp は差分があれば exit 1 を返す
        # のが正しい挙動で、set -euo pipefail の下ではそこでスクリプトが即死する。
        # ここが従来 PASS していたのは nix の diffutils 3.12 の cmp が「出力先が
        # パイプだと差分があっても exit 0 を返す」バグを持っていたからで、
        # 正しい cmp に替わった瞬間に壊れる。出力だけを || true 付きで一度受け、
        # 分類は awk に任せる
        cmpout=$(cmp -l "$work/fa.bin" "$work/fb.bin" || true)
        #
        # cmp -l は 1-origin のバイト位置を出すので 1 引いて (frame, y, x) に直す。
        # 窓内/窓外を分けて数え、窓外は「frame の scanline y に n ピクセル」の形に
        # 畳んで出す。座標を 1 ピクセルずつ並べないのは、この差分が出るときは
        # たいてい行単位でまとまって出る (ライン一括描画がずれるため) からで、
        # 生座標を数千行流すより「どの行が何ピクセル」のほうが読める
        classified=$(printf '%s\n' "$cmpout" | awk -v L="$known_line" -v X="$known_x" \
            -v flist="$(printf '%s ' "${framesorted[@]}")" '
            BEGIN { split(flist, fr, " ") }
            NF == 0 { next }
            {
                p = $1 - 1
                fi = int(p / 61440) + 1          # 何番目のダンプフレームか (1-origin)
                q  = p % 61440
                y  = int(q / 256); x = q % 256
                if (y == L && x >= X) { inpix++; next }
                outpix++
                key = fr[fi]
                if (!(key in seenf)) { seenf[key] = 1; order[++n] = key }
                cell = key "/" y
                if (!(cell in seenc)) { seenc[cell] = 1; lines[key] = lines[key] " " y }
                cnt[cell]++
                if (!(cell in minx) || x < minx[cell]) minx[cell] = x
                if (!(cell in maxx) || x > maxx[cell]) maxx[cell] = x
            }
            END {
                printf "inside=%d outside=%d outframes=%d\n", inpix + 0, outpix + 0, n + 0
                for (i = 1; i <= n; i++) {
                    key = order[i]
                    ny = split(lines[key], ys, " ")
                    s = ""
                    for (j = 1; j <= ny; j++) {
                        cell = key "/" ys[j]
                        s = s sprintf(" y=%s(%dpx x=%d..%d)", ys[j], cnt[cell], minx[cell], maxx[cell])
                    }
                    printf "frame %s:%s\n", key, s
                }
            }')
        counts=$(printf '%s\n' "$classified" | head -1)
        inside=$(printf '%s' "$counts" | sed 's/.*inside=\([0-9]*\).*/\1/')
        outpix=$(printf '%s' "$counts" | sed 's/.*outside=\([0-9]*\).*/\1/')
        outframes=$(printf '%s' "$counts" | sed 's/.*outframes=\([0-9]*\).*/\1/')
        if [ "$outframes" -eq 0 ]; then
            echo "WARN: $nfb of {{frames}} frames differ, $inside pixels total"
            echo "      all within the known window (scanline $known_line, x>=$known_x)"
            echo "      = renderScanline's whole-line draw, accepted (see the known_line comment above)"
        else
            echo "FAIL: $outframes frame(s), $outpix pixel(s) differ outside the known window"
            echo "      ($inside further pixel(s) are inside the window)"
            printf '%s\n' "$classified" | tail -n +2 | head -40 | sed 's/^/  OUTSIDE: /'
            rc=1
        fi
    fi
    echo
    echo "=== (2) embedded/all vs (3) embedded/skip4 ==="
    # skip4 は描画しないフレームの fb が '-' になるので、fb 列を落とした残り
    # (= CPU から観測できる状態) が全行一致することを見る
    for f in 2-emb-all 3-emb-skip4; do
        sed 's/ fb=[0-9A-F-]*//' "$work/$f.state" > "$work/$f.nofb"
    done
    if diff -u "$work/2-emb-all.nofb" "$work/3-emb-skip4.nofb" > "$work/d23.txt"; then
        echo "OK: non-framebuffer state identical on all {{frames}} frames"
    else
        echo "FAIL: $(grep -c '^-[0-9]' "$work/d23.txt" || true) differing frames"
        head -40 "$work/d23.txt"
        rc=1
    fi
    # (3) が実際に描画したフレームは、その fb が (2) の同フレームと一致すること
    join -j1 \
        <(awk '{print $1, $NF}' "$work/2-emb-all.state") \
        <(awk '$NF != "fb=-" {print $1, $NF}' "$work/3-emb-skip4.state") \
        > "$work/fbjoin.txt"
    drawn=$(wc -l < "$work/fbjoin.txt" | tr -d ' ')
    bad=$(awk '$2 != $3' "$work/fbjoin.txt" | tee "$work/fbbad.txt" | wc -l | tr -d ' ')
    if [ "$bad" -eq 0 ]; then
        echo "OK: framebuffer matches on all $drawn drawn frames"
    else
        echo "FAIL: $bad of $drawn drawn frames differ"
        head -20 "$work/fbbad.txt"
        rc=1
    fi
    echo
    if [ "$rc" -eq 0 ]; then echo "verify: PASS"; else echo "verify: FAIL"; fi
    exit $rc

# PPU::frameFullyRendered() の単体テスト (tools/ppu_flag_test.cpp)
#
# verify が実 ROM の流しっぱなしで bit-exact を見るのに対し、こちらは $2001 を
# 狙ったドットで書いてフラグの述語だけを突く。可視行の描画区間の内と外で
# レンダリングを切り替えるケースは、ROM を流すだけでは踏めるかどうかが
# ゲーム任せになるため、直接叩くテストを分けている。
#
# 参照/組み込みの両方でビルドして走らせる。framebuffer を書くドットは両者で
# 違うが、フラグの述語 (「描画区間 1..256 の全体でレンダリングが有効だったか」)
# は同じ答えにならなければならない
test-ppu-flag:
    #!/usr/bin/env bash
    set -euo pipefail
    # 窓や CRC には依存しないが、マッパーが要る (PPU が dot 260 で
    # mapper->scanline() を呼ぶ) ので verify と同じ ROM を使う
    if [ -f m5stack/data/game.nes ]; then
        ./m5stack/scripts/fetch_rom.sh --check
    else
        ./m5stack/scripts/fetch_rom.sh
    fi
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    clang++ -std=c++17 -O2 -o "$work/ref" tools/ppu_flag_test.cpp core/*.cpp
    clang++ -std=c++17 -O2 -DNES_EMBEDDED -o "$work/emb" tools/ppu_flag_test.cpp core/*.cpp
    "$work/ref" m5stack/data/game.nes
    "$work/emb" m5stack/data/game.nes

# コードを整形 (C++ は .clang-format、Python は ruff.toml、JS は .oxfmtrc.json 準拠)
#
# oxfmt に web/*.js を明示するのは、ディレクトリを渡すと index.html まで
# 整形対象に入るため (HTML の整形は今回の範囲外)
format:
    clang-format -i core/*.cpp core/*.h m5stack/src/*.cpp m5stack/src/*.h tools/*.cpp
    ruff format tools/
    oxfmt 'web/*.js'

# 整形漏れがないか検査 (差分があれば失敗)
format-check:
    clang-format --dry-run --Werror core/*.cpp core/*.h m5stack/src/*.cpp m5stack/src/*.h tools/*.cpp
    ruff format --check tools/
    oxfmt --check 'web/*.js'

# Python の静的解析 (ruff.toml 準拠。PEP 8 + pyflakes + import 整列)
lint-py:
    ruff check tools/

# JS の静的解析 (.oxlintrc.json 準拠)。warning も CI を落とすため --deny-warnings
lint-js:
    oxlint --deny-warnings web/

# コアと検証ハーネスの静的解析 (.clang-tidy 準拠)。m5stack/src は対象外 —
# ESP-IDF/Arduino のヘッダが必要で、PlatformIO の toolchain 抜きには単体で
# パースできない
#
# tools/*.cpp を入れているのは、ハーネスがコアと同じ「素の clang++ で通る」
# 制約で書かれていて、core 向けの設定がそのまま通るため (実際に通した上で
# 対象化している)
#
# nix の clang++ は libc++ の include パスをラッパ経由で注入するため、素の
# clang-tidy には見えない。NIX_CFLAGS_COMPILE だけでは c++/v1 が欠けるので、
# ラッパ自身に検索パスを吐かせて -isystem として渡す
tidy:
    #!/usr/bin/env bash
    set -euo pipefail
    isystem=$(echo | clang++ -std=c++17 -E -x c++ - -v 2>&1 \
        | sed -n '/^#include <\.\.\.>/,/^End of search/p' \
        | grep '^ /' | grep -v framework | sed 's/^ /-isystem /' | tr '\n' ' ')
    clang-tidy --quiet core/*.cpp tools/*.cpp -- -std=c++17 $isystem
