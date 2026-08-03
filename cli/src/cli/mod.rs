//! コマンド体系の定義とディスパッチ。
//!
//! 名詞→動詞の 2 階層 (`stackchan ctrl reset`)。`stackchan ctrl --help` を
//! 引けばその配下が全部見える形にして、探索できるようにしている。

mod ctrl;
mod debug;
mod discover;
mod pins;
mod rom;
mod sd;

use std::sync::LazyLock;

use clap::{Args, Parser, Subcommand};

use stackchan::exit::{EXIT_STATUS_HELP, ExitCode};
use stackchan::output::Output;
use stackchan::proto::constants::DEFAULT_PORT;
use stackchan::transport::Device;

/// GNU coding standards の `--version` 書式。1 行目がプログラム名とバージョン、
/// 続けて著作権・ライセンス・無保証の告知。
///
/// clap の `version` に渡す文字列は**プログラム名の後ろに置かれる**ので、
/// ここには名前を含めない (含めると `stackchan stackchan 0.1.0` になる)
pub const VERSION_TEXT: &str = concat!(
    env!("CARGO_PKG_VERSION"),
    "\n",
    "Copyright (C) 2026 GOROman\n",
    "License MIT: <https://opensource.org/licenses/MIT>\n",
    "This is free software: you are free to change and redistribute it.\n",
    "There is NO WARRANTY, to the extent permitted by law.",
);

const BUG_REPORT: &str =
    "Report bugs to: <https://github.com/kexi/cluade-famicom-emu-stackchan/issues>";

/// `--help` の末尾。終了コードの一覧は `exit.rs` から借りる — 2 か所に書くと
/// 実装を変えたときにヘルプだけ古くなる。
///
/// `concat!` はリテラルしか取れず、clap は `&'static str` を要求するので、
/// 一度だけ組み立てて borrow する
static AFTER_HELP: LazyLock<String> =
    LazyLock::new(|| format!("{EXIT_STATUS_HELP}\n\n{BUG_REPORT}"));

#[derive(Parser, Debug)]
#[command(
    name = "stackchan",
    version = VERSION_TEXT,
    about = "Control an M5Stack CoreS3 running the Famicom emulator over its UDP protocol.",
    after_help = AFTER_HELP.as_str(),
    disable_help_subcommand = true,
)]
pub struct Cli {
    #[command(flatten)]
    pub global: GlobalArgs,

    #[command(subcommand)]
    pub command: Command,
}

#[derive(Args, Debug, Clone)]
pub struct GlobalArgs {
    // 未指定でブロードキャストに倒さないのは、LAN 上の全機体を同時に動かして
    // しまうため。procon_udp.py の既定は 255.255.255.255 だが、CLI の既定に
    // するには危険すぎる
    /// Device address: an IP or an mDNS name such as stackchan-a1b2c3.local
    #[arg(short = 'H', long, env = "STACKCHAN_HOST", global = true)]
    pub host: Option<String>,

    /// UDP port on the device
    #[arg(short = 'p', long, env = "STACKCHAN_PORT", default_value_t = DEFAULT_PORT, global = true)]
    pub port: u16,

    /// Machine-readable output
    #[arg(long, global = true)]
    pub json: bool,

    /// Suppress output on success
    #[arg(short = 'q', long, global = true)]
    pub quiet: bool,

    /// Trace what is sent and received (-vv adds a hex dump)
    #[arg(short = 'v', long, action = clap::ArgAction::Count, global = true)]
    pub verbose: u8,
}

impl GlobalArgs {
    pub fn output(&self) -> Output {
        Output::new(self.json, self.quiet)
    }

    /// デバイスへの接続を用意する。`--host` は実質必須だが、clap の
    /// `required` にはしていない — `discover` だけは不要なので、
    /// 必要なコマンドがここで要求する形にしている
    pub fn device(&self) -> Result<Device, (String, ExitCode)> {
        let Some(host) = &self.host else {
            return Err((
                "no device address; pass --host or set STACKCHAN_HOST".to_string(),
                ExitCode::Usage,
            ));
        };
        Device::new(host, self.port, self.verbose).map_err(|e| (format!("{e}"), ExitCode::Failure))
    }
}

#[derive(Subcommand, Debug)]
pub enum Command {
    /// Find devices on the local network
    Discover(discover::DiscoverArgs),

    /// List, boot, rename, or delete the ROMs on the SD card
    #[command(subcommand)]
    Sd(sd::SdCommand),

    /// Send a cartridge image to the device
    #[command(subcommand)]
    Rom(RomCommand),

    /// Reset the console, set the volume, or open the ROM menu
    #[command(subcommand)]
    Ctrl(ctrl::CtrlCommand),

    /// Break or restore cartridge connector pins
    #[command(subcommand)]
    Pins(pins::PinsCommand),

    /// Inspect the emulator's internal state
    #[command(subcommand)]
    Debug(debug::DebugCommand),
}

/// `rom` の下のコマンド。
///
/// URL を取りに行く口は持たない。TLS を抱えるとクロスビルドが厄介になり、
/// 信頼ストアも CLI に固定されて古びる。`curl` に任せてパイプで繋ぐほうがよい
#[derive(Subcommand, Debug)]
pub enum RomCommand {
    /// Send a .nes image to the device
    ///
    /// To fetch one from the network, pipe it in:
    ///   curl -fsSL "$URL" | stackchan rom send - --save game.nes
    Send(rom::RomSendArgs),
}

pub fn run(cli: Cli) -> ExitCode {
    let output = cli.global.output();

    let result = match &cli.command {
        Command::Discover(args) => discover::run(args, &cli.global),
        Command::Sd(command) => sd::run(command, &cli.global),
        Command::Rom(RomCommand::Send(args)) => rom::run(args, &cli.global),
        Command::Ctrl(command) => ctrl::run(command, &cli.global),
        Command::Pins(command) => pins::run(command, &cli.global),
        Command::Debug(command) => debug::run(command, &cli.global),
    };

    match result {
        Ok(()) => ExitCode::Success,
        Err((message, code)) => output.failure(&message, code),
    }
}

/// サブコマンドの実行結果。失敗はメッセージと終了コードの組で返す
pub type CommandResult = Result<(), (String, ExitCode)>;

#[cfg(test)]
mod tests {
    use super::*;

    // clap がプログラム名を前置するので、ここはバージョンから始まる。
    // 名前まで入れると `stackchan stackchan 0.1.0` になる
    #[test]
    fn version_text_follows_the_gnu_layout() {
        let mut lines = VERSION_TEXT.lines();
        assert_eq!(lines.next(), Some(env!("CARGO_PKG_VERSION")));
        assert!(
            lines
                .next()
                .is_some_and(|l| l.starts_with("Copyright (C) "))
        );
        assert!(lines.next().is_some_and(|l| l.starts_with("License MIT:")));
        assert!(VERSION_TEXT.contains("NO WARRANTY"));
    }

    // --help の EXIT STATUS が実装と食い違うと、ヘルプが嘘をつく
    #[test]
    fn after_help_documents_every_exit_code() {
        for code in ["  0  ", "  1  ", "  2  ", "  3  ", "  4  "] {
            assert!(AFTER_HELP.contains(code), "missing {code:?}");
        }
        assert!(AFTER_HELP.contains("Report bugs to:"));
        // exit.rs の一覧と同じ内容であること
        assert!(AFTER_HELP.contains(EXIT_STATUS_HELP.lines().nth(1).unwrap()));
    }

    #[test]
    fn the_command_tree_parses() {
        use clap::CommandFactory;
        Cli::command().debug_assert();
    }
}
