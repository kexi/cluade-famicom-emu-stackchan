//! バイナリを実際に起動して、対外契約を検証する。
//!
//! `run()` の単体テストは終了コードしか見ないので、「stdout に出るか stderr に
//! 出るか」「実プロセスの終了コード」「`main` の配線」は保証できない。ここが
//! 壊れると、呼び出し側 (スクリプトや AI) が stdout をパースした瞬間に破綻する。
//!
//! dev-dependency を足していないのは、`CARGO_BIN_EXE_<name>` と
//! `std::process::Command` だけで足りるため。

use std::process::{Command, Output};

fn run(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_stackchan"))
        .args(args)
        .output()
        .expect("failed to run the stackchan binary")
}

fn stdout(out: &Output) -> String {
    String::from_utf8(out.stdout.clone()).expect("stdout was not valid UTF-8")
}

fn stderr(out: &Output) -> String {
    String::from_utf8(out.stderr.clone()).expect("stderr was not valid UTF-8")
}

#[test]
fn version_goes_to_stdout_and_exits_zero() {
    for flag in ["-V", "--version"] {
        let out = run(&[flag]);
        assert_eq!(out.status.code(), Some(0), "flag {flag}");
        assert!(stderr(&out).is_empty(), "flag {flag} wrote to stderr");

        // GNU の書式: 1 行目がプログラム名とバージョン、続けて著作権とライセンス
        let text = stdout(&out);
        let mut lines = text.lines();
        assert_eq!(lines.next(), Some("stackchan 0.1.0"), "flag {flag}");
        assert!(
            lines
                .next()
                .is_some_and(|l| l.starts_with("Copyright (C) ")),
            "flag {flag}"
        );
        assert!(
            lines.next().is_some_and(|l| l.starts_with("License MIT:")),
            "flag {flag}"
        );
    }
}

#[test]
fn help_goes_to_stdout_and_exits_zero() {
    for flag in ["-h", "--help"] {
        let out = run(&[flag]);
        assert_eq!(out.status.code(), Some(0), "flag {flag}");
        assert!(stderr(&out).is_empty(), "flag {flag} wrote to stderr");

        let text = stdout(&out);
        assert!(text.starts_with("Usage: stackchan"), "flag {flag}");
        assert!(text.contains("Exit status:"), "flag {flag}");
        assert!(text.contains("Report bugs to:"), "flag {flag}");
    }
}

/// 終了コード規約は対外的な約束なので、`--help` の記述と実際の値が食い違わない
/// ことを実プロセスで確かめる
#[test]
fn help_documents_every_exit_code() {
    let text = stdout(&run(&["--help"]));
    for code in ["  0  ", "  1  ", "  2  ", "  3  ", "  4  "] {
        assert!(text.contains(code), "EXIT STATUS help is missing {code:?}");
    }
}

#[test]
fn usage_errors_go_to_stderr_only_and_exit_two() {
    // 引数なし / 未知のコマンド / 未知のオプション の 3 系統
    for args in [vec![], vec!["frobnicate"], vec!["--nope"]] {
        let out = run(&args);
        assert_eq!(out.status.code(), Some(2), "args {args:?}");

        // stdout が汚れていると、--json を付けた呼び出しでパースが壊れる
        assert!(stdout(&out).is_empty(), "args {args:?} wrote to stdout");

        let text = stderr(&out);
        assert!(text.starts_with("stackchan: "), "args {args:?}: {text}");
        assert!(text.contains("Try 'stackchan --help'"), "args {args:?}");
    }
}

/// GNU では先に現れた方が勝ち、残りは無視される。ここは単体テストでは
/// 区別できない (どちらも成功で返るため) ので、実際の出力で確かめる
#[test]
fn the_first_of_help_or_version_wins() {
    let version_first = stdout(&run(&["--version", "--help"]));
    assert!(
        version_first.starts_with("stackchan 0.1.0"),
        "--version --help should print the version, got: {version_first}"
    );

    let help_first = stdout(&run(&["--help", "--version"]));
    assert!(
        help_first.starts_with("Usage: stackchan"),
        "--help --version should print the help, got: {help_first}"
    );
}

/// `--` は解析を止めるだけで、後続を捨てはしない。捨てていると
/// 「コマンドが無い」と誤診断され、段階1 以降のディスパッチが成立しない
#[test]
fn the_terminator_keeps_operands_as_commands() {
    let out = run(&["--", "frobnicate"]);
    assert_eq!(out.status.code(), Some(2));
    assert!(
        stderr(&out).contains("unrecognised command 'frobnicate'"),
        "operand after -- was dropped: {}",
        stderr(&out)
    );
}

/// 非 UTF-8 の引数で panic してはいけない。panic は終了コード 101 になり、
/// 規約 (0/1/2/3/4) の外の値が呼び出し側に漏れる
#[cfg(unix)]
#[test]
fn invalid_utf8_arguments_are_a_usage_error_not_a_panic() {
    use std::ffi::OsStr;
    use std::os::unix::ffi::OsStrExt;

    let out = Command::new(env!("CARGO_BIN_EXE_stackchan"))
        .arg(OsStr::from_bytes(b"\xff"))
        .output()
        .expect("failed to run the stackchan binary");

    assert_eq!(out.status.code(), Some(2), "expected a usage error");
    assert!(stderr(&out).starts_with("stackchan: "));
}

/// `--` の後ろのハイフン始まりはオプションとして解釈してはいけない。
/// SD のファイル名は firmware が `[A-Za-z0-9._-]` を許すので `-x.nes` が実在しうる
#[test]
fn the_terminator_protects_hyphen_leading_operands() {
    let out = run(&["--", "--help"]);
    assert_eq!(out.status.code(), Some(2), "-- --help must not print help");
    assert!(stdout(&out).is_empty(), "-- --help printed help to stdout");
    assert!(stderr(&out).contains("unrecognised command '--help'"));

    let out = run(&["--", "-x.nes"]);
    assert_eq!(out.status.code(), Some(2));
    assert!(stderr(&out).contains("unrecognised command '-x.nes'"));
}
