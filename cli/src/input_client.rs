//! コントローラ入力の送信 (type 0)。
//!
//! **500ms (`INPUT_TIMEOUT_MS`) 無音で firmware は両パッドを離した状態に戻す。**
//! 送信側が落ちてもボタンが押しっぱなしにならないための仕組みだが、裏を返すと
//! 「押している」状態は送り続けないと維持できない。
//!
//! 送信ループは `tools/procon_udp.py` と同じ形にする: 変化したら即送り、
//! 変化が無くても一定間隔で送り直す。締切を `next += interval` で進めるのは、
//! 送信にかかった時間ぶんずつ遅れていくのを防ぐため。

use std::thread;
use std::time::{Duration, Instant};

use crate::proto::pad;
use crate::transport::{Device, Result};

/// 既定の送信レート。`procon_udp.py` と同じ
pub const DEFAULT_RATE_HZ: u32 = 120;

/// 押しっぱなしを保つために必要な最低レート。
///
/// 「1 発落ちても間に合う」には、`2 * interval < INPUT_TIMEOUT_MS` が要る。
/// 境界ちょうど (4Hz = 250ms 間隔、落ちると 500ms) では余裕が無く、送信・
/// 受信・フレームループのジッタが少しでも入ると先に離されてしまう。
/// 5Hz なら 1 発落ちても 400ms で、100ms の余裕がある
pub const MIN_RATE_HZ: u32 = 5;

/// 上限。これより速くしても届く量が増えるだけで、押しっぱなしの維持には
/// 寄与しない。`Duration` が 0 に丸まって sleep 無しの送信ループになるのも防ぐ
pub const MAX_RATE_HZ: u32 = 1000;

/// 押す時間の上限。これを超える指定は、待ち続けるより間違いの可能性が高い
pub const MAX_HOLD: Duration = Duration::from_secs(60);

/// 離すときに送る回数。
///
/// 1 発だと落ちたときにボタンが押されたままになり、タイムアウトで戻るまでの
/// 500ms のあいだ勝手に動く。数発重ねるほうが安い
const RELEASE_REPEATS: u32 = 3;

/// ボタンを `hold` のあいだ押してから離す。
///
/// **押している時間は最初の送信が成功してから数える。** `.local` の解決に
/// 時間がかかると、期限を先に作る形では押している時間がそのぶん短くなる
pub fn press(device: &mut Device, buttons: u8, hold: Duration, rate_hz: u32) -> Result<()> {
    // 押しはじめは即座に送る
    let seq = device.next_seq();
    device.send(&pad::state(seq, buttons, 0))?;

    let started = Instant::now();
    let until = started + hold;
    let result = hold_until(device, buttons, until, rate_hz);

    // 送信の途中で落ちても離しにいく。押されたまま抜けると、firmware の
    // タイムアウト (500ms) まで勝手に動く
    let released = release(device);
    result.and(released)
}

/// `until` まで同じ状態を送り続ける。離しはしない
fn hold_until(device: &mut Device, buttons: u8, until: Instant, rate_hz: u32) -> Result<()> {
    let interval = interval_for(rate_hz);
    let mut next = Instant::now() + interval;

    while Instant::now() < until {
        let now = Instant::now();
        let is_time_to_resend = now >= next;
        if !is_time_to_resend {
            // 締切か終わり、近いほうまで眠る
            let wake = next.min(until);
            let nap = wake.saturating_duration_since(now);
            thread::sleep(nap.min(Duration::from_millis(5)));
            continue;
        }

        let seq = device.next_seq();
        device.send(&pad::state(seq, buttons, 0))?;
        // 締切基準で進める。now + interval にすると送信ぶんずつ遅れていく
        next += interval;
        let has_fallen_behind = next < now;
        if has_fallen_behind {
            next = now + interval;
        }
    }
    Ok(())
}

