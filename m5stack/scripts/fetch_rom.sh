#!/bin/sh
set -eu

# game.nes を取得する、または既にあるものを検査する。
#
#   fetch_rom.sh          取得して検査する (既にあっても取り直す)
#   fetch_rom.sh --check  取得はせず、既にあるファイルだけを検査する
#
# --check があるのは、just verify が「ファイルがあれば何もしない」で済ませて
# いたため。存在するだけで中身は見ておらず、途中で壊れた ROM や別の ROM を
# 置いた状態でも検証がそのまま走ってしまう。verify は ROM 固有の値 (下記の
# 許容窓) を前提にしているので、これは黙って誤った結論を出す道になる。
#
# 取得元は main ブランチを追う URL なので、上流が差し替われば中身が黙って変わる。
# それが困るのは just verify が ROM 固有の値を焼き込んでいるため — fb 差分の
# 許容窓 (KNOWN_LINE) は game.nes の分割位置そのもので、別の ROM に対しては何の
# 意味も持たない。窓と ROM の対応が崩れると、verify は「既知の差分」を誤って
# FAIL と呼ぶか、逆に本物の desync を見逃す。
#
# よってハッシュで留める。期待値も許容窓も rom.lock が持つ (両者はセットで
# 動くので同じファイルに置いてある)。

script_dir="$(dirname "$0")"
lock="$script_dir/rom.lock"

test -f "$lock" || {
    echo "fetch_rom: missing $lock" >&2
    exit 1
}

# KEY=VALUE の行だけを拾う。lock を . で読み込まないのは、任意のシェルコードが
# 走りうる形にしたくないため — あれは値の表であって設定スクリプトではない。
ROM_SHA256=$(sed -n 's/^ROM_SHA256=//p' "$lock")
test -n "$ROM_SHA256" || {
    echo "fetch_rom: ROM_SHA256 not found in $lock" >&2
    exit 1
}

# data/ は *.nes ごと gitignore されており checkout 直後は存在しない
dir="$script_dir/../data"
out="$dir/game.nes"

# sha256sum (GNU/nix) と shasum -a 256 (macOS 標準) のどちらかがあればよい
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        echo "fetch_rom: neither sha256sum nor shasum found; cannot verify the ROM" >&2
        exit 1
    fi
}

# 不一致の説明は取得時と検査時で宛先が違う (上流が変わったのか、手元が壊れたのか)
# ので、共通部分だけをここに置いて呼び出し側が続きを足す。
report_mismatch() {
    echo "  expected $ROM_SHA256" >&2
    echo "  actual   $1" >&2
    echo "The framebuffer tolerance window (KNOWN_LINE/KNOWN_X in rom.lock) is" >&2
    echo "specific to this ROM, so the two must be updated together." >&2
}

if [ "${1:-}" = "--check" ]; then
    test -f "$out" || {
        echo "fetch_rom: $out does not exist; run ./m5stack/scripts/fetch_rom.sh" >&2
        exit 1
    }
    got=$(sha256_of "$out")
    if [ "$got" != "$ROM_SHA256" ]; then
        echo "fetch_rom: checksum mismatch for the existing $out" >&2
        report_mismatch "$got"
        echo "Delete it and re-run ./m5stack/scripts/fetch_rom.sh to fetch a clean copy," >&2
        echo "or update rom.lock if this ROM is intentionally the new baseline." >&2
        exit 1
    fi
    exit 0
fi

if [ $# -gt 0 ]; then
    echo "fetch_rom: unknown argument '$1' (expected nothing or --check)" >&2
    exit 2
fi

mkdir -p "$dir"

# 検証前の中身を残さない: 途中で落ちた場合に、壊れた ROM が「取得済み」として
# 居座ると次回以降 fetch がスキップされて原因が分かりにくくなる
tmp="$out.tmp.$$"
trap 'rm -f "$tmp"' EXIT

# タイムアウト付き: このスクリプトは just verify から自動で呼ばれるので、
# 接続が刺さったまま返らないと検証がハングしたようにしか見えない
curl -fL --connect-timeout 10 --max-time 60 \
    https://raw.githubusercontent.com/GOROman/calude-famicom-game/main/game.nes -o "$tmp"

got=$(sha256_of "$tmp")

if [ "$got" != "$ROM_SHA256" ]; then
    echo "fetch_rom: checksum mismatch for game.nes" >&2
    report_mismatch "$got"
    echo "The upstream ROM has changed. Update ROM_SHA256 in rom.lock and re-derive" >&2
    echo "the tolerance window there." >&2
    exit 1
fi

mv "$tmp" "$out"
