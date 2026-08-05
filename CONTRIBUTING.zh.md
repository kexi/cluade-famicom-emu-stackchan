# Contributing

🌐 [English](CONTRIBUTING.md) · [日本語](CONTRIBUTING.ja.md)

开发工具链由 nix flake 固定(clang / Emscripten / PlatformIO / cargo / uv / just / lefthook / gitleaks)。
Python 脚本(`tools/*.py`)由 uv 在运行时根据 PEP 723 元数据解析解释器与依赖(uv 本身由 flake 固定)。
无需单独安装各个工具,进入环境有以下两种方式。

## 1. 安装 nix

需要 [nix](https://nixos.org)。如果尚未安装,[Determinate Systems installer](https://install.determinate.systems/) 最为便捷:

```sh
curl -fsSL https://install.determinate.systems/nix | sh -s -- install
```

如果使用官方安装器,请启用 flakes(`~/.config/nix/nix.conf`):

```
experimental-features = nix-command flakes
```

## 2A. 使用 direnv(推荐)

安装 [direnv](https://direnv.net) 后,只需 `cd` 进仓库,环境即自动就绪。
同时推荐安装 [nix-direnv](https://github.com/nix-community/nix-direnv)。
direnv 自带的 `use flake` 既不缓存 devshell,也不注册 GC root,
因此每次 `nix-collect-garbage` 都会把整个环境从头重建。nix-direnv 可以同时解决这两个问题。

```sh
# 安装示例(nix 或 brew 均可)
nix profile install nixpkgs#direnv nixpkgs#nix-direnv
echo 'source $HOME/.nix-profile/share/nix-direnv/direnvrc' >> ~/.config/direnv/direnvrc

# 挂接到你的 shell(此处以 fish 为例;bash/zsh 请参阅 direnv 文档)
echo 'direnv hook fish | source' >> ~/.config/fish/config.fish
```

仅首次需要在仓库根目录授权:

```sh
cd cluade-famicom-emu-stackchan
direnv allow    # 信任 .envrc (use flake)
```

之后只需 `cd` 即可进入 devshell。flake.lock 更新时会自动重新评估。

## 2B. 不使用 direnv

在每条命令前加 `nix develop --command`,或直接进入 shell:

```sh
nix develop                        # 进入 devshell
nix develop --command just build   # 单次执行
```

## 进入 devshell 时自动完成的设置

`flake.nix` 的 shellHook 会完成以下设置,无需手动准备:

- **安装 pre-commit hook** — 自动执行 `lefthook install`,提交时
  [gitleaks](https://github.com/gitleaks/gitleaks) 会扫描已暂存差异中的敏感信息
  (参见 `lefthook.yml`)。一旦检测到,提交将被中断。
  涉及 `cli/` 下 Rust 源文件的提交还会运行 `cargo fmt --check`
- 将 PlatformIO 的 esptool 依赖加入 `PYTHONPATH`
- 将 Emscripten 缓存复制到可写位置(`~/.cache/emscripten-*`)

## 首次设置

```sh
just secrets     # 生成 WiFi 配置模板 → 编辑 m5stack/src/secrets.h(已 gitignore)
just fetch-rom   # 获取默认 ROM (game.nes)(*.nes 已 gitignore)
```

## 常用任务

任务运行器是 [just](https://github.com/casey/just)。运行 `just --list` 可查看全部任务。
主要任务的一览表在 [README 的 Reproducible toolchain 一节](README.md#reproducible-toolchain-nix)(为避免重复维护表格,此处不再罗列)。

## 分支流程

请勿直接向 `main` 提交,请创建 feature 分支并发起 PR:

```sh
git switch -c feature/<topic>
```
