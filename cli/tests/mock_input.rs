//! コントローラ入力を、受信したパケットを記録するモックに対して検証する。
//!
//! 見たいのは:
//! - 押している間は送り続けること (500ms 無音で firmware が離してしまう)
//! - 離すときに確実に 0 を送ること (落ちると押しっぱなしになる)
//! - シナリオが `just verify` と同じ書式で読めること

use std::net::UdpSocket;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use stackchan::input_client;
use stackchan::proto::constants::{INPUT_TIMEOUT_MS, button};
use stackchan::transport::Device;

/// 受け取ったパッドパケットを記録するモック
struct MockPad {
    port: u16,
    /// (受信時刻, pad1)
    received: Arc<Mutex<Vec<(Instant, u8)>>>,
    _handle: thread::JoinHandle<()>,
}

impl MockPad {
    fn new() -> Self {
        let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
        let port = socket.local_addr().expect("addr").port();
        socket
            .set_read_timeout(Some(Duration::from_secs(10)))
            .expect("timeout");

        let received = Arc::new(Mutex::new(Vec::new()));
        let recorder = Arc::clone(&received);

        let handle = thread::spawn(move || {
            let mut buffer = [0u8; 64];
            loop {
                let Ok((n, _)) = socket.recv_from(&mut buffer) else {
                    return;
                };
                let is_a_pad_packet = n >= 8 && buffer[0] == b'N' && buffer[1] == b'P';
                if !is_a_pad_packet {
                    continue;
                }
                recorder.lock().unwrap().push((Instant::now(), buffer[6]));
            }
        });

        Self {
            port,
            received,
            _handle: handle,
        }
    }

    fn device(&self) -> Device {
        Device::new("127.0.0.1", self.port, 0).expect("device")
    }

    fn packets(&self) -> Vec<(Instant, u8)> {
        self.received.lock().unwrap().clone()
    }

    fn request_count(&self) -> usize {
        self.received.lock().unwrap().len()
    }

    fn buttons_seen(&self) -> Vec<u8> {
        self.packets().iter().map(|(_, b)| *b).collect()
    }

    /// 受信が落ち着くまで待つ
    fn settle(&self) {
        thread::sleep(Duration::from_millis(120));
    }
}

/// **押している間は送り続ける。** 500ms 無音で firmware が離してしまうので、
/// 1 発送って黙っていると押しっぱなしにならない
#[test]
fn a_held_button_is_resent_faster_than_the_device_forgets() {
    let device = MockPad::new();
    let hold = Duration::from_millis(600);

    input_client::press(&mut device.device(), button::A, hold, 60).expect("press");
    device.settle();

    let packets = device.packets();
    let pressed: Vec<&(Instant, u8)> = packets.iter().filter(|(_, b)| *b == button::A).collect();
    assert!(
        pressed.len() > 5,
        "a 600 ms hold at 60 Hz should resend many times, got {}",
        pressed.len()
    );

    // どの隣り合う 2 発も、firmware が忘れるより短い間隔で届いていること
    let timeout = Duration::from_millis(INPUT_TIMEOUT_MS);
    for pair in pressed.windows(2) {
        let gap = pair[1].0.duration_since(pair[0].0);
        assert!(
            gap < timeout,
            "a gap of {gap:?} would let the device release the pad"
        );
    }
}

/// 離すときは 0 を重ねて送る。1 発落ちると、タイムアウトまでの 500ms
/// ボタンが押されたままになる
#[test]
fn releasing_is_repeated_so_a_lost_packet_cannot_stick() {
    let device = MockPad::new();

    input_client::press(
        &mut device.device(),
        button::B,
        Duration::from_millis(50),
        60,
    )
    .expect("press");
    device.settle();

    let seen = device.buttons_seen();
    let trailing_releases = seen.iter().rev().take_while(|b| **b == 0).count();
    assert!(
        trailing_releases >= 3,
        "the release should be repeated, got {trailing_releases}"
    );
}

#[test]
fn a_press_sends_the_requested_buttons() {
    let device = MockPad::new();

    let chord = button::A | button::RIGHT;
    input_client::press(&mut device.device(), chord, Duration::from_millis(50), 60).expect("press");
    device.settle();

    assert!(
        device.buttons_seen().contains(&chord),
        "the chord should have been sent: {:?}",
        device.buttons_seen()
    );
}

/// 1 発だけ送る形も要る。押しっぱなしにはならない
#[test]
fn a_single_packet_can_be_sent() {
    let device = MockPad::new();

    input_client::send_once(&mut device.device(), button::START).expect("send");
    device.settle();

    assert_eq!(device.buttons_seen(), vec![button::START]);
}

// ----------------------------------------------------- バイナリ経由の検証

mod through_the_binary {
    use super::*;
    use std::io::Write;
    use std::process::{Command, Stdio};

    fn run(port: u16, args: &[&str]) -> std::process::Output {
        Command::new(env!("CARGO_BIN_EXE_stackchan"))
            .args(args)
            .env("STACKCHAN_HOST", "127.0.0.1")
            .env("STACKCHAN_PORT", port.to_string())
            .output()
            .expect("failed to run the stackchan binary")
    }