/// シナリオを**絶対時刻**で再生する。
///
/// 各行のフレーム番号は開始時刻からの位置であって、前の行からの相対では
/// ない。相対として扱うと、行が増えるほど後ろの行が遅れていく。
///
/// 同じフレームに複数行あるときは最後だけを送る (`verify_host` と同じ)。
/// 途中で状態が変わるとき、間に「離す」を挟まない — 挟むとシナリオが
/// 意図していない一瞬の解放が入る
pub fn play(device: &mut Device, steps: &[Step], tail: Duration, rate_hz: u32) -> Result<()> {
    let is_empty = steps.is_empty();
    if is_empty {
        return Ok(());
    }

    let started = Instant::now();
    let result = play_from(device, steps, tail, rate_hz, started);
    // 何があっても最後は離す
    let released = release(device);
    result.and(released)
}

fn play_from(
    device: &mut Device,
    steps: &[Step],
    tail: Duration,
    rate_hz: u32,
    started: Instant,
) -> Result<()> {
    for (index, step) in steps.iter().enumerate() {
        // 同じフレームの行が続くなら、最後のものだけを送る
        let is_superseded = steps.get(index + 1).is_some_and(|next| next.at == step.at);
        if is_superseded {
            continue;
        }

        // この状態を保つのは、次の行の時刻まで (最後の行は tail ぶん)
        let until = match steps[index + 1..].iter().find(|next| next.at > step.at) {
            Some(next) => started + next.at,
            None => started + step.at + tail,
        };

        // 開始からの絶対位置で待つ。前の行からの相対にすると誤差が積もる
        let begins_at = started + step.at;
        let now = Instant::now();
        let is_still_ahead = begins_at > now;
        if is_still_ahead {
            thread::sleep(begins_at - now);
        }

        let seq = device.next_seq();
        device.send(&pad::state(seq, step.buttons, 0))?;
        hold_until(device, step.buttons, until, rate_hz)?;
    }
    Ok(())
}

/// 両パッドを離す。数発重ねて、1 発落ちても押されたままにならないようにする
pub fn release(device: &mut Device) -> Result<()> {
    for _ in 0..RELEASE_REPEATS {
        let seq = device.next_seq();
        device.send(&pad::release(seq))?;
    }
    Ok(())
}

/// 1 発だけ送る。押しっぱなしにはならない (500ms で戻る)
pub fn send_once(device: &mut Device, buttons: u8) -> Result<()> {
    let seq = device.next_seq();
    device.send(&pad::state(seq, buttons, 0))
}

fn interval_for(rate_hz: u32) -> Duration {
    let rate = rate_hz.max(1);
    Duration::from_secs_f64(1.0 / rate as f64)
}

/// シナリオの 1 行
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Step {
    /// 開始からの位置。**前の行からの相対ではない** — 相対にすると行が
    /// 増えるほど後ろが遅れていく
    pub at: Duration,
    pub buttons: u8,
}

/// `tools/scenario-sample.txt` と同じ書式を読む。
///
/// `<フレーム番号> <ボタン>` で、状態は次の行まで保持される。フレーム番号を
/// 時間に直すのは NTSC の 1 フレーム (16.639ms) 換算。同じファイルが
/// `just verify` でも実機でも使えるようにするため、書式は変えない
pub fn parse_scenario(text: &str) -> std::result::Result<Vec<Step>, String> {
    // NTSC の 1 フレーム。`config.h` の FRAME_PERIOD_US
    const FRAME: Duration = Duration::from_micros(16639);

    let mut steps = Vec::new();
    let mut previous_frame: u64 = 0;

    for (number, raw) in text.lines().enumerate() {
        let line = raw.split('#').next().unwrap_or("").trim();
        let is_blank = line.is_empty();
        if is_blank {
            continue;
        }

        let mut fields = line.split_whitespace();
        let Some(frame_text) = fields.next() else {
            continue;
        };
        let frame: u64 = frame_text
            .parse()
            .map_err(|_| format!("line {}: '{frame_text}' is not a frame number", number + 1))?;

        let is_out_of_order = frame < previous_frame;
        if is_out_of_order {
            return Err(format!(
                "line {}: frame {frame} comes after {previous_frame}; frames must not go backwards",
                number + 1
            ));
        }

        let Some(buttons_text) = fields.next() else {
            return Err(format!("line {}: no buttons after the frame", number + 1));
        };
        let buttons = parse_buttons(buttons_text)
            .map_err(|message| format!("line {}: {message}", number + 1))?;

        // 余りは黙って捨てない。`verify_host` も拒否するので、同じ書式で
        // 通るファイルの範囲を揃える
        if let Some(extra) = fields.next() {
            return Err(format!(
                "line {}: unexpected '{extra}' after the buttons",
                number + 1
            ));
        }

        // 大きなフレーム番号で折り返さないように、u32 に収まるか先に見る
        let Ok(frames) = u32::try_from(frame) else {
            return Err(format!(
                "line {}: frame {frame} is too far away",
                number + 1
            ));
        };
        let Some(at) = FRAME.checked_mul(frames) else {
            return Err(format!(
                "line {}: frame {frame} is too far away",
                number + 1
            ));
        };

        steps.push(Step { at, buttons });
        previous_frame = frame;
    }

    Ok(steps)
}

