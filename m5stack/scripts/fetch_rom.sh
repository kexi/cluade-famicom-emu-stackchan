#!/bin/sh
set -eu

# data/ は *.nes ごと gitignore されており checkout 直後は存在しない
mkdir -p "$(dirname "$0")/../data"
curl -fL https://raw.githubusercontent.com/GOROman/calude-famicom-game/main/game.nes \
    -o "$(dirname "$0")/../data/game.nes"
