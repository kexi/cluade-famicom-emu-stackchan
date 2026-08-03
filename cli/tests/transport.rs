//! ループバックの UDP サーバに対して、再送とタイムアウトの挙動を確かめる。
//!
//! ここが段階3 のモックデバイスの土台になる。実機でパケットロスを故意に
//! 起こすのは難しいので、落とす・遅らせる・無関係なものを混ぜるといった
//! 状況はこちらで作るほうが確実に踏める。

use std::net::UdpSocket;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use stackchan::exit::ExitCode;
use stackchan::transport::{Device, PartOutcome, Retry, TransportError};

/// 受け取ったリクエストに対して、あらかじめ決めた応答を返すサーバ。
///
/// `replies` の n 番目が n 回目のリクエストへの応答。`None` は無応答 (再送を誘発)
struct FakeDevice {
    port: u16,
    received: Arc<AtomicUsize>,
    _handle: thread::JoinHandle<()>,
}

impl FakeDevice {
    fn new(replies: Vec<Option<Vec<Vec<u8>>>>) -> Self {
        let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
        let port = socket.local_addr().expect("addr").port();
        socket
            .set_read_timeout(Some(Duration::from_secs(5)))
            .expect("timeout");

        let received = Arc::new(AtomicUsize::new(0));
        let counter = Arc::clone(&received);
        let handle = thread::spawn(move || {
            let mut buffer = [0u8; 2048];
            for reply in replies {
                let Ok((_, from)) = socket.recv_from(&mut buffer) else {
                    return;
                };
                counter.fetch_add(1, Ordering::SeqCst);
                let Some(datagrams) = reply else {
                    continue; // 無応答
                };
                for datagram in datagrams {
                    let _ = socket.send_to(&datagram, from);
                }
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

    fn request_count(&self) -> usize {
        self.received.load(Ordering::SeqCst)
    }
}

fn short() -> Duration {
    Duration::from_millis(300)
}

#[test]
fn a_matching_reply_ends_the_wait_immediately() {
    let server = FakeDevice::new(vec![Some(vec![b"pong".to_vec()])]);
    let mut device = server.device();

    let start = std::time::Instant::now();
    let got: Vec<u8> = device
        .request(
            b"ping",
            Duration::from_secs(5),
            Retry::Idempotent { attempts: 1 },
            |reply| (reply == b"pong").then(|| reply.to_vec()),
        )
        .expect("should have received the reply");

    assert_eq!(got, b"pong");
    // 締切を待たずに返ること
    assert!(start.elapsed() < Duration::from_secs(2));
}

/// 無関係なデータグラムで締切を消費しない。消費すると、別の送信元からの
/// 1 発で本来の ACK を取り逃す
#[test]
fn unrelated_datagrams_do_not_consume_the_window() {
    let server = FakeDevice::new(vec![Some(vec![
        b"noise".to_vec(),
        b"more noise".to_vec(),
        b"pong".to_vec(),
    ])]);
    let mut device = server.device();

    let got: Vec<u8> = device
        .request(
            b"ping",
            Duration::from_secs(2),
            Retry::Idempotent { attempts: 1 },
            |reply| (reply == b"pong").then(|| reply.to_vec()),
        )
        .expect("should have skipped the noise and found the reply");
    assert_eq!(got, b"pong");
}

/// 無関係なデータグラムが届き続けても締切は延びない。1 回の待ちごとに
/// 固定のタイムアウトを設定し直すと、ノイズが来るたびに待ち時間が伸びて
/// いつまでも返らなくなる
#[test]
fn a_steady_stream_of_noise_does_not_extend_the_deadline() {
    let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
    let port = socket.local_addr().expect("addr").port();
    socket
        .set_read_timeout(Some(Duration::from_secs(5)))
        .expect("timeout");

    thread::spawn(move || {
        let mut buffer = [0u8; 2048];
        let Ok((_, from)) = socket.recv_from(&mut buffer) else {
            return;
        };
        // 締切をまたいでノイズを送り続ける。答えは決して返さない
        for _ in 0..40 {
            let _ = socket.send_to(b"noise", from);
            thread::sleep(Duration::from_millis(25));
        }
    });

    let timeout = Duration::from_millis(300);
    let mut device = Device::new("127.0.0.1", port, 0).expect("device");

    let start = std::time::Instant::now();
    let result: Result<Vec<u8>, _> =
        device.request(b"ping", timeout, Retry::Idempotent { attempts: 1 }, |_| {
            None
        });
    let elapsed = start.elapsed();

    assert!(result.is_err(), "noise must not be taken for an answer");
    // 締切どおりに諦めること。延びていれば 1 秒 (40 x 25ms) 近くかかる
    assert!(
        elapsed < timeout * 3,
        "the deadline was extended by the noise: {elapsed:?}"
    );
}

#[test]
fn an_idempotent_request_is_retried_the_requested_number_of_times() {
    // 2 回落として 3 回目に答える
    let server = FakeDevice::new(vec![None, None, Some(vec![b"pong".to_vec()])]);
    let mut device = server.device();

    let got: Vec<u8> = device
        .request(
            b"ping",
            short(),
            Retry::Idempotent { attempts: 3 },
            |reply| (reply == b"pong").then(|| reply.to_vec()),
        )
        .expect("the third attempt should have been answered");
    assert_eq!(got, b"pong");
    assert_eq!(server.request_count(), 3);
}

#[test]
fn an_idempotent_request_that_is_never_answered_reports_a_timeout() {
    let server = FakeDevice::new(vec![None, None]);
    let mut device = server.device();

    let result: Result<Vec<u8>, _> =
        device.request(b"ping", short(), Retry::Idempotent { attempts: 2 }, |_| {
            None
        });

    let err = result.err().expect("should have timed out");
    assert!(matches!(err, TransportError::Timeout));
    assert_eq!(err.exit_code(), ExitCode::Timeout);
    assert_eq!(server.request_count(), 2);
}

/// 冪等でない操作は 1 回しか送らない。再送すると、成功した DELETE の
/// 2 回目が NotFound を返して成功が失敗に化ける
#[test]
fn a_non_idempotent_request_is_sent_once_and_reports_an_unknown_outcome() {
    let server = FakeDevice::new(vec![None, None, None]);
    let mut device = server.device();

    let result: Result<Vec<u8>, _> = device.request(b"delete", short(), Retry::Once, |_| None);

    let err = result.err().expect("should not have got an answer");
    assert!(matches!(err, TransportError::Unknown));
    assert_eq!(err.exit_code(), ExitCode::Unknown);
    assert_eq!(server.request_count(), 1, "it must not be retransmitted");
}

/// 試行をまたいでパートを混ぜない。試行 1 の part 0 と試行 2 の part 1 を
/// 繋ぐと、別々の時点のデータを 1 つの答えとして返すことになる
#[test]
fn collected_parts_are_discarded_between_attempts() {
    // 1 回目は part 0 だけ、2 回目は両方
    let server = FakeDevice::new(vec![
        Some(vec![b"A0".to_vec()]),
        Some(vec![b"B0".to_vec(), b"B1".to_vec()]),
    ]);
    let mut device = server.device();

    let attempts_seen = Arc::new(Mutex::new(Vec::new()));
    let recorder = Arc::clone(&attempts_seen);

    let got: Vec<Vec<u8>> = device
        .request_parts(b"list", short(), Retry::Idempotent { attempts: 2 }, || {
            // 試行ごとに新しい collector が作られるので、集めかけは持ち越さない
            let mut parts: Vec<Vec<u8>> = Vec::new();
            let recorder = Arc::clone(&recorder);
            move |reply: &[u8]| {
                parts.push(reply.to_vec());
                recorder.lock().unwrap().push(parts.len());
                let is_complete = parts.len() == 2;
                if is_complete {
                    return PartOutcome::Done(parts.clone());
                }
                PartOutcome::NeedMore
            }
        })
        .expect("the second attempt sent both parts");

    // 1 回目の A0 が混ざっていないこと
    assert_eq!(got, vec![b"B0".to_vec(), b"B1".to_vec()]);
    // 2 回目は 1 から数え直している (持ち越していれば 2, 3 になる)
    assert_eq!(*attempts_seen.lock().unwrap(), vec![1, 1, 2]);
}

/// 遅れて届いた前の試行のパートを、新しい試行に混ぜない。
///
/// collector を作り直すだけでは足りない — ソケットを共有していると、試行 1 の
/// パートが遅れて到着したときに試行 2 の collector が受け取ってしまう。
/// リクエストは seq まで同一なので collector 側では区別できない
#[test]
fn a_late_part_from_a_previous_attempt_cannot_reach_the_next_one() {
    let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
    let port = socket.local_addr().expect("addr").port();
    socket
        .set_read_timeout(Some(Duration::from_secs(5)))
        .expect("timeout");

    let timeout = Duration::from_millis(300);
    thread::spawn(move || {
        let mut buffer = [0u8; 2048];

        // 試行 1: 締切を過ぎてから A0 を送る (遅延したパート)
        let Ok((_, first)) = socket.recv_from(&mut buffer) else {
            return;
        };
        let late = socket.try_clone().expect("clone");
        thread::spawn(move || {
            thread::sleep(timeout + Duration::from_millis(150));
            let _ = late.send_to(b"A0", first);
        });

        // 試行 2: B1 だけを返す。混ざれば 2 パート揃って誤って成功する
        let Ok((_, second)) = socket.recv_from(&mut buffer) else {
            return;
        };
        let _ = socket.send_to(b"B1", second);
    });

    let mut device = Device::new("127.0.0.1", port, 0).expect("device");
    let result: Result<Vec<Vec<u8>>, _> =
        device.request_parts(b"list", timeout, Retry::Idempotent { attempts: 2 }, || {
            let mut parts: Vec<Vec<u8>> = Vec::new();
            move |reply: &[u8]| {
                parts.push(reply.to_vec());
                let is_complete = parts.len() == 2;
                if is_complete {
                    return PartOutcome::Done(parts.clone());
                }
                PartOutcome::NeedMore
            }
        });

    // 揃わないので timeout になるのが正しい。A0 が混ざると Ok になってしまう
    let err = result.err().expect("a late part must not complete the set");
    assert!(matches!(err, TransportError::Timeout), "got {err:?}");
}

/// デバイスが理由を添えて断ったら、タイムアウトに丸めずその理由を返す
#[test]
fn a_refusal_is_reported_instead_of_a_timeout() {
    let server = FakeDevice::new(vec![Some(vec![b"nope".to_vec()])]);
    let mut device = server.device();

    let result: Result<Vec<u8>, _> =
        device.request_parts(b"list", short(), Retry::Idempotent { attempts: 3 }, || {
            |reply: &[u8]| {
                let is_refusal = reply == b"nope";
                if is_refusal {
                    return PartOutcome::Refused("no SD card".to_string());
                }
                PartOutcome::Ignore
            }
        });

    let err = result.err().expect("should have been refused");
    assert!(matches!(err, TransportError::Refused(_)), "got {err:?}");
    assert_eq!(err.exit_code(), ExitCode::Failure);
    assert_eq!(err.to_string(), "no SD card");
    // 断られたら再送しない
    assert_eq!(server.request_count(), 1);
}

/// 分割応答の試行ごとに送信元ポートが変わる。
///
/// UDP には TIME_WAIT が無いので、閉じた直後の bind に OS が同じポートを
/// 返しうる。実装が諦めた試行のソケットを保持していないと、ここは実装が
/// 正しくても時々失敗する = 保証になっていない
#[test]
fn each_attempt_of_a_split_reply_uses_a_different_source_port() {
    let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
    let port = socket.local_addr().expect("addr").port();
    socket
        .set_read_timeout(Some(Duration::from_secs(5)))
        .expect("timeout");

    let ports = Arc::new(Mutex::new(Vec::new()));
    let recorder = Arc::clone(&ports);
    thread::spawn(move || {
        let mut buffer = [0u8; 2048];
        // 3 回とも無応答にして、3 試行ぶんの送信元ポートを集める
        for _ in 0..3 {
            let Ok((_, from)) = socket.recv_from(&mut buffer) else {
                return;
            };
            recorder.lock().unwrap().push(from.port());
        }
    });

    let mut device = Device::new("127.0.0.1", port, 0).expect("device");
    let _: Result<Vec<u8>, _> = device.request_parts(
        b"list",
        Duration::from_millis(150),
        Retry::Idempotent { attempts: 3 },
        || |_: &[u8]| PartOutcome::NeedMore,
    );

    let seen = ports.lock().unwrap();
    assert_eq!(seen.len(), 3, "every attempt must have been sent");
    let mut unique = seen.clone();
    unique.sort_unstable();
    unique.dedup();
    assert_eq!(
        unique.len(),
        seen.len(),
        "a retried attempt reused a source port, so a late part could slip in: {seen:?}"
    );
}

/// 応答待ちはリクエストごとに新しいソケットを使う。共有すると、前の
/// リクエストへの遅れた応答を今の答えとして拾う。
///
/// 応答の中身だけを見ても共有ソケットで通ってしまうので、**送信元ポートが
/// 変わっていること**を直接確かめる
#[test]
fn each_request_comes_from_a_different_source_port() {
    let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
    let port = socket.local_addr().expect("addr").port();
    socket
        .set_read_timeout(Some(Duration::from_secs(5)))
        .expect("timeout");

    let ports = Arc::new(Mutex::new(Vec::new()));
    let recorder = Arc::clone(&ports);
    thread::spawn(move || {
        let mut buffer = [0u8; 2048];
        for _ in 0..2 {
            let Ok((_, from)) = socket.recv_from(&mut buffer) else {
                return;
            };
            recorder.lock().unwrap().push(from.port());
            let _ = socket.send_to(b"ok", from);
        }
    });

    let mut device = Device::new("127.0.0.1", port, 0).expect("device");
    for _ in 0..2 {
        let _: Vec<u8> = device
            .request(b"a", short(), Retry::Idempotent { attempts: 1 }, |r| {
                Some(r.to_vec())
            })
            .expect("reply");
    }

    let seen = ports.lock().unwrap();
    assert_eq!(seen.len(), 2);
    assert_ne!(
        seen[0], seen[1],
        "each request must use its own socket, got {seen:?}"
    );
}

#[test]
fn each_request_gets_its_own_reply() {
    let server = FakeDevice::new(vec![
        Some(vec![b"first".to_vec()]),
        Some(vec![b"second".to_vec()]),
    ]);
    let mut device = server.device();

    let first: Vec<u8> = device
        .request(b"a", short(), Retry::Idempotent { attempts: 1 }, |r| {
            Some(r.to_vec())
        })
        .expect("first");
    assert_eq!(first, b"first");

    // 2 回目が 1 回目の応答を拾わないこと
    let second: Vec<u8> = device
        .request(b"b", short(), Retry::Idempotent { attempts: 1 }, |r| {
            Some(r.to_vec())
        })
        .expect("second");
    assert_eq!(second, b"second");
}
