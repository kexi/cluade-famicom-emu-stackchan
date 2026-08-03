//! デバッグスナップショットを、応答を制御できるモックデバイスに対して検証する。
//!
//! 見たいのは:
//! - 分割された応答を組み立てられること
//! - パートが欠けたら**部分的なスナップショットを返さない** (レジスタの途中で
//!   切れたバイト列は意味を持たない)
//! - **再送しない** (取り直しても「さっきの状態」は返らない)

use std::net::UdpSocket;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::thread;
use std::time::Duration;

use stackchan::debug_client;
use stackchan::exit::ExitCode;
use stackchan::proto::constants::{
    DEBUG_CHUNK, DEBUG_HEADER, DEBUG_SNAPSHOT_SIZE, DEBUG_SNAPSHOT_WITH_WAVES_SIZE,
};
use stackchan::transport::Device;

/// スナップショットを返すモックデバイス
struct MockDebug {
    port: u16,
    requests: Arc<AtomicUsize>,
    _handle: thread::JoinHandle<()>,
}

/// どう答えるか
#[derive(Clone, Copy)]
enum Behavior {
    /// 仕様どおりのスナップショットを分割して返す
    Snapshot { with_waves: bool },
    /// 最後のパートを落とす
    MissingLastPart,
    /// 仕様のどの長さとも合わないものを返す
    WrongSize,
    /// 2 か所の PC が食い違うものを返す
    InconsistentPc,
    /// 無応答
    Silent,
}

impl MockDebug {
    fn new(behavior: Behavior) -> Self {
        let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
        let port = socket.local_addr().expect("addr").port();
        socket
            .set_read_timeout(Some(Duration::from_secs(10)))
            .expect("timeout");

        let requests = Arc::new(AtomicUsize::new(0));
        let counter = Arc::clone(&requests);

        let handle = thread::spawn(move || {
            let mut buffer = [0u8; 2048];
            loop {
                let Ok((n, from)) = socket.recv_from(&mut buffer) else {
                    return;
                };
                counter.fetch_add(1, Ordering::SeqCst);
                let request = &buffer[..n];
                let seq = u16::from_le_bytes([request[4], request[5]]);

                let Behavior::Silent = behavior else {
                    let body = match behavior {
                        Behavior::WrongSize => vec![0u8; DEBUG_SNAPSHOT_SIZE + 100],
                        Behavior::Snapshot { with_waves: true } => {
                            build_body(DEBUG_SNAPSHOT_WITH_WAVES_SIZE)
                        }
                        Behavior::InconsistentPc => {
                            let mut body = build_body(DEBUG_SNAPSHOT_SIZE);
                            // コード窓の側の PC だけを別の値にする
                            body[36] = 0x00;
                            body[37] = 0x90;
                            body
                        }
                        _ => build_body(DEBUG_SNAPSHOT_SIZE),
                    };

                    let parts: Vec<&[u8]> = body.chunks(DEBUG_CHUNK).collect();
                    let nparts = parts.len() as u8;
                    let drop_last = matches!(behavior, Behavior::MissingLastPart);

                    for (index, payload) in parts.iter().enumerate() {
                        let is_dropped = drop_last && index + 1 == parts.len();
                        if is_dropped {
                            continue;
                        }
                        let s = seq.to_le_bytes();
                        let mut datagram = vec![b'N', b'D', 1, index as u8, nparts, s[0], s[1]];
                        datagram.extend_from_slice(payload);
                        debug_assert!(datagram.len() >= DEBUG_HEADER);
                        let _ = socket.send_to(&datagram, from);
                    }
                    continue;
                };
            }
        });

        Self {
            port,
            requests,
            _handle: handle,
        }
    }

    fn device(&self) -> Device {
        Device::new("127.0.0.1", self.port, 0).expect("device")
    }

    fn request_count(&self) -> usize {
        self.requests.load(Ordering::SeqCst)
    }
}