    #[test]
    fn buttons_given_as_arguments_are_pressed_in_order() {
        let device = MockPad::new();
        let out = run(device.port, &["input", "send", "A", "B", "--hold", "30"]);

        assert_eq!(out.status.code(), Some(0));
        device.settle();

        let seen = device.buttons_seen();
        let a_at = seen.iter().position(|b| *b == button::A).expect("A");
        let b_at = seen.iter().position(|b| *b == button::B).expect("B");
        assert!(a_at < b_at, "presses should happen in order: {seen:?}");
    }

    #[test]
    fn the_test_pattern_walks_through_every_button() {
        let device = MockPad::new();
        let out = run(device.port, &["input", "test-pattern", "--hold", "20"]);

        assert_eq!(out.status.code(), Some(0));
        device.settle();

        let seen = device.buttons_seen();
        for expected in [button::A, button::B, button::START, button::SELECT, 0xFF] {
            assert!(
                seen.contains(&expected),
                "the pattern should include {expected:#04x}: {seen:?}"
            );
        }
        // 最後は必ず離しておく
        assert_eq!(seen.last(), Some(&0), "it must end released");
    }

    /// `just verify` のシナリオがそのまま流せる。この書式を選んだ理由
    #[test]
    fn a_scenario_can_be_piped_in() {
        let device = MockPad::new();

        let mut child = Command::new(env!("CARGO_BIN_EXE_stackchan"))
            .args(["input", "send", "--script", "-"])
            .env("STACKCHAN_HOST", "127.0.0.1")
            .env("STACKCHAN_PORT", device.port.to_string())
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("spawn");

        // フレーム番号は小さくして、テストを待たせない
        child
            .stdin
            .as_mut()
            .expect("stdin")
            .write_all(b"# a comment\n1 START\n3 NONE\n")
            .expect("write");

        let out = child.wait_with_output().expect("wait");
        assert_eq!(out.status.code(), Some(0));
        device.settle();

        assert!(
            device.buttons_seen().contains(&button::START),
            "the scenario should have pressed START: {:?}",
            device.buttons_seen()
        );
    }

    /// 送るものが無いのに黙って成功しない
    #[test]
    fn sending_nothing_is_a_usage_error() {
        let device = MockPad::new();
        let out = run(device.port, &["input", "send"]);

        assert_eq!(out.status.code(), Some(2));
        let text = String::from_utf8(out.stderr).expect("utf-8");
        assert!(text.starts_with("stackchan: "), "got: {text}");
    }

    /// 押しっぱなしにできないレートは断る。黙って受けると、押したつもりが
    /// 途中で離れる
    #[test]
    fn a_rate_too_slow_to_hold_a_button_is_refused() {
        let device = MockPad::new();
        let out = run(device.port, &["input", "send", "A", "--rate", "1"]);

        assert_eq!(out.status.code(), Some(2));
        let text = String::from_utf8(out.stderr).expect("utf-8");
        assert!(text.contains("too slow"), "got: {text}");
    }

    /// 極端な値は断る。速すぎるレートは sleep 無しの送信ループになり、
    /// 0ms の hold は実機が見る前に離されて観測できない
    #[test]
    fn absurd_rates_and_holds_are_refused() {
        let device = MockPad::new();

        for args in [
            vec!["input", "send", "A", "--rate", "999999"],
            vec!["input", "send", "A", "--hold", "0"],
            vec!["input", "send", "A", "--hold", "999999999"],
        ] {
            let out = run(device.port, &args);
            assert_eq!(out.status.code(), Some(2), "args {args:?}");
        }
        assert_eq!(device.request_count(), 0, "nothing should have been sent");
    }

    /// シナリオの時刻は開始からの絶対位置。相対として扱うと、行が増えるほど
    /// 後ろがずれていく (`10 A / 40 B` の B が frame 70 に来てしまう)
    #[test]
    fn a_scenario_keeps_the_gap_between_its_lines() {
        let device = MockPad::new();

        let mut child = Command::new(env!("CARGO_BIN_EXE_stackchan"))
            .args(["input", "send", "--script", "-", "--hold", "50"])
            .env("STACKCHAN_HOST", "127.0.0.1")
            .env("STACKCHAN_PORT", device.port.to_string())
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("spawn");
        child
            .stdin
            .as_mut()
            .expect("stdin")
            .write_all(b"10 A\n40 B\n")
            .expect("write");
        let out = child.wait_with_output().expect("wait");
        assert_eq!(out.status.code(), Some(0));
        device.settle();

        let packets = device.packets();
        let first_a = packets
            .iter()
            .find(|(_, b)| *b == button::A)
            .expect("A should have been sent");
        let first_b = packets
            .iter()
            .find(|(_, b)| *b == button::B)
            .expect("B should have been sent");

        // 30 フレーム ≒ 499ms。相対として扱うと倍近くになる
        let gap = first_b.0.duration_since(first_a.0);
        assert!(
            gap > Duration::from_millis(400) && gap < Duration::from_millis(650),
            "the gap between the two lines should be about 30 frames, got {gap:?}"
        );
    }

    #[test]
    fn a_malformed_scenario_says_which_line_is_wrong() {
        let device = MockPad::new();
        let script = std::env::temp_dir().join("stackchan-test-bad-scenario.txt");
        std::fs::write(&script, "1 A\nnope B\n").expect("write");

        let out = run(
            device.port,
            &["input", "send", "--script", script.to_str().unwrap()],
        );
        assert_eq!(out.status.code(), Some(2));
        let text = String::from_utf8(out.stderr).expect("utf-8");
        assert!(text.contains("line 2"), "got: {text}");

        let _ = std::fs::remove_file(script);
    }
}
