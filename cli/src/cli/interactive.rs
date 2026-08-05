//! 対話的な入力 (`input keys` / `input procon`)。
//!
//! どちらも「止めるまで送り続ける」形なので、抜けるときに必ず離す。
//! 押されたまま抜けると、firmware のタイムアウト (500ms) まで実機が勝手に動く。

use std::io::IsTerminal;
use std::time::{Duration, Instant};

use stackchan::exit::ExitCode;
use stackchan::input_client;
use stackchan::keys::{self, Decoder, Held, Key, Layout, RawMode};
use stackchan::proto::pad;
use stackchan::transport::Device;

use super::{CommandResult, GlobalArgs};

/// キーボードで遊ぶ
pub fn keys(two_button: bool, sticky_ms: u64, rate_hz: u32, global: &GlobalArgs) -> CommandResult {
    let is_a_terminal = std::io::stdin().is_terminal();
    if !is_a_terminal {
        return Err((
            "input keys needs a terminal; use `input send` from a script".to_string(),
            ExitCode::Usage,
        ));
    }

    let mut device = global.device()?;
    let layout = Layout { two_button };
    let sticky = Duration::from_millis(sticky_ms.max(1));
    let interval = Duration::from_secs_f64(1.0 / rate_hz.max(1) as f64);

    // raw mode は Drop で戻る。戻し忘れると端末が壊れたまま残る
    let _raw = RawMode::enter().map_err(|e| {
        (
            format!("cannot switch the terminal to raw mode: {e}"),
            ExitCode::Failure,
        )
    })?;

    eprintln!("z=B  x=A  arrows=d-pad  Enter=Start  Space=Select   (Ctrl-C to stop)");

    let result = keys_loop(
        &mut device,
        layout,
        sticky,
        interval,
        &mut Decoder::default(),
    );

    // 何があっても離す。押されたまま抜けると実機が 500ms 勝手に動く
    let released = input_client::release(&mut device);
    match result {
        Err(message) => Err((message, ExitCode::Failure)),
        Ok(()) => released.map_err(|e| (format!("{e}"), e.exit_code())),
    }
}

fn keys_loop(
    device: &mut Device,
    layout: Layout,
    sticky: Duration,
    interval: Duration,
    decoder: &mut Decoder,
) -> std::result::Result<(), String> {
    let mut held = Held::new(sticky);
    let mut buffer = [0u8; 64];
    let mut last_sent: Option<u8> = None;
    let mut next = Instant::now();

    loop {
        // 読めなかったのを 0 として扱うと、端末が壊れたときに永久に回る
        let read = keys::read_available(&mut buffer)
            .map_err(|e| format!("cannot read the terminal: {e}"))?;
        let now = Instant::now();

        for key in decoder.feed(&buffer[..read]) {
            let is_a_quit = key == Key::Interrupt;
            if is_a_quit {
                return Ok(());
            }
            if let Some(bit) = keys::button_for(key, layout) {
                held.saw(bit, now);
            }
        }

        let buttons = held.buttons(now);
        // 変化したら即送り、変わらなくても一定間隔で送り直す
        // (500ms 無音で firmware が離してしまうため)
        let has_changed = last_sent != Some(buttons);
        let is_time_to_resend = now >= next;
        if has_changed || is_time_to_resend {
            let seq = device.next_seq();
            device
                .send(&pad::state(seq, buttons, 0))
                .map_err(|e| format!("{e}"))?;
            last_sent = Some(buttons);
            // 締切基準で進める。now + interval にすると処理ぶんずつ遅れていく
            next += interval;
            let has_fallen_behind = next < now;
            if has_fallen_behind {
                next = now + interval;
            }
        }

        // 次の送信までか、キーを拾うのに十分短い間隔か、近いほうだけ眠る。
        // VTIME に待たせないので、ここで周期を作る
        let until_next = next.saturating_duration_since(Instant::now());
        std::thread::sleep(until_next.min(Duration::from_millis(2)));
    }
}

