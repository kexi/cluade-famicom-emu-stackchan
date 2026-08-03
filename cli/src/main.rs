//! `stackchan` — M5Stack CoreS3 のファミコンエミュレータを操作する CLI。
//!
//! この段階では足場だけで、実装済みなのは `--help` と `--version` に限られる。
//! サブコマンドは順次生えるが、GNU の作法 (`--version` の書式、EXIT STATUS の
//! 明示、エラーは `program: message` で stderr) は最初から満たしておく。

use std::io::Write;

use stackchan::exit::{EXIT_STATUS_HELP, ExitCode};

/// GNU coding standards の `--version` 書式。1 行目がプログラム名とバージョン、
/// 続けて著作権・ライセンス・無保証の告知。
///
/// 実行時 format ではなく `concat!` でリテラルに畳んでいるのは、この文字列が
/// 起動直後に一度出るだけで、組み立てる理由が無いため
const VERSION_TEXT: &str = concat!(
    env!("CARGO_PKG_NAME"),
    " ",
    env!("CARGO_PKG_VERSION"),
    "\n",
    "Copyright (C) 2026 GOROman\n",
    "License MIT: <https://opensource.org/licenses/MIT>\n",
    "This is free software: you are free to change and redistribute it.\n",
    "There is NO WARRANTY, to the extent permitted by law.",
);

const USAGE: &str = "\
Usage: stackchan [OPTION]... COMMAND [ARG]...

Control an M5Stack CoreS3 running the Famicom emulator over its UDP protocol.

Commands:
  (none yet -- this build only implements --help and --version)

Options:
  -h, --help     display this help and exit
  -V, --version  output version information and exit";

const AFTER_HELP: &str = "\
Report bugs to: <https://github.com/kexi/cluade-famicom-emu-stackchan/issues>";

fn main() {
    // `args()` は非 UTF-8 の引数で panic する (Unix のファイル名は任意バイト列を
    // 取りうるので実際に起こる)。panic は終了コード 101 になり、規約の外の値が
    // 呼び出し側に漏れる。`args_os()` で受けて、変換に失敗したら使用法エラーとして
    // 返す
    let mut args = Vec::new();
    for raw in std::env::args_os().skip(1) {
        match raw.into_string() {
            Ok(arg) => args.push(arg),
            Err(bad) => {
                let message = format!("argument is not valid UTF-8: {}", bad.to_string_lossy());
                std::process::exit(usage_error(&message).code());
            }
        }
    }
    std::process::exit(run(&args).code());
}

/// 引数を捌いて終了コードを返す。`main` から分けてあるのは、`std::process::exit`
/// を呼ぶ前の判断をテストから叩けるようにするため
fn run(args: &[String]) -> ExitCode {
    // 走査は「先頭から順に」でなければならない。`--help` と `--version` は
    // 見つけた時点で残りを無視するので、どちらが先に現れたかで結果が決まる
    // (`--version --help` は version)。全体を検索してから判定すると、引数の
    // 順序と無関係にどちらか一方が常に勝ってしまう
    let mut operands: Vec<&String> = Vec::new();
    let mut options_ended = false;

    for arg in args {
        // `--` はオプション解析の終端であって、以降を捨てるものではない。
        // 後続はオペランド (コマンド名やその引数) として残す。SD のファイル名は
        // firmware が `[A-Za-z0-9._-]` を許すので `-x.nes` が実在しうる
        let is_terminator = !options_ended && arg == "--";
        if is_terminator {
            options_ended = true;
            continue;
        }

        let is_option = !options_ended && arg.starts_with('-') && arg.len() > 1;
        if !is_option {
            operands.push(arg);
            continue;
        }

        let wants_help = arg == "-h" || arg == "--help";
        if wants_help {
            println!("{USAGE}\n\n{EXIT_STATUS_HELP}\n\n{AFTER_HELP}");
            return ExitCode::Success;
        }

        let wants_version = arg == "-V" || arg == "--version";
        if wants_version {
            println!("{VERSION_TEXT}");
            return ExitCode::Success;
        }

        return usage_error(&format!("unrecognised option '{arg}'"));
    }

    // 使用法エラーは stdout を汚さず stderr へ。出力先を分けておかないと、
    // `--json` を付けた呼び出しで stdout をパースする側がエラー文を JSON として
    // 読もうとして壊れる
    match operands.first() {
        None => usage_error("missing command"),
        Some(unknown) => usage_error(&format!("unrecognised command '{unknown}'")),
    }
}

