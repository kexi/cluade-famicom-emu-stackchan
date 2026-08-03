//! `stackchan rom` — カートリッジイメージの転送 (type 4)。
//!
//! URL からの取得は持たない。TLS スタックを抱えるとクロスビルドが厄介になる
//! うえ、信頼ストアが CLI に固定されて古びる。`curl` は macOS にも Linux にも
//! 標準で入っていて OS の信頼ストアを使うので、パイプで繋ぐほうが正しい:
//!
//! ```text
//! curl -fsSL "$URL" | stackchan rom send - --save game.nes
//! ```

use std::io::{IsTerminal, Read, Write};

use clap::Args;

use stackchan::exit::ExitCode;
use stackchan::output::quote;
use stackchan::proto::constants::ROM_MAX_SIZE;
use stackchan::proto::rom::RomOptions;
use stackchan::rom_client;

use super::{CommandResult, GlobalArgs};

#[derive(Args, Debug)]
pub struct RomSendArgs {
    /// Path to a .nes image, or "-" to read it from standard input
    pub path: String,

    /// Also write the image to the SD card under this name
    ///
    /// If the card write fails the image is not installed either, so a
    /// running game keeps going rather than being replaced by a cart that
    /// is not on the card.
    #[arg(long, value_name = "NAME")]
    pub save: Option<String>,

    /// Store the image without booting it, leaving the running game alone
    ///
    /// Requires --save: without somewhere to put the image, this would do
    /// nothing at all.
    #[arg(long)]
    pub no_load: bool,

    /// Swap the cart without resetting the CPU
    ///
    /// The classic swap-with-the-power-on trick. Ignored with --no-load.
    #[arg(long)]
    pub swap: bool,

    /// Report progress while the image is sent
    #[arg(long)]
    pub progress: bool,
}

pub fn run(args: &RomSendArgs, global: &GlobalArgs) -> CommandResult {
    let options = RomOptions {
        swap: args.swap,
        save_as: args.save.clone(),
        no_load: args.no_load,
    };

    // イメージを読む前に、データと無関係な指定の不備を弾く。終わらない
    // パイプに `rom send - --no-load` を繋いだとき、使用法エラーと判って
    // いるのに読み終わるまで返らないのはおかしい
    check_options(&options)?;

    let data = read_image(&args.path)?;
    let mut device = global.device()?;
    let output = global.output();

    // 進捗は stderr に出す。stdout は結果のためのもので、`--json` を付けた
    // 呼び出しがパースする先でもある
    let wants_progress = args.progress && !global.quiet;
    let mut last_shown = usize::MAX;
    let mut report = |sent: usize, total: usize| {
        let percent = if total == 0 { 100 } else { sent * 100 / total };
        // 1% ごとに間引く。1400 バイトごとに書くと出力が溢れる
        let is_worth_showing = percent != last_shown;
        if !is_worth_showing {
            return;
        }
        last_shown = percent;
        let mut stderr = std::io::stderr();
        let _ = write!(stderr, "\rsending... {percent}% ({sent}/{total} chunks)");
        let _ = stderr.flush();
    };

    let result = rom_client::send(
        &mut device,
        &data,
        &options,
        if wants_progress {
            Some(&mut report)
        } else {
            None
        },
    );

    // 進捗の行は成否によらず閉じる。失敗のときだけ閉じそこねると、
    // 途中の行にエラーが続いて `\rsending... 33%stackchan: ...` になり、
    // GNU の `program: message` 書式が崩れる
    if wants_progress {
        let _ = writeln!(std::io::stderr());
    }

    let transfer = result.map_err(|e| (format!("{e}"), e.exit_code()))?;

    // 保存を頼んだのに結果が判らない / 失敗しているなら、そう報告する。
    // 転送そのものは通っていても、求められたのは「カードに置くこと」
    let is_ok = transfer.is_ok();
    if !is_ok {
        let (message, code) = match &transfer.saved {
            Some(Some(status)) => (
                format!("the image was sent but not saved: {status}"),
                ExitCode::Failure,
            ),
            _ => (
                "the image was sent but the device did not report whether it was saved".to_string(),
                ExitCode::Unknown,
            ),
        };
        return Err((message, code));
    }

    output.success(&render(&transfer, args), &render_json(&transfer, args));
    Ok(())
}

/// イメージの中身に関係なく判る指定の不備を先に見る。
///
/// `proto::rom::begin` も同じことを検査するが、そちらはイメージを読み終えて
/// からしか呼べない。読む前に判るものはここで断る
fn check_options(options: &RomOptions) -> std::result::Result<(), (String, ExitCode)> {
    let is_a_no_op = options.no_load && options.save_as.is_none();
    if is_a_no_op {
        return Err((
            "--no-load without --save would neither install nor store the image".to_string(),
            ExitCode::Usage,
        ));
    }
    if let Some(name) = &options.save_as {
        stackchan::proto::name::validate(name, "save name")
            .map_err(|message| (message, ExitCode::Usage))?;
    }
    Ok(())
}

