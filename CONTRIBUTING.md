# Contributing

🌐 [日本語](CONTRIBUTING.ja.md) · [中文](CONTRIBUTING.zh.md)

The development toolchain is pinned by a nix flake (clang / Emscripten / PlatformIO / uv / just / lefthook / gitleaks).
Python scripts (`tools/*.py`) are run by uv, which resolves the interpreter and dependencies at run time from PEP 723 metadata (uv itself is pinned by the flake).
There is no need to install tools individually; there are two ways to enter the environment.

## 1. Install nix

[nix](https://nixos.org) is required. If you don't have it yet, the [Determinate Systems installer](https://install.determinate.systems/) is the easiest way:

```sh
curl -fsSL https://install.determinate.systems/nix | sh -s -- install
```

If you use the official installer, enable flakes (`~/.config/nix/nix.conf`):

```
experimental-features = nix-command flakes
```

## 2A. With direnv (recommended)

With [direnv](https://direnv.net) installed, just `cd` into the repository and the environment is ready.
We also recommend [nix-direnv](https://github.com/nix-community/nix-direnv).
direnv's built-in `use flake` neither caches the devshell nor registers a GC root,
so every `nix-collect-garbage` rebuilds the whole environment from scratch. nix-direnv solves both.

```sh
# Install example (nix or brew both work)
nix profile install nixpkgs#direnv nixpkgs#nix-direnv
echo 'source $HOME/.nix-profile/share/nix-direnv/direnvrc' >> ~/.config/direnv/direnvrc

# Hook into your shell (fish shown here; see the direnv docs for bash/zsh)
echo 'direnv hook fish | source' >> ~/.config/fish/config.fish
```

Only on first use, grant permission at the repository root:

```sh
cd cluade-famicom-emu-stackchan
direnv allow    # trust .envrc (use flake)
```

From then on, simply `cd` to enter the devshell. It re-evaluates automatically when flake.lock changes.

## 2B. Without direnv

Prefix every command with `nix develop --command`, or enter the shell:

```sh
nix develop                        # enter the devshell
nix develop --command just build   # one-off run
```

## What entering the devshell sets up automatically

The shellHook in `flake.nix` takes care of the following — no manual preparation needed:

- **Installs the pre-commit hook** — `lefthook install` runs automatically, and on each
  commit [gitleaks](https://github.com/gitleaks/gitleaks) scans the staged diff for
  secrets (see `lefthook.yml`). If anything is detected, the commit is aborted
- Adds PlatformIO's esptool dependencies to `PYTHONPATH`
- Copies the Emscripten cache to a writable location (`~/.cache/emscripten-*`)

## First-time setup

```sh
just secrets     # create the WiFi config template → edit m5stack/src/secrets.h (gitignored)
just fetch-rom   # fetch the default ROM (game.nes) (*.nes is gitignored)
```

## Common tasks

The task runner is [just](https://github.com/casey/just). Run `just --list` to see all tasks.
A table of the main tasks lives in the [Reproducible toolchain section of the README](README.md#reproducible-toolchain-nix) (kept there to avoid maintaining the table in two places).

## Branch workflow

Do not commit directly to `main`; create a feature branch and open a PR:

```sh
git switch -c feature/<topic>
```
