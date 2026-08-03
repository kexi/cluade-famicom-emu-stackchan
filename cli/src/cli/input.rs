//! `stackchan input` — コントローラ入力 (type 0)。
//!
//! **`send` が対話端末を持たない呼び出し側の入口。** スクリプトや AI は
//! `keys` のような対話モードを使えないので、押すボタンと長さを引数で渡せる
//! 形が要る。
//!
//! `--script` は `tools/scenario-sample.txt` と同じ書式を読む。`just verify`
//! のシナリオがそのまま実機に流せる。

use std::io::Read;
use std::time::Duration;

use clap::{Args, Subcommand};

use stackchan::exit::ExitCode;
use stackchan::input_client::{self, DEFAULT_RATE_HZ, MAX_HOLD, MAX_RATE_HZ, MIN_RATE_HZ, Step};
use stackchan::output::quote;
use stackchan::proto::pad;

use super::{CommandResult, GlobalArgs};

#[derive(Subcommand, Debug)]
pub enum InputCommand {
    /// Press buttons for a while, then release them
    ///
    /// Buttons are A, B, SELECT, START, UP, DOWN, LEFT and RIGHT, joined with
    /// "+" for a chord. NONE or 0 releases everything.
    Send(SendArgs),

    /// Send a fixed sequence of presses, to check the link works
    TestPattern {
        /// How long to hold each state, in milliseconds
        #[arg(long, default_value_t = 100)]
        hold: u64,
    },
}

#[derive(Args, Debug)]
pub struct SendArgs {
    /// Buttons to press, e.g. A, START, or A+RIGHT
    ///
    /// Each is pressed for --hold and then released, in order.
    #[arg(conflicts_with = "script")]
    pub buttons: Vec<String>,

    /// Read a scenario instead: a file of "<frame> <buttons>" lines, or "-"
    /// for standard input
    ///
    /// This is the format used by tools/scenario-sample.txt, so a scenario
    /// written for `just verify` can be replayed on the device.
    #[arg(long, value_name = "FILE")]
    pub script: Option<String>,

    /// How long to hold each press, in milliseconds
    #[arg(long, default_value_t = 100)]
    pub hold: u64,

    /// How often to repeat the state while held, in Hz
    ///
    /// The device releases the pad after 500 ms of silence, so a held button
    /// has to be resent. Lower rates are gentler on the network but risk a
    /// dropped packet letting go early.
    #[arg(long, default_value_t = DEFAULT_RATE_HZ)]
    pub rate: u32,
}

pub fn run(command: &InputCommand, global: &GlobalArgs) -> CommandResult {
    match command {
        InputCommand::Send(args) => send(args, global),
        InputCommand::TestPattern { hold } => test_pattern(*hold, global),
    }
}

fn send(args: &SendArgs, global: &GlobalArgs) -> CommandResult {
    check_rate(args.rate)?;
    check_hold(args.hold)?;

    // 送る中身を先に決める。指定の不備はデバイスに触る前に判る
    let steps = plan(args)?;
    let is_empty = steps.is_empty();
    if is_empty {
        return Err((
            "nothing to send; give some buttons or a --script".to_string(),
            ExitCode::Usage,
        ));
    }

    let mut device = global.device()?;
    let output = global.output();
    let hold = Duration::from_millis(args.hold);

    // シナリオも引数も同じ再生器に渡す。時刻は開始からの絶対位置なので、
    // 行が増えても後ろがずれない
    input_client::play(&mut device, &steps, hold, args.rate)
        .map_err(|e| (format!("{e}"), e.exit_code()))?;

    let names: Vec<String> = steps
        .iter()
        .map(|s| pad::combo_to_names(s.buttons))
        .collect();
    output.success(
        &format!("sent {} press(es): {}", steps.len(), names.join(", ")),
        &format!(
            "{{\"ok\":true,\"presses\":[{}]}}",
            names.iter().map(|n| quote(n)).collect::<Vec<_>>().join(",")
        ),
    );
    Ok(())
}

/// レートが押しっぱなしを保てる範囲にあるか
fn check_rate(rate: u32) -> std::result::Result<(), (String, ExitCode)> {
    let is_too_slow = rate < MIN_RATE_HZ;
    if is_too_slow {
        return Err((
            format!(
                "a rate of {rate} Hz is too slow to hold a button; \
                 the device releases the pad after 500 ms of silence"
            ),
            ExitCode::Usage,
        ));
    }
    let is_absurdly_fast = rate > MAX_RATE_HZ;
    if is_absurdly_fast {
        return Err((
            format!("a rate of {rate} Hz is faster than the device can use (max {MAX_RATE_HZ})"),
            ExitCode::Usage,
        ));
    }
    Ok(())
}

/// 押す時間が使える範囲にあるか。
///
/// 0 を許すと、押した直後に離すことになって実機からはほぼ観測できない
fn check_hold(hold_ms: u64) -> std::result::Result<(), (String, ExitCode)> {
    let is_instant = hold_ms == 0;
    if is_instant {
        return Err((
            "a hold of 0 ms would be released before the device sees it".to_string(),
            ExitCode::Usage,
        ));
    }
    let is_too_long = Duration::from_millis(hold_ms) > MAX_HOLD;
    if is_too_long {
        return Err((
            format!(
                "a hold of {hold_ms} ms is longer than the {} s maximum",
                MAX_HOLD.as_secs()
            ),
            ExitCode::Usage,
        ));
    }
    Ok(())
}

