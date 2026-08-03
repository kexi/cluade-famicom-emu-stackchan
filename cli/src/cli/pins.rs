//! `stackchan pins` — カセット端子の導通状態 (type 1)。
//!
//! **デバイスはマスクの読み出し API を持たない。** よって CLI は現在値を
//! 知りえず、`break` / `fix` は「今の状態からの差分」ではなく **全ピン正常からの
//! 差分**としてしか定義できない。累積したいなら `set` に完全な値を渡す。
//!
//! パッドと違い、ピンの状態はタイムアウトしない。接触不良は物理的な状態で、
//! 送信side が止まっても勝手に直るものではないため。

use clap::Subcommand;

use stackchan::exit::ExitCode;
use stackchan::output::quote;
use stackchan::proto::constants::{CTRL_RESET, PIN_MASK_ALL_OK, PIN_MASK_VALID};
use stackchan::proto::{ctrl, pins};

use super::{CommandResult, GlobalArgs};

#[derive(Subcommand, Debug)]
pub enum PinsCommand {
    /// Set the whole mask: a hex value, or "all" / "none"
    Set {
        /// Up to 16 hex digits (0x prefix optional), "all", or "none".
        /// Bit n-1 is pin n; only the low 60 bits are meaningful
        mask: String,
    },

    /// Break the listed pins, leaving the rest connected
    ///
    /// The device cannot be asked for its current mask, so this is always
    /// relative to a fully seated cartridge, not to whatever is set now.
    Break {
        /// Pin numbers, 1-60 (comma-separated or repeated)
        #[arg(required = true, value_delimiter = ',')]
        pins: Vec<u8>,
    },

    /// Connect only the listed pins, breaking every other one
    Fix {
        /// Pin numbers, 1-60 (comma-separated or repeated)
        #[arg(required = true, value_delimiter = ',')]
        pins: Vec<u8>,
    },
}

pub fn run(command: &PinsCommand, global: &GlobalArgs) -> CommandResult {
    let requested = match command {
        PinsCommand::Set { mask } => parse_mask(mask)?,
        PinsCommand::Break { pins } => pins::break_pins(pins).map_err(|e| (e, ExitCode::Usage))?,
        PinsCommand::Fix { pins } => pins::only_pins(pins).map_err(|e| (e, ExitCode::Usage))?,
    };

    // 上位 4bit は firmware が落とすので、判定・出力・送信のすべてで同じ
    // 正規化済みの値を使う。ここを分けると `pins set ffffffffffffffff` が
    // 「全ピン正常」を送りながら reset を送らず、出力にも嘘のマスクが出る
    let mask = requested & PIN_MASK_VALID;

    let mut device = global.device()?;
    let output = global.output();

    let seq = device.next_seq();
    device
        .send(&pins::mask(seq, mask))
        .map_err(|e| (format!("{e}"), e.exit_code()))?;

    // 全ピンを戻すときはリセットも送る。CPU バス系のピンを抜くと
    // エミュレート中のプログラムが暴走することがあり、端子を戻すだけでは
    // 復帰しない (リセットベクタを読み直す必要がある)。ブラウザの
    // 「まっすぐ挿す」が同じことをしている
    let is_a_full_reseat = mask == PIN_MASK_ALL_OK;
    if is_a_full_reseat {
        let seq = device.next_seq();
        device
            .send(&ctrl::command(seq, CTRL_RESET, 0))
            .map_err(|e| (format!("{e}"), e.exit_code()))?;
    }

    let broken = pins::broken_pins(mask);
    let text = if broken.is_empty() {
        "all 60 pins connected (reset sent too)".to_string()
    } else {
        let list: Vec<String> = broken.iter().map(|p| p.to_string()).collect();
        format!("{} pin(s) broken: {}", broken.len(), list.join(", "))
    };
    let json = format!(
        "{{\"ok\":true,\"mask\":\"{mask:016x}\",\"broken\":[{}],\"reset\":{},\"host\":{}}}",
        broken
            .iter()
            .map(|p| p.to_string())
            .collect::<Vec<_>>()
            .join(","),
        is_a_full_reseat,
        quote(device.host())
    );
    output.success(&text, &json);
    Ok(())
}

/// `set` の引数を解釈する
fn parse_mask(value: &str) -> Result<u64, (String, ExitCode)> {
    let lowered = value.to_ascii_lowercase();
    if lowered == "all" {
        return Ok(PIN_MASK_ALL_OK);
    }
    if lowered == "none" {
        return Ok(0);
    }

    let digits = lowered.strip_prefix("0x").unwrap_or(&lowered);
    u64::from_str_radix(digits, 16).map_err(|_| {
        (
            format!("'{value}' is not a hex mask, \"all\", or \"none\""),
            ExitCode::Usage,
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mask_keywords_are_recognised() {
        assert_eq!(parse_mask("all").unwrap(), PIN_MASK_ALL_OK);
        assert_eq!(parse_mask("ALL").unwrap(), PIN_MASK_ALL_OK);
        assert_eq!(parse_mask("none").unwrap(), 0);
    }

    #[test]
    fn hex_masks_are_accepted_with_or_without_a_prefix() {
        assert_eq!(parse_mask("ff").unwrap(), 0xFF);
        assert_eq!(parse_mask("0xFF").unwrap(), 0xFF);
        assert_eq!(
            parse_mask("0fffffffffffffff").unwrap(),
            0x0FFF_FFFF_FFFF_FFFF
        );
    }

    #[test]
    fn nonsense_masks_are_a_usage_error() {
        for bad in ["", "xyz", "0x", "12g4"] {
            let err = parse_mask(bad).unwrap_err();
            assert_eq!(err.1, ExitCode::Usage, "input {bad:?}");
        }
    }
}
