//! `stackchan` — M5Stack CoreS3 のファミコンエミュレータを操作する CLI。
//!
//! GNU の作法に従う: `--version` の書式、`--help` の EXIT STATUS 節と
//! バグ報告先、エラーは `program: message` で stderr、使用法エラーは exit 2。

mod cli;

use clap::Parser;

use stackchan::exit::ExitCode;

/// パイプの読み手が先に閉じたときに、素直に死ぬようにする。
///
/// Rust の標準ライブラリは起動時に `SIGPIPE` を無視する。ライブラリとして
/// 使われたときに、書き込み失敗をプロセスの死ではなくエラーとして扱えるように
/// するためだが、CLI では裏目に出る — `println!` / `eprintln!` は書き込み
/// エラーで panic するので、`stackchan -vv sd ls 2>&1 | head -1` が **exit 101**
/// になる。101 は終了コード規約 (0/1/2/3/4) の外の値で、`--help` に書いた
/// 契約を破る。
///
/// 既定動作に戻せば、`head` や `less` で打ち切ったときに `grep` や `cat` と
/// 同じく SIGPIPE で死ぬ。これは Unix のツールとして期待される振る舞い。
///
/// `output.rs` の `println!` だけを書き込みエラーに強い形へ変えても足りない。
/// トレース (`transport.rs` の `eprintln!`)、進捗表示 (`rom.rs`)、clap が出す
/// ヘルプなど、書き込む場所は全体に散っている
fn restore_sigpipe() {
    // SAFETY: シグナルハンドラを既定に戻すだけ。main の先頭で、スレッドを
    // 立てる前に 1 回だけ呼ぶ
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_DFL);
    }
}

fn main() {
    restore_sigpipe();

    // `args()` は非 UTF-8 の引数で panic する (Unix のファイル名は任意バイト列を
    // 取りうるので実際に起こる)。panic は終了コード 101 になり、規約の外の値が
    // 呼び出し側に漏れるので、`args_os()` で受けて使用法エラーとして返す
    let mut args = Vec::new();
    for raw in std::env::args_os() {
        match raw.into_string() {
            Ok(arg) => args.push(arg),
            Err(bad) => {
                let program = env!("CARGO_PKG_NAME");
                eprintln!(
                    "{program}: argument is not valid UTF-8: {}",
                    bad.to_string_lossy()
                );
                eprintln!("Try '{program} --help' for more information.");
                std::process::exit(ExitCode::Usage.code());
            }
        }
    }

    // clap の使用法エラーは自前で受ける。既定でも exit 2 だが、書式を
    // `program: message` に揃えるため
    let parsed = match cli::Cli::try_parse_from(&args) {
        Ok(parsed) => parsed,
        Err(e) => {
            // --help / --version は「エラー」として返るが、明示的に求められた
            // 出力なので stdout に出して 0 で抜ける。
            //
            // DisplayHelpOnMissingArgumentOrSubcommand をここに含めないのは、
            // 引数なしの起動は「ヘルプの要求」ではなく使用法エラーだから。
            // 0 で抜けると、スクリプトが「コマンドを渡し忘れた」ことに気づけない
            let is_help_or_version = matches!(
                e.kind(),
                clap::error::ErrorKind::DisplayHelp | clap::error::ErrorKind::DisplayVersion
            );
            if is_help_or_version {
                // 明示的に stdout へ書く。clap の `Error::print()` は種別によって
                // stderr を選ぶことがあるが、GNU は --help / --version を stdout に
                // 出すよう求めている (`stackchan --help | less` が成立しなくなる)
                print!("{e}");
                let _ = std::io::Write::flush(&mut std::io::stdout());
                std::process::exit(ExitCode::Success.code());
            }
            // clap の既定書式は `error: ...` だが、GNU は `program: message` を
            // 求める。自前のエラーと形が揃っていないと、呼び出し側は 2 通りの
            // 書式を相手にすることになる
            let program = env!("CARGO_PKG_NAME");
            eprintln!("{program}: {}", usage_message(&e));
            eprintln!("Try '{program} --help' for more information.");
            std::process::exit(ExitCode::Usage.code());
        }
    };

    std::process::exit(cli::run(parsed).code());
}

/// clap のエラーを 1 行の診断にまとめる。
///
/// 単純に描画の 1 行目を取ると意味を失うものがある:
/// - サブコマンド未指定は 1 行目が**コマンドの説明文**で、エラーに見えない
/// - 必須引数の不足は、足りない引数の名前が次の行にある
///
/// そのため種別ごとに扱う
fn usage_message(error: &clap::Error) -> String {
    use clap::error::ErrorKind;

    match error.kind() {
        ErrorKind::DisplayHelpOnMissingArgumentOrSubcommand => "missing command".to_string(),
        ErrorKind::MissingRequiredArgument | ErrorKind::MissingSubcommand => {
            let rendered = error.render().to_string();
            // 「何が足りないか」は次行以降にあるので、空行までを 1 行にまとめる
            let detail: Vec<&str> = rendered
                .lines()
                .map(|line| line.trim_start_matches("error: ").trim())
                .take_while(|line| !line.is_empty())
                .filter(|line| !line.is_empty())
                .collect();
            let has_detail = !detail.is_empty();
            if has_detail {
                return detail.join(" ");
            }
            "missing a required argument".to_string()
        }
        _ => {
            let rendered = error.render().to_string();
            rendered
                .lines()
                .next()
                .unwrap_or("invalid arguments")
                .trim_start_matches("error: ")
                .to_string()
        }
    }
}