/// 送るものを組み立てる。
///
/// 引数指定もシナリオも `Step` の並びにする。時刻は開始からの絶対位置で、
/// 再生は `input_client::play` が引き受ける
fn plan(args: &SendArgs) -> std::result::Result<Vec<Step>, (String, ExitCode)> {
    let hold = Duration::from_millis(args.hold);

    let Some(path) = &args.script else {
        // 引数のボタンは順に押す。1 つあたり hold + 離す間 (hold の半分) を取る
        let stride = hold + hold / 2;
        let mut steps = Vec::new();
        for (index, text) in args.buttons.iter().enumerate() {
            let buttons =
                input_client::parse_buttons(text).map_err(|message| (message, ExitCode::Usage))?;
            // ボタンとボタンの間に「離す」を挟む。挟まないと A の直後に B を
            // 送ることになり、押し直したことにならない
            let at = stride * index as u32;
            steps.push(Step { at, buttons });
            let is_last = index + 1 == args.buttons.len();
            if !is_last {
                steps.push(Step {
                    at: at + hold,
                    buttons: 0,
                });
            }
        }
        return Ok(steps);
    };

    let text = read_script(path)?;
    input_client::parse_scenario(&text).map_err(|message| (message, ExitCode::Usage))
}

fn read_script(path: &str) -> std::result::Result<String, (String, ExitCode)> {
    let is_stdin = path == "-";
    if is_stdin {
        let mut text = String::new();
        std::io::stdin()
            .read_to_string(&mut text)
            .map_err(|e| (format!("cannot read the scenario: {e}"), ExitCode::Failure))?;
        return Ok(text);
    }
    std::fs::read_to_string(path)
        .map_err(|e| (format!("cannot read '{path}': {e}"), ExitCode::Failure))
}

/// `procon_udp.py` の `--test-pattern` と同じ並び。ジョイスティックを繋がずに
/// 疎通を確かめる
fn test_pattern(hold_ms: u64, global: &GlobalArgs) -> CommandResult {
    use stackchan::proto::constants::button;

    check_hold(hold_ms)?;

    let states: [(&str, u8); 11] = [
        ("none", 0),
        ("A", button::A),
        ("B", button::B),
        ("START", button::START),
        ("SELECT", button::SELECT),
        ("UP", button::UP),
        ("DOWN", button::DOWN),
        ("LEFT", button::LEFT),
        ("RIGHT", button::RIGHT),
        ("A+RIGHT", button::A | button::RIGHT),
        ("all", 0xFF),
    ];

    let mut device = global.device()?;
    let output = global.output();
    let hold = Duration::from_millis(hold_ms);

    for (_, buttons) in states {
        input_client::press(&mut device, buttons, hold, DEFAULT_RATE_HZ)
            .map_err(|e| (format!("{e}"), e.exit_code()))?;
    }
    // 最後は必ず離しておく
    input_client::release(&mut device).map_err(|e| (format!("{e}"), e.exit_code()))?;

    output.success(
        &format!("sent {} test states", states.len()),
        &format!("{{\"ok\":true,\"states\":{}}}", states.len()),
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use stackchan::proto::constants::button;

    fn args(buttons: &[&str]) -> SendArgs {
        SendArgs {
            buttons: buttons.iter().map(|s| s.to_string()).collect(),
            script: None,
            hold: 100,
            rate: DEFAULT_RATE_HZ,
        }
    }

    #[test]
    fn buttons_become_presses_in_order_with_releases_between() {
        let steps = plan(&args(&["A", "B", "START"])).expect("plan");
        let buttons: Vec<u8> = steps.iter().map(|s| s.buttons).collect();
        // 押しっぱなしにならないよう、ボタンとボタンの間に離すのを挟む
        assert_eq!(
            buttons,
            vec![button::A, 0, button::B, 0, button::START],
            "each press should be let go before the next"
        );
        // 時刻は開始からの絶対位置で、単調に増える
        let times: Vec<u128> = steps.iter().map(|s| s.at.as_millis()).collect();
        assert!(times.windows(2).all(|w| w[0] < w[1]), "got {times:?}");
    }

    #[test]
    fn chords_are_accepted() {
        let steps = plan(&args(&["A+RIGHT"])).expect("plan");
        assert_eq!(steps[0].buttons, button::A | button::RIGHT);
        // 1 つだけなら離すステップは足さない (play が最後に離す)
        assert_eq!(steps.len(), 1);
    }

    #[test]
    fn nonsense_buttons_are_a_usage_error() {
        let err = plan(&args(&["NOPE"])).unwrap_err();
        assert_eq!(err.1, ExitCode::Usage);
    }

    /// シナリオの時刻は**開始からの絶対位置**。前の行からの相対にすると、
    /// 行が増えるほど後ろがずれていく
    #[test]
    fn a_scenario_keeps_its_absolute_timing() {
        let script = std::env::temp_dir().join("stackchan-test-scenario.txt");
        std::fs::write(&script, "10 A\n40 B\n").expect("write");

        let mut with_script = args(&[]);
        with_script.script = Some(script.to_string_lossy().into_owned());
        let steps = plan(&with_script).expect("plan");

        assert_eq!(steps.len(), 2);
        // frame 10 ≒ 166ms、frame 40 ≒ 666ms
        assert!(
            (160..175).contains(&steps[0].at.as_millis()),
            "{:?}",
            steps[0].at
        );
        assert!(
            (655..675).contains(&steps[1].at.as_millis()),
            "{:?}",
            steps[1].at
        );

        let _ = std::fs::remove_file(script);
    }

    #[test]
    fn a_missing_scenario_is_reported() {
        let mut with_script = args(&[]);
        with_script.script = Some("/no/such/scenario.txt".to_string());
        let err = plan(&with_script).unwrap_err();
        assert_eq!(err.1, ExitCode::Failure);
        assert!(err.0.contains("cannot read"), "got: {}", err.0);
    }
}
