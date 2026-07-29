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

# コードを整形 (.clang-format 準拠)
format:
    clang-format -i core/*.cpp core/*.h m5stack/src/*.cpp m5stack/src/*.h

# 整形漏れがないか検査 (差分があれば失敗)
format-check:
    clang-format --dry-run --Werror core/*.cpp core/*.h m5stack/src/*.cpp m5stack/src/*.h

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