/// レイアウトどおりの中身を持つスナップショットを作る
fn build_body(size: usize) -> Vec<u8> {
    let mut body = vec![0u8; size];
    // CPU レジスタ: PC(2) A X Y SP P pad frame(4)
    body[0] = 0x23; // PC lo
    body[1] = 0x81; // PC hi
    body[2] = 0xAA; // A
    body[3] = 0x01; // X
    body[4] = 0x02; // Y
    body[5] = 0xFD; // SP
    body[6] = 0x24; // P
    body[8] = 0xD2; // frame lo (1234)
    body[9] = 0x04;
    // PC はコード窓の直前にもう一度載る (core/nes.cpp:385)。firmware が
    // 繰り返すのは、コード窓を別の PC と組み合わせないため
    body[36] = 0x23;
    body[37] = 0x81;
    // WRAM の先頭に目印
    body[86] = 0xBE;
    body[87] = 0xEF;
    body
}

#[test]
fn a_split_snapshot_is_reassembled() {
    let device = MockDebug::new(Behavior::Snapshot { with_waves: false });

    let (snapshot, registers) =
        debug_client::snapshot(&mut device.device(), false).expect("snapshot");

    assert_eq!(registers.pc, 0x8123);
    assert_eq!(registers.a, 0xAA);
    assert_eq!(registers.sp, 0xFD);
    assert_eq!(registers.frame, 1234);
    assert_eq!(snapshot.wram.len(), 2048);
    assert_eq!(&snapshot.wram[..2], &[0xBE, 0xEF]);
    assert!(snapshot.waves.is_none());
    assert_eq!(device.request_count(), 1);
}

#[test]
fn waves_are_collected_when_asked_for() {
    let device = MockDebug::new(Behavior::Snapshot { with_waves: true });

    let (snapshot, _) = debug_client::snapshot(&mut device.device(), true).expect("snapshot");
    let waves = snapshot.waves.expect("waves should be present");
    assert_eq!(waves.len(), 6, "P1, P2, TRI, NOI, DMC, MIX");
    assert!(waves.iter().all(|row| row.len() == 280));
}

/// **部分的なスナップショットを返さない。** レジスタの途中で切れたバイト列は
/// 意味を持たないので、欠けたら失敗として報告する
#[test]
fn a_missing_part_is_not_returned_as_a_partial_snapshot() {
    let device = MockDebug::new(Behavior::MissingLastPart);

    let err = debug_client::snapshot(&mut device.device(), false)
        .expect_err("an incomplete snapshot must not be returned");
    assert_eq!(err.exit_code(), ExitCode::Timeout);
}

/// **再送しない。** 取り直しても「さっきの状態」は返らないので、待ち直す
/// 意味がない
#[test]
fn a_snapshot_is_requested_only_once() {
    let device = MockDebug::new(Behavior::Silent);

    let err = debug_client::snapshot(&mut device.device(), false).expect_err("no reply");
    assert_eq!(err.exit_code(), ExitCode::Timeout);
    assert_eq!(
        device.request_count(),
        1,
        "a snapshot is a moment in time; retrying would fetch a different one"
    );
}

/// 仕様のどの長さとも合わないものは、黙って一部だけ読まずに断る
#[test]
fn a_snapshot_of_the_wrong_size_is_refused() {
    let device = MockDebug::new(Behavior::WrongSize);

    let err = debug_client::snapshot(&mut device.device(), false).expect_err("wrong size");
    assert_eq!(err.exit_code(), ExitCode::Failure);
    assert!(err.to_string().contains("unexpected size"), "got: {err}");
}

/// firmware が PC を 2 か所に載せるのは、コード窓を別の PC と組み合わせない
/// ため (`core/nes.cpp:382`)。食い違うなら組み立てを間違えているので、
/// 読めたふりをしない
#[test]
fn a_snapshot_that_disagrees_with_itself_is_refused() {
    let device = MockDebug::new(Behavior::InconsistentPc);

    let err = debug_client::snapshot(&mut device.device(), false)
        .expect_err("a snapshot whose two PCs differ must not be trusted");
    assert_eq!(err.exit_code(), ExitCode::Failure);
    assert!(err.to_string().contains("disagrees"), "got: {err}");
}

