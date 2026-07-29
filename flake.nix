{
  description = "Famicom emulator dev environment (web + M5Stack CoreS3)";

  inputs = {
    # unstable は darwin で pygame-ce のビルドが壊れているため 25.05 に固定
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        # nixpkgs の platformio-core は esptool の実行時依存を同梱しないため、
        # pio と同じ python (3.12) の site-packages を PYTHONPATH で補う
        pioEsptoolDeps = with pkgs.python312Packages; [ intelhex reedsolo bitstring ];
        pioEsptoolPath = pkgs.lib.concatMapStringsSep ":"
          (p: "${p}/lib/python3.12/site-packages") pioEsptoolDeps;
      in
      {
        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.platformio-core # M5Stack CoreS3 (m5stack/)
            pkgs.emscripten      # Web/WASM (build.sh)
            pkgs.gnused          # build.sh は GNU sed 前提 (BSD sed 非対応)
            pkgs.uv              # tools/*.py は PEP 723 メタデータ + uv run で実行
            pkgs.just            # タスクランナー (justfile 参照)
            pkgs.lefthook        # git hook 管理 (lefthook.yml 参照)
            pkgs.gitleaks        # pre-commit での秘密情報スキャン
          ];
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