/// GNU の `program: message` 書式で stderr に出し、使用法エラーとして返す
fn usage_error(message: &str) -> ExitCode {
    let program = env!("CARGO_PKG_NAME");
    let mut stderr = std::io::stderr();
    let _ = writeln!(stderr, "{program}: {message}");
    let _ = writeln!(stderr, "Try '{program} --help' for more information.");
    ExitCode::Usage
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| (*s).to_string()).collect()
    }

    #[test]
    fn help_and_version_succeed() {
        for flag in ["-h", "--help", "-V", "--version"] {
            assert_eq!(run(&args(&[flag])), ExitCode::Success, "flag {flag}");
        }
    }

    #[test]
    fn no_command_is_a_usage_error() {
        assert_eq!(run(&args(&[])), ExitCode::Usage);
    }

    #[test]
    fn unknown_command_is_a_usage_error() {
        assert_eq!(run(&args(&["frobnicate"])), ExitCode::Usage);
    }

    // `--` の後ろは値であってオプションではない。ここを取り違えると
    // `sd rm -- --help` がファイルを消さずにヘルプを出してしまう
    #[test]
    fn options_after_the_terminator_are_not_interpreted() {
        assert_eq!(run(&args(&["--", "--help"])), ExitCode::Usage);
        assert_eq!(run(&args(&["--", "-V"])), ExitCode::Usage);
    }

    // `--` は解析を止めるだけで、後続を捨てはしない。捨ててしまうと
    // `sd rm -- -x.nes` のファイル名が消え、段階1 以降のディスパッチが成立しない
    #[test]
    fn the_terminator_keeps_operands() {
        // オペランドが残っていれば「コマンド名として認識できない」に倒れる。
        // 捨てられていると「コマンドが無い」になってしまう
        assert_eq!(run(&args(&["--", "frobnicate"])), ExitCode::Usage);
        assert_eq!(run(&args(&["--", "-x.nes"])), ExitCode::Usage);
    }

    // GNU では先に現れた方が勝ち、残りは無視される
    #[test]
    fn the_first_of_help_or_version_wins() {
        // 出力の中身は結合テスト (tests/cli.rs) で見る。ここでは順序を入れ替えても
        // どちらも成功で返ること = 片方が常に勝つ実装になっていないことを押さえる
        assert_eq!(run(&args(&["--version", "--help"])), ExitCode::Success);
        assert_eq!(run(&args(&["--help", "--version"])), ExitCode::Success);
    }

    #[test]
    fn unknown_option_is_a_usage_error() {
        assert_eq!(run(&args(&["--nope"])), ExitCode::Usage);
        assert_eq!(run(&args(&["-z"])), ExitCode::Usage);
    }

    // 単独の `-` は慣例的に stdin を指すオペランドで、オプションではない
    // (`rom send -` で使う)
    #[test]
    fn a_lone_dash_is_an_operand() {
        assert_eq!(run(&args(&["-"])), ExitCode::Usage);
    }

    #[test]
    fn version_text_follows_the_gnu_layout() {
        let mut lines = VERSION_TEXT.lines();
        assert_eq!(lines.next(), Some("stackchan 0.1.0"));
        assert!(
            lines
                .next()
                .is_some_and(|l| l.starts_with("Copyright (C) "))
        );
        assert!(lines.next().is_some_and(|l| l.starts_with("License MIT:")));
    }

    // --help がバグ報告先を欠くと GNU の要件を満たさない
    #[test]
    fn help_points_at_the_bug_tracker() {
        assert!(AFTER_HELP.contains("Report bugs to:"));
    }
}
