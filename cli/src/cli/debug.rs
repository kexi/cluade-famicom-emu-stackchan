//! `stackchan debug` — 実機の内部状態を覗く (type 3)。
//!
//! 読み取りに副作用は無い。`$2000-$401F` は 0 として返る (`$2002` を読むと
//! vblank がクリアされる等、観測が対象を変えてしまうため)。

use clap::Subcommand;

use stackchan::debug_client::{self, Registers};
use stackchan::output::quote;
use stackchan::proto::debug::{Snapshot, layout};

use super::{CommandResult, GlobalArgs};

#[derive(Subcommand, Debug)]
pub enum DebugCommand {
    /// Show the CPU and APU state
    Snapshot {
        /// Also fetch the APU scope rows
        ///
        /// Asking for these arms the device's per-sample capture for two
        /// seconds, so leave it off unless the waveforms are wanted.
        #[arg(long)]
        waves: bool,

        /// Write the raw snapshot bytes to standard output
        #[arg(long, conflicts_with = "waves")]
        raw: bool,
    },

    /// Dump work RAM
    Wram {
        /// First byte to show (decimal, or 0x-prefixed hex)
        #[arg(long, default_value = "0", value_parser = parse_offset)]
        offset: usize,

        /// How many bytes to show
        #[arg(long, default_value_t = 256)]
        length: usize,
    },
}

/// WRAM の大きさは仕様で決まっている (`layout::WRAM`)
const WRAM_SIZE: usize = layout::WRAM.1;

pub fn run(command: &DebugCommand, global: &GlobalArgs) -> CommandResult {
    // 範囲外の offset は送る前に判る。デバイスに要求してから断ると、
    // 無応答の機体に対して「使用法エラー」ではなく「タイムアウト」が返る
    if let DebugCommand::Wram { offset, .. } = command {
        let is_out_of_range = *offset >= WRAM_SIZE;
        if is_out_of_range {
            return Err((
                format!("offset {offset} is past the end of work RAM ({WRAM_SIZE} bytes)"),
                stackchan::exit::ExitCode::Usage,
            ));
        }
    }

    let mut device = global.device()?;
    let output = global.output();

    let wants_waves = matches!(command, DebugCommand::Snapshot { waves: true, .. });
    let (snapshot, registers) = debug_client::snapshot(&mut device, wants_waves)
        .map_err(|e| (format!("{e}"), e.exit_code()))?;

    match command {
        DebugCommand::Snapshot { raw: true, .. } => {
            // 線に載っていたのと同じ並びで出す。パイプの先が
            // `m5stack/README.md` のレイアウト表どおりに読めるように
            // (CPU 12 | APU 24 | PC 2 | コード窓 48 | WRAM 2048 = 2134)
            use std::io::Write;
            let mut stdout = std::io::stdout();
            let written = stdout
                .write_all(&snapshot.cpu_regs)
                .and_then(|()| stdout.write_all(&snapshot.apu_regs))
                .and_then(|()| stdout.write_all(&snapshot.pc.to_le_bytes()))
                .and_then(|()| stdout.write_all(&snapshot.code_window))
                .and_then(|()| stdout.write_all(&snapshot.wram))
                .and_then(|()| stdout.flush());
            // 書けなかったのを黙って成功にすると、切れた出力を掴んだ側が
            // 気づけない
            written.map_err(|e| {
                (
                    format!("cannot write the snapshot: {e}"),
                    stackchan::exit::ExitCode::Failure,
                )
            })?;
        }
        DebugCommand::Snapshot { waves, .. } => {
            output.success(
                &render_snapshot(&snapshot, &registers, *waves),
                &snapshot_json(&snapshot, &registers, *waves),
            );
        }
        DebugCommand::Wram { offset, length } => {
            // 範囲は上で検査済み。届いた WRAM が仕様どおりの長さであることは
            // `parse_snapshot` が保証している
            let end = offset.saturating_add(*length).min(snapshot.wram.len());
            let window = &snapshot.wram[*offset..end];
            output.success(&hex_dump(window, *offset), &wram_json(window, *offset));
        }
    }
    Ok(())
}

/// `0x300` と `768` のどちらも受ける
fn parse_offset(value: &str) -> std::result::Result<usize, String> {
    let lowered = value.to_ascii_lowercase();
    let Some(hex) = lowered.strip_prefix("0x") else {
        return value
            .parse()
            .map_err(|_| format!("'{value}' is not a number"));
    };
    usize::from_str_radix(hex, 16).map_err(|_| format!("'{value}' is not a hex number"))
}

fn render_snapshot(snapshot: &Snapshot, registers: &Registers, waves: bool) -> String {
    let mut lines = vec![
        format!(
            "PC={:04X}  A={:02X} X={:02X} Y={:02X} SP={:02X}  P={:02X} [{}]",
            registers.pc,
            registers.a,
            registers.x,
            registers.y,
            registers.sp,
            registers.p,
            registers.flags()
        ),
        format!("frame={}", registers.frame),
        String::new(),
        // $4000-$4017 の最後の書き込み値。実機の APU が今どう鳴っているかの手がかり
        format!("APU $4000-$4017: {}", hex_line(&snapshot.apu_regs)),
        format!("code at PC:      {}", hex_line(&snapshot.code_window[..16])),
    ];

    if waves {
        lines.push(String::new());
        let Some(rows) = &snapshot.waves else {
            lines.push("(the device sent no waveforms)".to_string());
            return lines.join("\n");
        };
        for (row, samples) in rows.iter().enumerate() {
            let label = layout::WAVE_LABELS.get(row).copied().unwrap_or("?");
            // 波形そのものは端末では読めないので、目安になる数字だけ出す
            let peak = samples.iter().copied().max().unwrap_or(0);
            let mean = samples.iter().map(|s| *s as u32).sum::<u32>() / samples.len() as u32;
            lines.push(format!(
                "{label:<4} {} samples, peak={peak:3}, mean={mean:3}",
                samples.len()
            ));
        }
    }
    lines.join("\n")
}