/// `A+RIGHT` / `NONE` / `0` / 1-2 桁の 16 進を読む
pub fn parse_buttons(text: &str) -> std::result::Result<u8, String> {
    let upper = text.to_ascii_uppercase();
    let is_release = upper == "NONE" || upper == "0";
    if is_release {
        return Ok(0);
    }

    // 16 進はボタン名と紛れない形のときだけ。`A` と `B` はボタン名として
    // 読むので、1 桁は数字だけを受ける (`verify_host` は 1-2 桁を受ける)
    let is_a_button_letter = upper.len() == 1 && upper.chars().all(|c| c.is_ascii_alphabetic());
    let looks_like_hex = !is_a_button_letter
        && (1..=2).contains(&upper.len())
        && upper.chars().all(|c| c.is_ascii_hexdigit());
    if looks_like_hex {
        if let Ok(bits) = u8::from_str_radix(&upper, 16) {
            return Ok(bits);
        }
    }

    pad::buttons_from_combo(text)
}

#[cfg(test)]
mod tests {
    use super::*;

    use crate::proto::constants::button;

    /// 「1 発落ちても間に合う」を数字で確かめる。境界ちょうどでは、
    /// ジッタが少し入るだけで先に離されてしまう
    #[test]
    fn the_minimum_rate_survives_a_dropped_packet() {
        use crate::proto::constants::INPUT_TIMEOUT_MS;
        let timeout = Duration::from_millis(INPUT_TIMEOUT_MS);
        let interval = interval_for(MIN_RATE_HZ);

        // 1 発落ちたときの実効間隔
        let with_one_lost = interval * 2;
        assert!(
            with_one_lost < timeout,
            "a lost packet would let the device release the pad: {with_one_lost:?} vs {timeout:?}"
        );
        // 余裕が 50ms 以上あること (境界ちょうどにしない)
        let margin = timeout - with_one_lost;
        assert!(
            margin >= Duration::from_millis(50),
            "too little margin for jitter: {margin:?}"
        );
    }

    #[test]
    fn the_maximum_rate_still_has_a_real_interval() {
        // 0 に丸まると sleep 無しの送信ループになる
        assert!(!interval_for(MAX_RATE_HZ).is_zero());
    }

    #[test]
    fn the_interval_follows_the_rate() {
        assert_eq!(interval_for(100), Duration::from_millis(10));
        // 0 を渡されても割り算で落ちない
        assert_eq!(interval_for(0), Duration::from_secs(1));
    }