/// Pro コントローラを流す
#[cfg(feature = "procon")]
pub fn procon(rate_hz: u32, global: &GlobalArgs) -> CommandResult {
    use stackchan::procon;

    // --host の不備は外部のデバイスに触る前に見る
    let mut target = global.device()?;

    let api = hidapi::HidApi::new().map_err(|e| {
        (
            format!("cannot open the HID subsystem: {e}"),
            ExitCode::Failure,
        )
    })?;
    let device = api
        .open(procon::VENDOR_ID, procon::PRODUCT_ID)
        .map_err(|e| {
            (
                format!("cannot open a Pro Controller: {e} (is one plugged in over USB?)"),
                ExitCode::Failure,
            )
        })?;

    // ハンドシェイクを終えないとコントローラは一切レポートを送らない
    handshake(&device).map_err(|message| (message, ExitCode::Failure))?;

    let interval = Duration::from_secs_f64(1.0 / rate_hz.max(1) as f64);
    eprintln!("streaming the Pro Controller; HOME opens the ROM menu   (Ctrl-C to stop)");

    let result = procon_loop(&device, &mut target, interval);
    let released = input_client::release(&mut target);
    match result {
        Err(message) => Err((message, ExitCode::Failure)),
        Ok(()) => released.map_err(|e| (format!("{e}"), e.exit_code())),
    }
}

#[cfg(feature = "procon")]
fn handshake(device: &hidapi::HidDevice) -> Result<(), String> {
    use stackchan::procon;

    let write = |bytes: &[u8]| -> Result<(), String> {
        device
            .write(bytes)
            .map(|_| ())
            .map_err(|e| format!("the controller did not accept the handshake: {e}"))
    };

    // `procon_udp.py:248` と同じ順序。これをやらないとレポートが流れない
    write(&[0x80, procon::USB_CMD_HANDSHAKE])?;
    std::thread::sleep(procon::HANDSHAKE_STEP);
    let mut scratch = [0u8; 64];
    let _ = device.read_timeout(&mut scratch, 50);

    write(&[0x80, procon::USB_CMD_FORCE_FULL_MODE])?;
    std::thread::sleep(procon::HANDSHAKE_STEP);

    let frame = procon::subcommand(
        0,
        procon::SUBCMD_SET_INPUT_REPORT_MODE,
        procon::REPORT_ID_STANDARD,
    );
    write(&frame)?;
    std::thread::sleep(procon::HANDSHAKE_SETTLE);

    // レポートが流れ始めるまで待つ
    let deadline = Instant::now() + procon::HANDSHAKE_DEADLINE;
    while Instant::now() < deadline {
        // 切断などの確定的なエラーは待ち続けず、理由を添えて返す
        let read = device
            .read_timeout(&mut scratch, 10)
            .map_err(|e| format!("the controller stopped responding: {e}"))?;
        let is_an_input_report = read > 0 && scratch[0] == procon::REPORT_ID_STANDARD;
        if is_an_input_report {
            return Ok(());
        }
    }
    Err("the controller never started sending reports".to_string())
}

