# NES エミュレータ開発タスク (nix develop / direnv 環境で実行)

# タスク一覧を表示
default:
    @just --list

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

# プロコン→UDP 送信 (hidapi 直読み)。host は CoreS3 起動時に画面表示される IP
procon host:
    uv run tools/procon_udp.py --backend hid --host {{host}}

# --------------------------------------------------------------------- Web

# Web (WASM) をビルド
build-web:
    ./build.sh

# Web 版をローカル配信 + 端子状態を実機へ UDP 中継 (http://localhost:8000)
# 実機に反映するには http://localhost:8000/?device=<CoreS3 の IP> を開く
serve port='8000':
    uv run tools/serve_web.py --port {{port}}

# -------------------------------------------------------------------- 検証

# コアの構文チェック (Web / 組み込み両モード)
check:
    clang++ -std=c++17 -fsyntax-only core/*.cpp
    clang++ -std=c++17 -DNES_EMBEDDED -fsyntax-only core/*.cpp

# 参照ビルドと組み込みビルドの bit-exact 検証 (tools/verify_host.cpp)
verify frames='600':
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
    test -f m5stack/data/game.nes || ./m5stack/scripts/fetch_rom.sh
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    echo "building (clang++ -std=c++17 -O2)..."
    clang++ -std=c++17 -O2 -o "$work/ref" tools/verify_host.cpp core/*.cpp
    clang++ -std=c++17 -O2 -DNES_EMBEDDED -o "$work/emb" tools/verify_host.cpp core/*.cpp
    echo "running {{frames}} frames x3..."
    "$work/ref" m5stack/data/game.nes {{frames}} all   > "$work/1-ref-all.txt"
    "$work/emb" m5stack/data/game.nes {{frames}} all   > "$work/2-emb-all.txt"
    "$work/emb" m5stack/data/game.nes {{frames}} skip4 > "$work/3-emb-skip4.txt"
    # 判定に使うのは各行の '#' より前 (STATE 列) だけ。'#' 以降は観測点依存の
    # 参考列で、フレーム境界では原理的に一致しない (verify_host.cpp 冒頭を参照)
    for f in 1-ref-all 2-emb-all 3-emb-skip4; do
        sed 's/  #.*//' "$work/$f.txt" | grep -v '^#' > "$work/$f.state"
    done
    rc=0
    echo
    echo "=== (1) reference/all vs (2) embedded/all ==="
    if diff -u "$work/1-ref-all.state" "$work/2-emb-all.state" > "$work/d12.txt"; then
        echo "OK: identical on all {{frames}} frames"
    else
        echo "MISMATCH: $(grep -c '^-[0-9]' "$work/d12.txt" || true) differing frames"
        head -40 "$work/d12.txt"
        rc=1
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
        echo "MISMATCH: $(grep -c '^-[0-9]' "$work/d23.txt" || true) differing frames"
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
        echo "MISMATCH: $bad of $drawn drawn frames differ"
        head -20 "$work/fbbad.txt"
        rc=1
    fi
    echo
    if [ "$rc" -eq 0 ]; then echo "verify: PASS"; else echo "verify: FAIL"; fi
    exit $rc

# コードを整形 (C++ は .clang-format、Python は ruff.toml、JS は .oxfmtrc.json 準拠)
#
# oxfmt に web/*.js を明示するのは、ディレクトリを渡すと index.html まで
# 整形対象に入るため (HTML の整形は今回の範囲外)
format:
    clang-format -i core/*.cpp core/*.h m5stack/src/*.cpp m5stack/src/*.h
    ruff format tools/
    oxfmt 'web/*.js'

# 整形漏れがないか検査 (差分があれば失敗)
format-check:
    clang-format --dry-run --Werror core/*.cpp core/*.h m5stack/src/*.cpp m5stack/src/*.h
    ruff format --check tools/
    oxfmt --check 'web/*.js'

# Python の静的解析 (ruff.toml 準拠。PEP 8 + pyflakes + import 整列)
lint-py:
    ruff check tools/

# JS の静的解析 (.oxlintrc.json 準拠)。warning も CI を落とすため --deny-warnings
lint-js:
    oxlint --deny-warnings web/

# コアの静的解析 (.clang-tidy 準拠)。m5stack/src は対象外 — ESP-IDF/Arduino の
# ヘッダが必要で、PlatformIO の toolchain 抜きには単体でパースできない
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
    clang-tidy --quiet core/*.cpp -- -std=c++17 $isystem
