#!/bin/sh
set -eu

# 取得元は main ブランチを追う URL なので、上流が差し替われば中身が黙って変わる。
# それが困るのは just verify が ROM 固有の値を焼き込んでいるため — fb 差分の
# 許容窓 (justfile の known_line=135) は game.nes の分割位置そのもので、別の
# ROM に対しては何の意味も持たない。窓と ROM の対応が崩れると、verify は
# 「既知の差分」を誤って FAIL と呼ぶか、逆に本物の desync を見逃す。
#
# よってハッシュで留める。上流を意図的に追いたくなったら、この値を新しい ROM の
# ものに更新した上で、justfile の許容窓も引き直すこと (両者はセットで動く)。
ROM_SHA256=6cd4e6052a16ce014cbd0f175830b77ebe4b236615409f2156de00179cc8311d

# data/ は *.nes ごと gitignore されており checkout 直後は存在しない
dir="$(dirname "$0")/../data"
mkdir -p "$dir"
out="$dir/game.nes"

# 検証前の中身を残さない: 途中で落ちた場合に、壊れた ROM が「取得済み」として
# 居座ると次回以降 fetch がスキップされて原因が分かりにくくなる
tmp="$out.tmp.$$"
trap 'rm -f "$tmp"' EXIT

curl -fL https://raw.githubusercontent.com/GOROman/calude-famicom-game/main/game.nes -o "$tmp"

# sha256sum (GNU/nix) と shasum -a 256 (macOS 標準) のどちらかがあればよい
if command -v sha256sum >/dev/null 2>&1; then
    got=$(sha256sum "$tmp" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
    got=$(shasum -a 256 "$tmp" | cut -d' ' -f1)
else
    echo "fetch_rom: neither sha256sum nor shasum found; cannot verify the ROM" >&2
    exit 1
fi

if [ "$got" != "$ROM_SHA256" ]; then
    echo "fetch_rom: checksum mismatch for game.nes" >&2
    echo "  expected $ROM_SHA256" >&2
    echo "  actual   $got" >&2
    echo "The upstream ROM has changed. Update ROM_SHA256 here and re-derive the" >&2
    echo "framebuffer tolerance window (known_line/known_x) in the justfile's" >&2
    echo "verify recipe — the window is specific to this ROM." >&2
    exit 1
fi

mv "$tmp" "$out"
