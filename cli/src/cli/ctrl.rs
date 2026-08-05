//! `stackchan ctrl` — 本体制御 (type 2)。
//!
//! どれも firmware が ACK を返さない片道の操作。届いたかどうかは確認できない
//! (UDP なので確認する術がなく、firmware 側も応答を返さない)。成功の報告は
//! 「送った」であって「効いた」ではないので、出力もそう書く。

use clap::Subcommand;

use stackchan::exit::ExitCode;
use stackchan::proto::ctrl;

use super::{CommandResult, GlobalArgs};

#[derive(Subcommand, Debug)]
pub enum CtrlCommand {
    /// Press RESET: restart from the reset vector, keeping work RAM
    Reset,

    /// Set the speaker volume (128 is the device default)
    Volume {
        /// 0-255
        level: u16,
    },

    /// Open the ROM menu (only meaningful while a game is running)
    Menu,
}

pub fn run(command: &CtrlCommand, global: &GlobalArgs) -> CommandResult {
    let mut device = global.device()?;
    let output = global.output();
    let seq = device.next_seq();

    let (packet, text, json) = match command {
        CtrlCommand::Reset => (
            ctrl::reset(seq),
            "sent reset".to_string(),
            format!(
                "{{\"ok\":true,\"sent\":\"reset\",\"host\":{}}}",
                quote_host(&device)
            ),
        ),
        CtrlCommand::Volume { level } => {
            // 値域は clap では表現しにくい (u8 にすると 256 が
            // 「invalid value」ではなく「out of range」になり、メッセージが
            // 数値型の話になってしまう) ので、ここで見る
            let is_in_range = *level <= 255;
            if !is_in_range {
                return Err((
                    format!("volume must be between 0 and 255, got {level}"),
                    ExitCode::Usage,
                ));
            }
            let level = *level as u8;
            (
                ctrl::volume(seq, level),
                format!("sent volume {level}"),
                format!(
                    "{{\"ok\":true,\"sent\":\"volume\",\"volume\":{level},\"host\":{}}}",
                    quote_host(&device)
                ),
            )
        }
        CtrlCommand::Menu => (
            ctrl::menu(seq),
            "sent menu".to_string(),
            format!(
                "{{\"ok\":true,\"sent\":\"menu\",\"host\":{}}}",
                quote_host(&device)
            ),
        ),
    };

    device
        .send(&packet)
        .map_err(|e| (format!("{e}"), e.exit_code()))?;
    output.success(&text, &json);
    Ok(())
}

fn quote_host(device: &stackchan::transport::Device) -> String {
    stackchan::output::quote(device.host())
}
