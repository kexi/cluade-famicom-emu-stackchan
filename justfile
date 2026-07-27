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

# Web 版をローカル配信 (http://localhost:8000)
serve port='8000':
    uv run python -m http.server {{port}} -d web

# -------------------------------------------------------------------- 検証

# コアの構文チェック (Web / 組み込み両モード)
check:
    clang++ -std=c++17 -fsyntax-only core/*.cpp
    clang++ -std=c++17 -DNES_EMBEDDED -fsyntax-only core/*.cpp