    #[test]
    fn buttons_accept_names_hex_and_release() {
        assert_eq!(parse_buttons("A"), Ok(button::A));
        assert_eq!(parse_buttons("A+RIGHT"), Ok(button::A | button::RIGHT));
        assert_eq!(parse_buttons("NONE"), Ok(0));
        assert_eq!(parse_buttons("none"), Ok(0));
        assert_eq!(parse_buttons("0"), Ok(0));
        // 16 進は 1-2 桁 (`verify_host` と同じ範囲)
        assert_eq!(parse_buttons("09"), Ok(0x09));
        assert_eq!(parse_buttons("FF"), Ok(0xFF));
        assert_eq!(parse_buttons("1"), Ok(0x01));
        assert_eq!(parse_buttons("9"), Ok(0x09));
    }

    // `A` は 16 進としても読めるが、ボタン名として扱う。シナリオの書式は
    // ボタン名が主で、16 進は補助
    #[test]
    fn a_single_letter_is_a_button_not_hex() {
        assert_eq!(parse_buttons("A"), Ok(button::A));
        assert_eq!(parse_buttons("B"), Ok(button::B));
    }

    #[test]
    fn nonsense_buttons_are_rejected() {
        assert!(parse_buttons("NOPE").is_err());
        assert!(parse_buttons("").is_err());
    }

    /// 時刻は**開始からの絶対位置**。前の行からの相対にすると、行が増える
    /// ほど後ろがずれていく (`10 A / 40 B` の B が frame 70 に来てしまう)
    #[test]
    fn a_scenario_becomes_absolute_positions() {
        let steps = parse_scenario("150 START\n180 NONE\n").expect("scenario");
        assert_eq!(steps.len(), 2);
        assert_eq!(steps[0].buttons, button::START);
        // 150 フレーム ≒ 2.5 秒
        assert!(
            (2450..2550).contains(&steps[0].at.as_millis()),
            "{:?}",
            steps[0].at
        );
        // 180 フレーム ≒ 3.0 秒 (30 フレームぶんの差分ではない)
        assert!(
            (2950..3050).contains(&steps[1].at.as_millis()),
            "{:?}",
            steps[1].at
        );
    }

    #[test]
    fn comments_and_blank_lines_are_skipped() {
        let text = "# a comment\n\n  \n150 A  # trailing comment\n";
        let steps = parse_scenario(text).expect("scenario");
        assert_eq!(steps.len(), 1);
        assert_eq!(steps[0].buttons, button::A);
    }

    // 余りを黙って捨てると、書き間違いに気づけないまま別の動きをする
    #[test]
    fn extra_fields_are_rejected() {
        let err = parse_scenario("1 A extra\n").unwrap_err();
        assert!(err.contains("unexpected"), "got: {err}");
    }

    // 大きなフレーム番号で折り返さない
    #[test]
    fn an_absurd_frame_number_is_rejected() {
        let err = parse_scenario("99999999999 A\n").unwrap_err();
        assert!(err.contains("too far away"), "got: {err}");
    }

    // 昇順でないと待ち時間が負になる。黙って 0 にすると意図と違う動きになる
    #[test]
    fn frames_going_backwards_are_rejected() {
        let err = parse_scenario("100 A\n50 B\n").unwrap_err();
        assert!(err.contains("backwards"), "got: {err}");
    }

    #[test]
    fn malformed_lines_say_which_line_is_wrong() {
        let err = parse_scenario("150 A\nnot-a-frame B\n").unwrap_err();
        assert!(err.contains("line 2"), "got: {err}");

        let err = parse_scenario("150\n").unwrap_err();
        assert!(err.contains("line 1"), "got: {err}");
        assert!(err.contains("no buttons"), "got: {err}");
    }

    /// リポジトリにある実物のシナリオが読めること。`just verify` と同じ
    /// ファイルを実機に流せる、というのがこの書式を選んだ理由
    #[test]
    fn the_repository_scenario_parses() {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../tools/scenario-sample.txt");
        let Ok(text) = std::fs::read_to_string(path) else {
            // リポジトリ外でビルドされた場合は飛ばす
            return;
        };
        let steps = parse_scenario(&text).expect("the sample scenario should parse");
        assert!(!steps.is_empty(), "the sample scenario has entries");
    }
}
