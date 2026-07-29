# Contributing

開発環境はすべて nix flake で固定しています(clang / Emscripten / PlatformIO / Python / just / lefthook / gitleaks)。
ツールを個別にインストールする必要はなく、環境の入り方は以下の2通りです。

## 1. nix のインストール

[nix](https://nixos.org) が必要です。未導入なら [Determinate Systems installer](https://install.determinate.systems/) が手軽です:

```sh
curl -fsSL https://install.determinate.systems/nix | sh -s -- install
```

公式インストーラを使う場合は flakes を有効にしてください(`~/.config/nix/nix.conf`):

```
experimental-features = nix-command flakes
```

## 2A. direnv を使う(推奨)

[direnv](https://direnv.net) を入れておくと、リポジトリに `cd` するだけで環境が整います。

```sh
# インストール例 (nix でも brew でも可)
nix profile install nixpkgs#direnv

# シェルへのフック (fish の場合。bash/zsh は direnv のドキュメント参照)
echo 'direnv hook fish | source' >> ~/.config/fish/config.fish
```

初回のみ、リポジトリ直下で許可が必要です:

```sh
cd cluade-famicom-emu-stackchan
direnv allow    # .envrc (use flake) を信頼する
```

以後は `cd` するだけで devshell に入ります。flake.lock の更新時は自動で再評価されます。

## 2B. direnv なしで使う

すべてのコマンドに `nix develop --command` を前置するか、シェルごと入ります:

```sh
nix develop                        # devshell に入る
nix develop --command just build   # 単発実行
```

## devshell に入ると自動で行われること

`flake.nix` の shellHook が以下を設定します。手動での準備は不要です:

- **pre-commit hook の設置** — `lefthook install` が自動実行され、コミット時に
  [gitleaks](https://github.com/gitleaks/gitleaks) がステージ済み差分の秘密情報を
  スキャンします(`lefthook.yml` 参照)。検出されるとコミットは中断されます
- PlatformIO 用の esptool 依存を `PYTHONPATH` に追加
- Emscripten のキャッシュを書き込み可能な場所 (`~/.cache/emscripten-*`) に複製

## 初回セットアップ

```sh
just secrets     # WiFi 設定の雛形を作成 → m5stack/src/secrets.h を編集 (gitignore 済み)
just fetch-rom   # デフォルト ROM (game.nes) を取得 (*.nes は gitignore 済み)
```

## よく使うタスク

タスクランナーは [just](https://github.com/casey/just) です。一覧は `just --list` で確認できます。

| タスク | コマンド |
|------|------|
| M5Stack ファームウェアのビルド | `just build` |
| ビルド + 実機書き込み | `just flash [port]` |
| シリアルモニタ | `just monitor` |
| web (WASM) のビルド | `just build-web` |
| web 配信 + 実機中継 | `just serve` → `http://localhost:8000/?device=<実機IP>` |
| コアの構文チェック (web/組み込み両モード) | `just check` |

## ブランチ運用

`main` へ直接コミットせず、feature ブランチを切って PR を作成してください:

```sh
git switch -c feature/<topic>
```