// ----------------------------------------------------- バイナリ経由の検証

mod through_the_binary {
    use super::*;
    use std::process::Command;

    fn run(port: u16, args: &[&str]) -> std::process::Output {
        Command::new(env!("CARGO_BIN_EXE_stackchan"))
            .args(args)
            .env("STACKCHAN_HOST", "127.0.0.1")
            .env("STACKCHAN_PORT", port.to_string())
            .output()
            .expect("failed to run the stackchan binary")
    }

    #[test]
    fn a_snapshot_prints_the_registers() {
        let device = MockDebug::new(Behavior::Snapshot { with_waves: false });
        let out = run(device.port, &["debug", "snapshot"]);

        assert_eq!(out.status.code(), Some(0));
        let text = String::from_utf8(out.stdout).expect("utf-8");
        assert!(text.contains("PC=8123"), "got: {text}");
        assert!(text.contains("frame=1234"), "got: {text}");
    }

    #[test]
    fn a_snapshot_can_be_read_as_json() {
        let device = MockDebug::new(Behavior::Snapshot { with_waves: false });
        let out = run(device.port, &["debug", "snapshot", "--json"]);

        assert_eq!(out.status.code(), Some(0));
        let text = String::from_utf8(out.stdout).expect("utf-8");
        assert!(text.starts_with("{\"ok\":true,\"cpu\":{"), "got: {text}");
        assert!(text.contains("\"pc\":33059"), "got: {text}");
    }

    #[test]
    fn work_ram_is_dumped_with_addresses() {
        let device = MockDebug::new(Behavior::Snapshot { with_waves: false });
        let out = run(device.port, &["debug", "wram", "--length", "32"]);

        assert_eq!(out.status.code(), Some(0));
        let text = String::from_utf8(out.stdout).expect("utf-8");
        assert!(text.starts_with("0000  BE EF"), "got: {text}");
        assert_eq!(text.lines().count(), 2, "32 bytes is two rows of 16");
    }

    /// 範囲外の offset は**送る前に**弾く。要求してから断ると、無応答の機体に
    /// 対して「使用法エラー」ではなく「タイムアウト」が返ることになる
    #[test]
    fn a_wram_offset_beyond_the_end_is_refused_without_asking_the_device() {
        // 無応答のモックを使う。送っていればタイムアウト (3) になる
        let device = MockDebug::new(Behavior::Silent);
        let out = run(device.port, &["debug", "wram", "--offset", "0x2000"]);

        assert_eq!(out.status.code(), Some(2), "it must be a usage error");
        let text = String::from_utf8(out.stderr).expect("utf-8");
        assert!(text.starts_with("stackchan: "), "got: {text}");
        assert_eq!(device.request_count(), 0, "nothing should have been sent");
    }

    /// `--raw` は線に載っていたのと同じバイト列を出す。
    ///
    /// 一部だけ書くと、パイプの先が `m5stack/README.md` のレイアウト表どおりに
    /// 読めない (以前は PC 2 バイトとコード窓 48 バイトが落ちていた)
    #[test]
    fn raw_output_is_the_whole_wire_body() {
        let device = MockDebug::new(Behavior::Snapshot { with_waves: false });
        let out = run(device.port, &["debug", "snapshot", "--raw"]);

        assert_eq!(out.status.code(), Some(0));
        assert_eq!(
            out.stdout.len(),
            DEBUG_SNAPSHOT_SIZE,
            "the raw snapshot must be the documented length"
        );
        // モックが送ったものと 1 バイトも違わないこと
        assert_eq!(out.stdout, build_body(DEBUG_SNAPSHOT_SIZE));
    }

    #[test]
    fn a_silent_device_exits_three() {
        let device = MockDebug::new(Behavior::Silent);
        let out = run(device.port, &["debug", "snapshot"]);
        assert_eq!(out.status.code(), Some(3));
    }
}
