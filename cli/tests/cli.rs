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
        assert!(text.contains("Usage: stackchan"), "flag {flag}");
        assert!(text.contains("Exit status:"), "flag {flag}");
        assert!(text.contains("Report bugs to:"), "flag {flag}");
    }
}

/// サブコマンドにも `--help` が届き、その配下だけが見える
#[test]
fn subcommands_have_their_own_help() {
    let text = stdout(&run(&["ctrl", "--help"]));
    assert!(text.contains("reset"), "ctrl help should list reset");
    assert!(text.contains("volume"), "ctrl help should list volume");
    assert!(text.contains("menu"), "ctrl help should list menu");
}

/// `--help` に内部向けの日本語コメントが漏れていないこと。
/// clap は doc comment をそのままヘルプに出すので、理由を `///` に書くと
/// 利用者の画面に出てしまう
#[test]
fn help_is_free_of_internal_commentary() {
    let text = stdout(&run(&["--help"]));
    let has_japanese = text
        .chars()
        .any(|c| matches!(c as u32, 0x3040..=0x30FF | 0x4E00..=0x9FFF));
    assert!(!has_japanese, "internal notes leaked into --help:\n{text}");
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

        // 引数パーサ由来のエラーも GNU の `program: message` に揃える。
        // 自前のエラーと形が違うと、呼び出し側が 2 通りの書式を相手にする
        let text = stderr(&out);
        assert!(
            text.starts_with("stackchan: "),
            "args {args:?} did not use the GNU format: {text}"
        );
    }
}

/// 診断は「何が足りないか」を言わなければ意味がない。描画の 1 行目を機械的に
/// 取ると、サブコマンド未指定はコマンドの説明文が、必須引数の不足は引数名を
/// 落とした文が出てしまう
#[test]
fn usage_diagnostics_say_what_is_actually_wrong() {
    let out = run(&["ctrl"]);
    assert_eq!(out.status.code(), Some(2));
    let text = stderr(&out);
    assert!(
        text.starts_with("stackchan: missing command"),
        "a missing subcommand should say so, got: {text}"
    );

    let out = Command::new(env!("CARGO_BIN_EXE_stackchan"))
        .args(["ctrl", "volume", "--host", "127.0.0.1"])
        .output()
        .expect("failed to run the stackchan binary");
    assert_eq!(out.status.code(), Some(2));
    let text = stderr(&out);
    assert!(
        text.contains("LEVEL"),
        "the missing argument must be named, got: {text}"
    );
}

/// マスクの上位 4bit は firmware が落とすので、判定・出力・送信で同じ
/// 正規化済みの値を使う。分かれていると `pins set ffffffffffffffff` が
/// 「全ピン正常」を送りながら reset を送らず、出力にも嘘のマスクが出る
#[test]
fn pin_masks_are_normalised_before_they_are_reported() {
    let out = Command::new(env!("CARGO_BIN_EXE_stackchan"))
        .args([
            "pins",
            "set",
            "ffffffffffffffff",
            "--host",
            "127.0.0.1",
            "--json",
        ])
        .output()
        .expect("failed to run the stackchan binary");

    let text = stdout(&out);
    assert!(
        text.contains("\"mask\":\"0fffffffffffffff\""),
        "the reported mask must be the one actually sent: {text}"
    );
    // 全ピン正常なので reseat 扱いになり、リセットも送られる
    assert!(text.contains("\"reset\":true"), "got: {text}");
}

/// 自前のエラーは GNU の `program: message` 書式で stderr に出す
#[test]
fn our_own_errors_use_the_gnu_message_format() {
    // --host も STACKCHAN_HOST も無い状態にする
    let out = Command::new(env!("CARGO_BIN_EXE_stackchan"))
        .args(["ctrl", "reset"])
        .env_remove("STACKCHAN_HOST")
        .output()
        .expect("failed to run the stackchan binary");

    assert_eq!(out.status.code(), Some(2));
    assert!(stdout(&out).is_empty());
    let text = stderr(&out);
    assert!(text.starts_with("stackchan: "), "got: {text}");
    assert!(
        text.contains("--host"),
        "the message should say how to fix it"
    );
}

/// 値域の違反は使用法エラー (2)。デバイスに送る前に弾く
#[test]
fn out_of_range_values_are_a_usage_error() {
    let out = Command::new(env!("CARGO_BIN_EXE_stackchan"))
        .args(["ctrl", "volume", "999", "--host", "127.0.0.1"])
        .output()
        .expect("failed to run the stackchan binary");
    assert_eq!(out.status.code(), Some(2));

    for pin in ["0", "61"] {
        let out = Command::new(env!("CARGO_BIN_EXE_stackchan"))
            .args(["pins", "break", pin, "--host", "127.0.0.1"])
            .output()
            .expect("failed to run the stackchan binary");
        assert_eq!(out.status.code(), Some(2), "pin {pin}");
    }
}

/// `--json` を付けたらエラーも stdout に JSON で出す。成功だけ JSON、失敗だけ
/// テキストだと、呼び出し側が 2 通りのパーサを持つことになる
#[test]
fn json_mode_reports_errors_as_json_too() {
    let out = Command::new(env!("CARGO_BIN_EXE_stackchan"))
        .args(["ctrl", "reset", "--json"])
        .env_remove("STACKCHAN_HOST")
        .output()
        .expect("failed to run the stackchan binary");

    assert_eq!(out.status.code(), Some(2), "the exit code must not change");
    let text = stdout(&out);
    assert!(text.contains("\"ok\":false"), "got: {text}");
    assert!(text.contains("\"exit\":2"), "got: {text}");
    // 人が見る stderr にも同じことを書く
    assert!(stderr(&out).starts_with("stackchan: "));
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
        help_first.contains("Usage: stackchan"),
        "--help --version should print the help, got: {help_first}"
    );
}

