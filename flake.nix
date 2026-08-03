{
  description = "Famicom emulator dev environment (web + M5Stack CoreS3)";

  inputs = {
    # unstable は darwin で pygame-ce のビルドが壊れているため 25.05 に固定
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    # oxlint / oxfmt 専用の第 2 入力。25.05 には oxfmt が無く oxlint も 0.16.7 と
    # 古いため (設定形式が現行と別物)、この 2 つだけ unstable から取る。
    # 本体を unstable にしないのは上記 pygame-ce の破損を踏むため。この 2 つを
    # 例外にできるのは、どちらも Rust 単体バイナリで stdenv や python を共有せず
    # 他パッケージと衝突しないから
    nixpkgs-unstable.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, nixpkgs-unstable, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        pkgsUnstable = nixpkgs-unstable.legacyPackages.${system};
        # nixpkgs の platformio-core は esptool の実行時依存を同梱しないため、
        # pio と同じ python (3.12) の site-packages を PYTHONPATH で補う。
        # pip は tool-esptoolpy の postinstall が `python -m pip install` を
        # 呼ぶため (nix の python 環境は pip を持たず初回インストールが失敗する)
        pioEsptoolDeps = with pkgs.python312Packages; [ intelhex reedsolo bitstring pip ];
        pioEsptoolPath = pkgs.lib.concatMapStringsSep ":"
          (p: "${p}/lib/python3.12/site-packages") pioEsptoolDeps;
      in
      {
        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.clang           # just check (Linux の stdenv は gcc のため明示的に入れる)
            pkgs.clang-tools     # just format / tidy (clang-format / clang-tidy を供給。pkgs.clang と同じ既定 LLVM 版)
            pkgs.platformio-core # M5Stack CoreS3 (m5stack/)
            pkgs.emscripten      # Web/WASM (build.sh)
            pkgs.gnused          # build.sh は GNU sed 前提 (BSD sed 非対応)
            pkgs.uv              # tools/*.py は PEP 723 メタデータ + uv run で実行
            pkgs.ruff            # just format / lint-py (tools/*.py の PEP 8 準拠を検査。ruff.toml 参照)
            pkgsUnstable.oxfmt   # just format / format-check (web/*.js の整形。.oxfmtrc.json 参照)
            pkgsUnstable.oxlint  # just lint-js (web/*.js の静的解析。.oxlintrc.json 参照)
            pkgs.pkg-config      # hidapi (cli/ の procon feature) が Linux で libudev を探すのに使う
            pkgs.cargo           # just cli-build / cli-test (cli/ の Rust CLI。cli/Cargo.toml 参照)
            pkgs.rustc           # 同上。pkgs.cargo は cargo 単体しか入れないため別途必要
            pkgs.clippy          # just cli-clippy (cli/ の静的解析。Cargo.toml の [lints.clippy] 参照)
            pkgs.rustfmt         # just format / format-check (cli/ の整形。rustfmt の既定に従うため設定ファイルは無し)
            pkgs.just            # タスクランナー (justfile 参照)
            pkgs.lefthook        # git hook 管理 (lefthook.yml 参照)
            pkgs.gitleaks        # pre-commit での秘密情報スキャン
            pkgs.pinact          # GitHub Actions の SHA ピン留め (CI と同じ版を flake で固定)
          ]
          # hidapi は Linux で libudev (hidraw backend) を要る。darwin は
          # IOKit なので不要。静的リンクできないので、Linux 向けの配布バイナリは
          # procon feature を落として配る (cli/Cargo.toml 参照)
          ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.udev ];
          # nix store 内の emscripten キャッシュは読み取り専用のため、
          # 書き込み可能な場所に複製して EM_CACHE を向ける
          shellHook = ''
            export PYTHONPATH="${pioEsptoolPath}''${PYTHONPATH:+:$PYTHONPATH}"
            # shell に入るだけで pre-commit hook が揃うように。手動の
            # `lefthook install` 忘れでスキャンなしのコミットが通るのを防ぐ
            if [ -d .git ]; then lefthook install --force > /dev/null; fi
            export EM_CACHE="$HOME/.cache/emscripten-${pkgs.emscripten.version}"
            if [ ! -d "$EM_CACHE" ]; then
              mkdir -p "$EM_CACHE"
              cp -r ${pkgs.emscripten}/share/emscripten/cache/. "$EM_CACHE"
              chmod -R u+w "$EM_CACHE"
            fi
          '';
        };
      });
}