fn snapshot_json(snapshot: &Snapshot, registers: &Registers, waves: bool) -> String {
    let mut json = format!(
        concat!(
            "{{\"ok\":true,\"cpu\":{{\"pc\":{},\"a\":{},\"x\":{},\"y\":{},",
            "\"sp\":{},\"p\":{},\"flags\":{},\"frame\":{}}}"
        ),
        registers.pc,
        registers.a,
        registers.x,
        registers.y,
        registers.sp,
        registers.p,
        quote(&registers.flags()),
        registers.frame
    );
    json.push_str(&format!(",\"apu\":[{}]", number_list(&snapshot.apu_regs)));
    json.push_str(&format!(
        ",\"code\":[{}]",
        number_list(&snapshot.code_window)
    ));

    let should_include_waves = waves && snapshot.waves.is_some();
    if should_include_waves {
        let rows = snapshot.waves.as_ref().expect("checked above");
        let rendered: Vec<String> = rows
            .iter()
            .enumerate()
            .map(|(i, samples)| {
                format!(
                    "{{\"name\":{},\"samples\":[{}]}}",
                    quote(layout::WAVE_LABELS.get(i).copied().unwrap_or("?")),
                    number_list(samples)
                )
            })
            .collect();
        json.push_str(&format!(",\"waves\":[{}]", rendered.join(",")));
    }
    json.push('}');
    json
}

fn wram_json(window: &[u8], offset: usize) -> String {
    format!(
        "{{\"ok\":true,\"offset\":{offset},\"length\":{},\"bytes\":[{}]}}",
        window.len(),
        number_list(window)
    )
}

fn number_list(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|b| b.to_string())
        .collect::<Vec<_>>()
        .join(",")
}

fn hex_line(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|b| format!("{b:02X}"))
        .collect::<Vec<_>>()
        .join(" ")
}

/// 16 バイトずつ、アドレスと ASCII を添えて並べる
fn hex_dump(bytes: &[u8], base: usize) -> String {
    bytes
        .chunks(16)
        .enumerate()
        .map(|(row, chunk)| {
            let address = base + row * 16;
            let hex = hex_line(chunk);
            let ascii: String = chunk
                .iter()
                .map(|b| {
                    let is_printable = b.is_ascii_graphic() || *b == b' ';
                    if is_printable { *b as char } else { '.' }
                })
                .collect();
            format!("{address:04X}  {hex:<47}  {ascii}")
        })
        .collect::<Vec<_>>()
        .join("\n")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn registers() -> Registers {
        Registers {
            pc: 0x8123,
            a: 0xAA,
            x: 0x01,
            y: 0x02,
            sp: 0xFD,
            p: 0x24,
            frame: 1234,
        }
    }

    fn snapshot() -> Snapshot {
        Snapshot {
            cpu_regs: vec![0; 12],
            apu_regs: vec![0x0F; 24],
            pc: 0x8123,
            code_window: vec![0xEA; 48],
            wram: (0..2048).map(|i| (i % 256) as u8).collect(),
            waves: None,
        }
    }

    #[test]
    fn the_summary_shows_the_registers_in_hex() {
        let text = render_snapshot(&snapshot(), &registers(), false);
        assert!(text.contains("PC=8123"), "got: {text}");
        assert!(text.contains("A=AA"), "got: {text}");
        assert!(text.contains("SP=FD"), "got: {text}");
        // フラグは読みやすい形で
        assert!(text.contains("[nv-bdIzc]"), "got: {text}");
        assert!(text.contains("frame=1234"), "got: {text}");
    }

    #[test]
    fn waves_are_summarised_rather_than_dumped() {
        let with_waves = Snapshot {
            waves: Some(vec![vec![100; 280]; 6]),
            ..snapshot()
        };
        let text = render_snapshot(&with_waves, &registers(), true);
        // 280 個の数字を並べても端末では読めない
        assert!(text.contains("280 samples"), "got: {text}");
        assert!(text.contains("peak=100"), "got: {text}");
        assert!(text.contains("P1"), "got: {text}");
        assert!(text.contains("MIX"), "got: {text}");
    }

    #[test]
    fn json_carries_the_numbers_for_a_program_to_read() {
        let json = snapshot_json(&snapshot(), &registers(), false);
        assert!(json.contains("\"pc\":33059"), "got: {json}");
        assert!(json.contains("\"flags\":\"nv-bdIzc\""), "got: {json}");
        assert!(json.contains("\"frame\":1234"), "got: {json}");
        // 波形を頼んでいなければ載せない
        assert!(!json.contains("\"waves\""), "got: {json}");
    }

    #[test]
    fn a_hex_dump_shows_addresses_and_ascii() {
        let text = hex_dump(b"Hello, world!\x00\x01\x02", 0x300);
        assert!(text.starts_with("0300  "), "got: {text}");
        assert!(text.contains("48 65 6C 6C 6F"), "got: {text}");
        // 表示できないバイトは . にする
        assert!(text.contains("Hello, world!..."), "got: {text}");
    }

    #[test]
    fn offsets_accept_decimal_and_hex() {
        assert_eq!(parse_offset("768"), Ok(768));
        assert_eq!(parse_offset("0x300"), Ok(768));
        assert_eq!(parse_offset("0X300"), Ok(768));
        assert!(parse_offset("nope").is_err());
    }
}