/// `--` は解析を止めるだけで、後続を捨てはしない。捨てていると
/// 「コマンドが無い」と誤診断され、段階1 以降のディスパッチが成立しない
#[test]
fn the_terminator_keeps_operands_as_commands() {
    let out = run(&["--", "frobnicate"]);
    assert_eq!(out.status.code(), Some(2));
    // 文言はパーサ実装のものなので、名前が診断に現れることだけを見る
    assert!(
        stderr(&out).contains("frobnicate"),
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

    let out = run(&["--", "-x.nes"]);
    assert_eq!(out.status.code(), Some(2));
    assert!(stderr(&out).contains("-x.nes"), "got: {}", stderr(&out));
}

/// `--` の先はサブコマンドの値として届く。届かないと `sd rm -- -x.nes` の
/// ような、ハイフン始まりのファイル名を扱う操作が成立しない
#[test]
fn hyphen_leading_values_survive_the_terminator() {
    // pins は数値しか取らないので、値として扱われた結果「数値ではない」で
    // 落ちることを見る (オプション扱いなら別の診断になる)
    let out = Command::new(env!("CARGO_BIN_EXE_stackchan"))
        .args(["pins", "break", "--host", "127.0.0.1", "--", "-5"])
        .output()
        .expect("failed to run the stackchan binary");
    assert_eq!(out.status.code(), Some(2));
    assert!(stderr(&out).contains("-5"), "got: {}", stderr(&out));
}

/// `--timeout` は「1 回の応答をどれだけ待つか」を置き換える。
///
/// 受け付けられるだけでは足りない — 値が transport まで届かないと、
/// オプションはあるのに何も変わらないという最悪の形になる。所要時間で
/// 効果を見る
#[test]
fn the_timeout_option_shortens_the_wait() {
    use std::net::UdpSocket;
    use std::time::Instant;

    // 返事をしないソケットを立てて、そこへ送らせる。
    //
    // TEST-NET-1 (192.0.2.1) だと、既定経路の無いコンテナでは send_to が
    // ENETUNREACH で即座に失敗し、締切を待たずに exit 1 になる。ここで
    // 見たいのは待ち時間なので、宛先は必ず届いて必ず黙っている先にする
    let silent = UdpSocket::bind(("127.0.0.1", 0)).expect("binding a local socket");
    let port = silent
        .local_addr()
        .expect("the socket has an address")
        .port();

    let started = Instant::now();
    let out = run(&[
        "--host",
        "127.0.0.1",
        "--port",
        &port.to_string(),
        "--timeout",
        "0.5",
        "sd",
        "ls",
    ]);
    let elapsed = started.elapsed();

    assert_eq!(out.status.code(), Some(3), "expected a timeout exit");
    // SD は 3 回試すので 0.5s × 3 = 1.5s 前後。既定 (3s × 3 = 9s) との差は
    // 大きく、取り違えようがない
    assert!(
        elapsed.as_secs_f64() < 4.0,
        "--timeout did not reach the transport: took {elapsed:?}"
    );
}

/// 0 と負数は「待たない」になり、応答を待つコマンドが必ず失敗する。
/// 無限大や NaN は `Duration::from_secs_f64` がパニックする
#[test]
fn the_timeout_option_rejects_values_it_cannot_honour() {
    for bad in ["0", "-1", "abc", "inf", "NaN", "1e9"] {
        let out = run(&["--host", "192.0.2.1", "--timeout", bad, "sd", "ls"]);
        assert_eq!(
            out.status.code(),
            Some(2),
            "--timeout {bad} was not refused: {}",
            stderr(&out)
        );
    }
}

/// 読み手が先に閉じたパイプで panic しない。
///
/// Rust は起動時に SIGPIPE を無視するので、既定のままだと `println!` /
/// `eprintln!` が書き込みエラーで panic し、**exit 101** になる。101 は
/// 終了コード規約の外の値で、`--help` に書いた契約を破る。
///
/// `head` で打ち切ったときは SIGPIPE (141) で死ぬのが Unix のツールとして
/// 正しい。101 でなければよいので、成功 (0) も 141 も受ける
#[test]
fn a_closed_pipe_does_not_panic() {
    use std::net::UdpSocket;
    use std::process::Stdio;

    // 返事をしないソケットへ送らせる (経路の無い環境で即座に失敗しないように)
    let silent = UdpSocket::bind(("127.0.0.1", 0)).expect("binding a local socket");
    let port = silent
        .local_addr()
        .expect("the socket has an address")
        .port();

    // -vv のトレースはパイプへ流れ続ける。読み手が読まずに閉じるので EPIPE を踏む
    let mut child = Command::new(env!("CARGO_BIN_EXE_stackchan"))
        .args([
            "-vv",
            "--host",
            "127.0.0.1",
            "--port",
            &port.to_string(),
            "--timeout",
            "1",
            "sd",
            "ls",
        ])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("failed to spawn the stackchan binary");

    // 読まずに閉じる。以降の書き込みは EPIPE になる
    drop(child.stdout.take());
    drop(child.stderr.take());

    let status = child.wait().expect("failed to wait for the child");
    let code = status.code();
    assert_ne!(
        code,
        Some(101),
        "a closed pipe made the process panic; SIGPIPE is probably still ignored"
    );
}