/// ファイルか標準入力からイメージを読む。
///
/// 上限を超えていないかは読みながら見る。`ROM_MAX_SIZE` + 1 バイト読んだ
/// 時点で判るので、大きすぎるファイルを全部メモリに載せてから断ることはない
fn read_image(path: &str) -> std::result::Result<Vec<u8>, (String, ExitCode)> {
    let is_stdin = path == "-";
    if is_stdin {
        let stdin = std::io::stdin();
        let is_a_terminal = stdin.is_terminal();
        if is_a_terminal {
            return Err((
                "reading a ROM from the terminal; pipe one in or give a path".to_string(),
                ExitCode::Usage,
            ));
        }
        return read_capped(stdin.lock(), "standard input");
    }

    let file = std::fs::File::open(path)
        .map_err(|e| (format!("cannot read '{path}': {e}"), ExitCode::Failure))?;
    read_capped(file, path)
}

fn read_capped(
    mut source: impl Read,
    what: &str,
) -> std::result::Result<Vec<u8>, (String, ExitCode)> {
    let mut data = Vec::new();
    // 上限 + 1 まで読めば、超えているかどうかが判る
    let limit = (ROM_MAX_SIZE + 1) as u64;
    source
        .by_ref()
        .take(limit)
        .read_to_end(&mut data)
        .map_err(|e| (format!("cannot read {what}: {e}"), ExitCode::Failure))?;

    let is_too_big = data.len() > ROM_MAX_SIZE;
    if is_too_big {
        return Err((
            format!("{what} is larger than {ROM_MAX_SIZE} bytes, which is all the device accepts"),
            ExitCode::Usage,
        ));
    }
    let is_empty = data.is_empty();
    if is_empty {
        return Err((format!("{what} is empty"), ExitCode::Usage));
    }
    Ok(data)
}

fn render(transfer: &rom_client::Transfer, args: &RomSendArgs) -> String {
    let mut text = format!(
        "sent {} bytes in {} chunk(s), {} ms",
        transfer.bytes,
        transfer.chunks,
        transfer.elapsed.as_millis()
    );
    let had_trouble = transfer.retries > 0;
    if had_trouble {
        text.push_str(&format!(" ({} retries)", transfer.retries));
    }
    if let Some(name) = &args.save {
        text.push_str(&format!("; saved as {name}"));
    }
    let is_a_swap = args.swap && !args.no_load;
    if is_a_swap {
        text.push_str("; swapped without a reset");
    }
    text
}

fn render_json(transfer: &rom_client::Transfer, args: &RomSendArgs) -> String {
    let saved = match &args.save {
        None => String::new(),
        Some(name) => format!(",\"saved\":{}", quote(name)),
    };
    format!(
        "{{\"ok\":true,\"bytes\":{},\"chunks\":{},\"retries\":{},\"ms\":{}{}}}",
        transfer.bytes,
        transfer.chunks,
        transfer.retries,
        transfer.elapsed.as_millis(),
        saved
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    fn transfer() -> rom_client::Transfer {
        rom_client::Transfer {
            bytes: 40976,
            chunks: 30,
            retries: 0,
            elapsed: Duration::from_millis(120),
            saved: None,
        }
    }

    fn args() -> RomSendArgs {
        RomSendArgs {
            path: "game.nes".to_string(),
            save: None,
            no_load: false,
            swap: false,
            progress: false,
        }
    }

    #[test]
    fn the_summary_reports_what_happened() {
        let text = render(&transfer(), &args());
        assert!(text.contains("40976 bytes"), "got: {text}");
        assert!(text.contains("30 chunk(s)"), "got: {text}");
        assert!(text.contains("120 ms"), "got: {text}");
        // 再送が無ければ触れない (無い情報を出さない)
        assert!(!text.contains("retries"), "got: {text}");
    }

    // 再送があったことは見えるようにする。リンクの状態を知る手がかりになる
    #[test]
    fn retries_are_reported_when_they_happen() {
        let with_retries = rom_client::Transfer {
            retries: 3,
            ..transfer()
        };
        let text = render(&with_retries, &args());
        assert!(text.contains("3 retries"), "got: {text}");
    }

    #[test]
    fn saving_is_mentioned_in_both_forms() {
        let saving = RomSendArgs {
            save: Some("game.nes".to_string()),
            ..args()
        };
        assert!(render(&transfer(), &saving).contains("saved as game.nes"));
        assert!(render_json(&transfer(), &saving).contains("\"saved\":\"game.nes\""));
    }

    #[test]
    fn json_has_the_expected_shape() {
        assert_eq!(
            render_json(&transfer(), &args()),
            r#"{"ok":true,"bytes":40976,"chunks":30,"retries":0,"ms":120}"#
        );
    }

    #[test]
    fn an_empty_image_is_a_usage_error() {
        let err = read_capped(&b""[..], "test").unwrap_err();
        assert_eq!(err.1, ExitCode::Usage);
    }

    // 上限ちょうどは通し、1 バイト超えたら断る
    #[test]
    fn the_size_limit_is_enforced_while_reading() {
        let just_fits = vec![0u8; ROM_MAX_SIZE];
        assert!(read_capped(&just_fits[..], "test").is_ok());

        let one_too_many = vec![0u8; ROM_MAX_SIZE + 1];
        let err = read_capped(&one_too_many[..], "test").unwrap_err();
        assert_eq!(err.1, ExitCode::Usage);
        assert!(err.0.contains("larger than"), "got: {}", err.0);
    }
}
