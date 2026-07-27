#!/bin/sh
set -eu

curl -fL https://raw.githubusercontent.com/GOROman/calude-famicom-game/main/game.nes \
    -o "$(dirname "$0")/../data/game.nes"