#[cfg(feature = "procon")]
fn procon_loop(
    controller: &hidapi::HidDevice,
    target: &mut Device,
    interval: Duration,
) -> Result<(), String> {
    use stackchan::procon;

    // レポートが途絶えたら押しっぱなしを解く。最後の状態を送り続けると、
    // firmware の 500ms フェイルセーフを CLI 自身が無効にしてしまう
    const SILENCE_LIMIT: Duration = Duration::from_millis(250);

    let mut report = [0u8; 64];
    let mut last_sent: Option<u8> = None;
    let mut last_home = false;
    let mut next = Instant::now();
    let mut last_report = Instant::now();

    loop {
        let read = controller
            .read_timeout(&mut report, 10)
            .map_err(|e| format!("the controller stopped responding: {e}"))?;

        let state = (read > 0)
            .then(|| procon::decode_report(&report[..read]))
            .flatten();
        if state.is_some() {
            last_report = Instant::now();
        }

        if let Some(state) = &state {
            // HOME は押した瞬間に 1 回だけ。押しっぱなしで連発しない
            let is_a_fresh_press = state.home && !last_home;
            if is_a_fresh_press {
                use stackchan::proto::constants::CTRL_MENU;
                use stackchan::proto::ctrl;
                let seq = target.next_seq();
                target
                    .send(&ctrl::command(seq, CTRL_MENU, 0))
                    .map_err(|e| format!("{e}"))?;
            }
            last_home = state.home;
        }

        let now = Instant::now();
        // 届いていれば新しい状態。途絶えていたら、押しっぱなしを解いて 0 に
        // 落とす — そうしないと最後のボタンを永久に送り続けることになる
        let has_gone_quiet = now.duration_since(last_report) > SILENCE_LIMIT;
        let buttons = match state {
            Some(state) => state.buttons,
            None if has_gone_quiet => 0,
            None => last_sent.unwrap_or(0),
        };
        let has_changed = last_sent != Some(buttons);
        let is_time_to_resend = now >= next;
        if has_changed || is_time_to_resend {
            let seq = target.next_seq();
            target
                .send(&pad::state(seq, buttons, 0))
                .map_err(|e| format!("{e}"))?;
            last_sent = Some(buttons);
            // 締切基準で進める (処理ぶんずつ遅れていかないように)
            next += interval;
            let has_fallen_behind = next < now;
            if has_fallen_behind {
                next = now + interval;
            }
        }
    }
}

/// HID なしでビルドされた場合
#[cfg(not(feature = "procon"))]
pub fn procon(_rate_hz: u32, _global: &GlobalArgs) -> CommandResult {
    Err((
        "this build has no HID support; rebuild with --features procon".to_string(),
        ExitCode::Failure,
    ))
}

/// 繋がっているコントローラを列挙する
#[cfg(feature = "procon")]
pub fn list(global: &GlobalArgs) -> CommandResult {
    use stackchan::output::quote;
    use stackchan::procon;

    let api = hidapi::HidApi::new().map_err(|e| {
        (
            format!("cannot open the HID subsystem: {e}"),
            ExitCode::Failure,
        )
    })?;

    let found: Vec<(String, u16, u16)> = api
        .device_list()
        .map(|info| {
            (
                info.product_string().unwrap_or("(unnamed)").to_string(),
                info.vendor_id(),
                info.product_id(),
            )
        })
        .filter(|(_, vendor, product)| {
            *vendor == procon::VENDOR_ID && *product == procon::PRODUCT_ID
        })
        .collect();

    let output = global.output();
    let is_empty = found.is_empty();
    if is_empty {
        output.success(
            "no Pro Controller found (plug one in over USB)",
            "{\"ok\":true,\"controllers\":[],\"count\":0}",
        );
        return Ok(());
    }

    let text = found
        .iter()
        .map(|(name, vendor, product)| format!("{name}  {vendor:04x}:{product:04x}"))
        .collect::<Vec<_>>()
        .join("\n");
    let json = format!(
        "{{\"ok\":true,\"controllers\":[{}],\"count\":{}}}",
        found
            .iter()
            .map(|(name, vendor, product)| format!(
                "{{\"name\":{},\"vendorId\":{vendor},\"productId\":{product}}}",
                quote(name)
            ))
            .collect::<Vec<_>>()
            .join(","),
        found.len()
    );
    output.success(&text, &json);
    Ok(())
}

#[cfg(not(feature = "procon"))]
pub fn list(_global: &GlobalArgs) -> CommandResult {
    Err((
        "this build has no HID support; rebuild with --features procon".to_string(),
        ExitCode::Failure,
    ))
}
