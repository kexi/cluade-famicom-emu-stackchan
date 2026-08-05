//! ROM 転送を、応答を制御できるモックデバイスに対して検証する。
//!
//! 見たいのは方針:
//! - 順序ずれは ACK の `expected` まで巻き戻して続ける (中断しない)
//! - 保存の結果は END の ACK とは別のデータグラムで、**同じソケット**に返る
//! - 保存を頼んだなら、判定は保存の結果に従う (イメージが届いただけでは足りない)

use std::collections::HashMap;
use std::net::UdpSocket;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use stackchan::exit::ExitCode;
use stackchan::proto::constants::{ROM_CHUNK, ROM_OP_BEGIN, ROM_OP_DATA, ROM_OP_END};
use stackchan::proto::rom::RomOptions;
use stackchan::rom_client::{self, RomError};
use stackchan::transport::Device;

/// 1 回のリクエストにどう答えるか
#[derive(Clone, Debug)]
enum Reply {
    /// 素直に OK
    Ok,
    /// 無応答 (再送を誘発)
    Drop,
    /// このステータスを返す
    Status(u8),
    /// status=4 (Seq) と、巻き戻し先を返す
    Rewind(u16),
}

/// ROM 転送に応答するモックデバイス
struct MockRom {
    port: u16,
    /// op ごとの受信回数
    counts: Arc<Mutex<HashMap<u8, usize>>>,
    /// 受け取った DATA のチャンク番号 (巻き戻しの確認用)
    data_indexes: Arc<Mutex<Vec<u16>>>,
    /// 受け取ったペイロードを繋いだもの
    received: Arc<Mutex<HashMap<u16, Vec<u8>>>>,
    total_requests: Arc<AtomicUsize>,
    /// (op, 送信元ポート)。同じソケットで通していることの確認用
    source_ports: Arc<Mutex<Vec<(u8, u16)>>>,
    _handle: thread::JoinHandle<()>,
}

/// モックの応答方針
struct Script {
    /// DATA の n 回目にどう答えるか (足りなければ Ok)
    data: Vec<Reply>,
    /// BEGIN への答え
    begin: Reply,
    /// END への答え
    end: Reply,
    /// END の後に返す保存イベントのステータス。`None` なら返さない
    save_event: Option<u8>,
    /// 保存イベントを返すまでの遅れ
    save_delay: Duration,
}

impl Default for Script {
    fn default() -> Self {
        Self {
            data: Vec::new(),
            begin: Reply::Ok,
            end: Reply::Ok,
            save_event: None,
            save_delay: Duration::ZERO,
        }
    }
}

impl MockRom {
    fn new(script: Script) -> Self {
        let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
        let port = socket.local_addr().expect("addr").port();
        socket
            .set_read_timeout(Some(Duration::from_secs(20)))
            .expect("timeout");

        let counts = Arc::new(Mutex::new(HashMap::new()));
        let data_indexes = Arc::new(Mutex::new(Vec::new()));
        let received = Arc::new(Mutex::new(HashMap::new()));
        let total_requests = Arc::new(AtomicUsize::new(0));
        let source_ports = Arc::new(Mutex::new(Vec::new()));

        let c = Arc::clone(&counts);
        let idx = Arc::clone(&data_indexes);
        let rx = Arc::clone(&received);
        let total = Arc::clone(&total_requests);

        let sources = Arc::clone(&source_ports);
        let handle = thread::spawn(move || {
            let mut buffer = [0u8; 2048];
            let mut data_seen = 0usize;
            // firmware は BEGIN が来たポートを覚えて、保存イベントをそこへ返す。
            // END の送信元に返すと、途中でソケットを開き直す実装でもテストが
            // 通ってしまい、Session を足した理由を固定できない
            let mut begin_from = None;

            loop {
                let Ok((n, from)) = socket.recv_from(&mut buffer) else {
                    return;
                };
                let request = &buffer[..n];
                total.fetch_add(1, Ordering::SeqCst);
                sources.lock().unwrap().push((request[6], from.port()));

                let session = u16::from_le_bytes([request[4], request[5]]);
                let op = request[6];
                *c.lock().unwrap().entry(op).or_insert(0) += 1;

                let (status, expected, chunk) = match op {
                    ROM_OP_BEGIN => {
                        begin_from.get_or_insert(from);
                        reply_of(&script.begin, 0)
                    }
                    ROM_OP_DATA => {
                        let index = u16::from_le_bytes([request[8], request[9]]);
                        idx.lock().unwrap().push(index);
                        let len = u16::from_le_bytes([request[10], request[11]]) as usize;
                        rx.lock()
                            .unwrap()
                            .insert(index, request[12..12 + len].to_vec());

                        let reply = script.data.get(data_seen).cloned().unwrap_or(Reply::Ok);
                        data_seen += 1;
                        reply_of(&reply, index)
                    }
                    ROM_OP_END => reply_of(&script.end, 0),
                    _ => (0, 0, 0),
                };

                let is_dropped = status == u8::MAX;
                if is_dropped {
                    continue;
                }

                let s = session.to_le_bytes();
                let ch = chunk.to_le_bytes();
                let ex = expected.to_le_bytes();
                let ack = [
                    b'N', b'R', 1, op, s[0], s[1], ch[0], ch[1], status, ex[0], ex[1], 0,
                ];
                let _ = socket.send_to(&ack, from);

                // 保存イベントは END の ACK とは別のデータグラムで、同じ
                // ソケット (= BEGIN が来たポート) に返る
                let is_end = op == ROM_OP_END;
                if is_end {
                    if let Some(sd_status) = script.save_event {
                        thread::sleep(script.save_delay);
                        let event = [b'N', b'S', 1, 4, s[0], s[1], sd_status, 0];
                        // BEGIN の送信元へ返す (firmware と同じ)
                        let target = begin_from.unwrap_or(from);
                        let _ = socket.send_to(&event, target);
                    }
                }
            }
        });

        Self {
            port,
            counts,
            data_indexes,
            received,
            total_requests,
            source_ports,
            _handle: handle,
        }
    }

    fn device(&self) -> Device {
        Device::new("127.0.0.1", self.port, 0).expect("device")
    }

    fn count(&self, op: u8) -> usize {
        *self.counts.lock().unwrap().get(&op).unwrap_or(&0)
    }

    fn data_indexes(&self) -> Vec<u16> {
        self.data_indexes.lock().unwrap().clone()
    }

    /// 受け取ったチャンクを順に繋いだもの
    fn assembled(&self) -> Vec<u8> {
        let map = self.received.lock().unwrap();
        let mut keys: Vec<&u16> = map.keys().collect();
        keys.sort();
        keys.iter().flat_map(|k| map[k].clone()).collect()
    }

    fn total_requests(&self) -> usize {
        self.total_requests.load(Ordering::SeqCst)
    }

    /// 転送に使われた送信元ポート (重複を除く)
    fn distinct_source_ports(&self) -> Vec<u16> {
        let mut ports: Vec<u16> = self
            .source_ports
            .lock()
            .unwrap()
            .iter()
            .map(|(_, port)| *port)
            .collect();
        ports.sort_unstable();
        ports.dedup();
        ports
    }
}

/// `(status, expected, chunk)`。status == u8::MAX は「返さない」
fn reply_of(reply: &Reply, index: u16) -> (u8, u16, u16) {
    match reply {
        Reply::Ok => (0, index.saturating_add(1), index),
        Reply::Drop => (u8::MAX, 0, 0),
        Reply::Status(s) => (*s, 0, index),
        Reply::Rewind(to) => (4, *to, index),
    }
}

fn image(size: usize) -> Vec<u8> {
    (0..size).map(|i| (i % 251) as u8).collect()
}

// -------------------------------------------------------------- 素直な転送

#[test]
fn a_small_image_is_sent_and_acknowledged() {
    let device = MockRom::new(Script::default());
    let data = image(100);

    let transfer = rom_client::send(&mut device.device(), &data, &RomOptions::default(), None)
        .expect("transfer");

    assert_eq!(transfer.bytes, 100);
    assert_eq!(transfer.chunks, 1);
    assert_eq!(transfer.retries, 0);
    assert_eq!(device.count(ROM_OP_BEGIN), 1);
    assert_eq!(device.count(ROM_OP_END), 1);
    // 中身がそのまま届いていること
    assert_eq!(device.assembled(), data);
}

/// チャンクをまたぐ大きさでも、順に全部届く
#[test]
fn a_multi_chunk_image_arrives_intact() {
    let device = MockRom::new(Script::default());
    let data = image(ROM_CHUNK * 3 + 7);

    let transfer = rom_client::send(&mut device.device(), &data, &RomOptions::default(), None)
        .expect("transfer");

    assert_eq!(transfer.chunks, 4);
    assert_eq!(device.count(ROM_OP_DATA), 4);
    assert_eq!(device.assembled(), data, "the image must arrive unchanged");
    assert_eq!(device.data_indexes(), vec![0, 1, 2, 3]);
}

#[test]
fn progress_is_reported_for_every_chunk() {
    let device = MockRom::new(Script::default());
    let data = image(ROM_CHUNK * 3);

    let mut seen = Vec::new();
    let mut report = |sent: usize, total: usize| seen.push((sent, total));
    rom_client::send(
        &mut device.device(),
        &data,
        &RomOptions::default(),
        Some(&mut report),
    )
    .expect("transfer");

    // 開始 (0) と各チャンクの完了
    assert_eq!(seen, vec![(0, 3), (1, 3), (2, 3), (3, 3)]);
}

// ---------------------------------------------------------------- 再送

#[test]
fn a_lost_ack_is_retried() {
    let device = MockRom::new(Script {
        data: vec![Reply::Drop, Reply::Drop],
        ..Default::default()
    });

    let transfer = rom_client::send(
        &mut device.device(),
        &image(100),
        &RomOptions::default(),
        None,
    )
    .expect("the third attempt was answered");

    assert!(transfer.retries > 0, "the retries should be reported");
    assert_eq!(device.count(ROM_OP_DATA), 3);
}

/// **順序がずれたら巻き戻して続ける。** 中断より巻き戻しがよいのは、原因の
/// 多くが「ACK を落として 1 つ先に進んでしまった」だから
#[test]
fn an_out_of_order_chunk_rewinds_instead_of_aborting() {
    let device = MockRom::new(Script {
        // 3 チャンク目で「0 番からやり直せ」と言われる
        data: vec![Reply::Ok, Reply::Ok, Reply::Rewind(0)],
        ..Default::default()
    });
    let data = image(ROM_CHUNK * 3);

    let transfer = rom_client::send(&mut device.device(), &data, &RomOptions::default(), None)
        .expect("the transfer should have recovered");

    let indexes = device.data_indexes();
    // 0,1,2 と送ったあと 0 に戻って送り直す
    assert_eq!(indexes[..3], [0, 1, 2]);
    assert_eq!(indexes[3], 0, "it should have rewound to 0: {indexes:?}");
    assert!(transfer.retries > 0);
}

/// **遅れて届いた前のチャンクの ACK を、今のチャンクの答えにしない。**
///
/// 照合しないと、ストップ&ウェイトのつもりが 1 つずれたまま進み、最後に
/// SizeMismatch で落ちる。ここでは chunk 1 の ACK を落とし、その待ちの間に
/// chunk 0 の ACK を遅れて届かせる
#[test]
fn a_late_ack_for_an_earlier_chunk_is_not_taken_as_this_ones() {
    let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
    let port = socket.local_addr().expect("addr").port();
    socket
        .set_read_timeout(Some(Duration::from_secs(10)))
        .expect("timeout");

    let data_sends = Arc::new(AtomicUsize::new(0));
    let counter = Arc::clone(&data_sends);

    thread::spawn(move || {
        let mut buffer = [0u8; 2048];
        loop {
            let Ok((n, from)) = socket.recv_from(&mut buffer) else {
                return;
            };
            let request = &buffer[..n];
            let session = u16::from_le_bytes([request[4], request[5]]);
            let op = request[6];
            let s = session.to_le_bytes();

            let ack = |chunk: u16, expected: u16| {
                let c = chunk.to_le_bytes();
                let e = expected.to_le_bytes();
                [b'N', b'R', 1, op, s[0], s[1], c[0], c[1], 0, e[0], e[1], 0]
            };

            let is_data = op == ROM_OP_DATA;
            if !is_data {
                let _ = socket.send_to(&ack(0, 0), from);
                continue;
            }

            let index = u16::from_le_bytes([request[8], request[9]]);
            let seen = counter.fetch_add(1, Ordering::SeqCst);

            // chunk 1 の 1 回目だけ、答えの代わりに chunk 0 の ACK を遅れて返す
            let is_the_stale_moment = index == 1 && seen == 1;
            if is_the_stale_moment {
                let _ = socket.send_to(&ack(0, 1), from);
                continue;
            }
            let _ = socket.send_to(&ack(index, index + 1), from);
        }
    });

    let mut device = Device::new("127.0.0.1", port, 0).expect("device");
    let transfer = rom_client::send(
        &mut device,
        &image(ROM_CHUNK * 2),
        &RomOptions::default(),
        None,
    )
    .expect("the transfer should recover by resending chunk 1");

    // 古い ACK を受け入れていれば再送は起きない
    assert!(
        transfer.retries > 0,
        "the stale ack was accepted as chunk 1's answer"
    );
}

/// **遅れて届いた SEQ も、番号が違えば今の答えではない。**
///
/// firmware は順序ずれの通知でも受け取ったチャンク番号をそのままエコーする
/// (`main.cpp:395`) ので、番号が違う SEQ は前の DATA への遅れた ACK。受けると
/// 古い `expected` へ飛ばされる
#[test]
fn a_late_seq_ack_for_an_earlier_chunk_is_not_followed() {
    let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
    let port = socket.local_addr().expect("addr").port();
    socket
        .set_read_timeout(Some(Duration::from_secs(10)))
        .expect("timeout");

    let sends = Arc::new(AtomicUsize::new(0));
    let counter = Arc::clone(&sends);

    thread::spawn(move || {
        let mut buffer = [0u8; 2048];
        loop {
            let Ok((n, from)) = socket.recv_from(&mut buffer) else {
                return;
            };
            let request = &buffer[..n];
            let session = u16::from_le_bytes([request[4], request[5]]);
            let op = request[6];
            let s = session.to_le_bytes();

            let ack = |chunk: u16, status: u8, expected: u16| {
                let c = chunk.to_le_bytes();
                let e = expected.to_le_bytes();
                [
                    b'N', b'R', 1, op, s[0], s[1], c[0], c[1], status, e[0], e[1], 0,
                ]
            };

            let is_data = op == ROM_OP_DATA;
            if !is_data {
                let _ = socket.send_to(&ack(0, 0, 0), from);
                continue;
            }

            let index = u16::from_le_bytes([request[8], request[9]]);
            let seen = counter.fetch_add(1, Ordering::SeqCst);

            // chunk 2 の 1 回目に、chunk 0 についての SEQ を返す。巻き戻し先は
            // 末尾を越える値にしておく — 従えば「末尾を越える巻き戻し」として
            // 転送が失敗するので、無視できているかどうかがはっきり分かれる
            let is_the_stale_moment = index == 2 && seen == 2;
            if is_the_stale_moment {
                let _ = socket.send_to(&ack(0, 4, 9999), from);
                continue;
            }
            let _ = socket.send_to(&ack(index, 0, index + 1), from);
        }
    });

    let mut device = Device::new("127.0.0.1", port, 0).expect("device");
    let transfer = rom_client::send(
        &mut device,
        &image(ROM_CHUNK * 3),
        &RomOptions::default(),
        None,
    )
    .expect("the stale SEQ must be ignored, not followed");

    // 無視できていれば chunk 2 を送り直して完走する
    assert!(
        transfer.retries > 0,
        "chunk 2 should have been resent after the stale SEQ was ignored"
    );
}

/// 巻き戻し先が末尾を越えていたら、そこで止める。従うと配列の外を読む
#[test]
fn a_rewind_past_the_end_is_refused() {
    let device = MockRom::new(Script {
        data: vec![Reply::Rewind(999)],
        ..Default::default()
    });

    let err = rom_client::send(
        &mut device.device(),
        &image(100),
        &RomOptions::default(),
        None,
    )
    .expect_err("a rewind past the end must not be followed");
    assert_eq!(err.exit_code(), ExitCode::Failure);
}

/// 巻き戻しが続いたら諦める。従い続けると終わらない
#[test]
fn endless_rewinding_gives_up() {
    let device = MockRom::new(Script {
        data: vec![Reply::Rewind(0); 200],
        ..Default::default()
    });

    let err = rom_client::send(
        &mut device.device(),
        &image(ROM_CHUNK * 2),
        &RomOptions::default(),
        None,
    )
    .expect_err("it must not loop forever");

    // デバイスは毎回答えているので「届かない」(3) ではなく失敗 (1)
    assert!(matches!(err, RomError::NotConverging), "got {err:?}");
    assert_eq!(err.exit_code(), ExitCode::Failure);
}

// -------------------------------------------------------------- 拒否

#[test]
fn a_refused_begin_stops_the_transfer() {
    // 2 = TooBig
    let device = MockRom::new(Script {
        begin: Reply::Status(2),
        ..Default::default()
    });

    let err = rom_client::send(
        &mut device.device(),
        &image(100),
        &RomOptions::default(),
        None,
    )
    .expect_err("should have been refused");

    assert_eq!(err.exit_code(), ExitCode::Failure);
    assert!(err.to_string().contains("larger than"), "got: {err}");
    // BEGIN で断られたらチャンクは送らない
    assert_eq!(device.count(ROM_OP_DATA), 0);
}

#[test]
fn a_checksum_failure_at_the_end_is_reported() {
    // 6 = Crc
    let device = MockRom::new(Script {
        end: Reply::Status(6),
        ..Default::default()
    });

    let err = rom_client::send(
        &mut device.device(),
        &image(100),
        &RomOptions::default(),
        None,
    )
    .expect_err("should have failed the checksum");
    assert!(err.to_string().contains("checksum"), "got: {err}");
}

#[test]
fn a_silent_device_times_out() {
    let device = MockRom::new(Script {
        begin: Reply::Drop,
        ..Default::default()
    });

    let err = rom_client::send(
        &mut device.device(),
        &image(100),
        &RomOptions::default(),
        None,
    )
    .expect_err("should have timed out");
    assert_eq!(err.exit_code(), ExitCode::Timeout);
}

// ------------------------------------------------------------ SD 保存

/// 保存の結果は END の ACK とは別のデータグラムで、同じソケットに返る
#[test]
fn a_save_result_arrives_after_the_end_ack() {
    let device = MockRom::new(Script {
        save_event: Some(0), // Ok
        ..Default::default()
    });

    let options = RomOptions {
        save_as: Some("game.nes".to_string()),
        ..Default::default()
    };
    let transfer =
        rom_client::send(&mut device.device(), &image(100), &options, None).expect("transfer");

    assert!(transfer.is_ok());
    assert!(matches!(transfer.saved, Some(Some(status)) if status.is_ok()));
}

/// カードへの書き込みは core 1 のフレーム境界で走るので、ACK より遅れて返る
#[test]
fn a_delayed_save_result_is_still_collected() {
    let device = MockRom::new(Script {
        save_event: Some(0),
        save_delay: Duration::from_millis(600),
        ..Default::default()
    });

    let options = RomOptions {
        save_as: Some("game.nes".to_string()),
        ..Default::default()
    };
    let transfer =
        rom_client::send(&mut device.device(), &image(100), &options, None).expect("transfer");
    assert!(transfer.is_ok(), "a slow card write must still be seen");
}

/// **保存を頼んだなら、判定は保存の結果に従う。** イメージが届いただけでは
/// 「カードに置く」という要求を満たしていない
#[test]
fn a_failed_save_makes_the_whole_transfer_a_failure() {
    // 3 = NoSpace
    let device = MockRom::new(Script {
        save_event: Some(3),
        ..Default::default()
    });

    let options = RomOptions {
        save_as: Some("game.nes".to_string()),
        ..Default::default()
    };
    let transfer = rom_client::send(&mut device.device(), &image(100), &options, None)
        .expect("the transfer itself succeeded");

    assert!(!transfer.is_ok(), "a failed save must not read as success");
}

/// 保存の返事が来なければ「判らない」。成功とも失敗とも言えない
#[test]
fn a_missing_save_result_is_not_treated_as_success() {
    let device = MockRom::new(Script {
        save_event: None, // 返さない
        ..Default::default()
    });

    let options = RomOptions {
        save_as: Some("game.nes".to_string()),
        ..Default::default()
    };
    let transfer = rom_client::send(&mut device.device(), &image(100), &options, None)
        .expect("the transfer itself succeeded");

    assert!(!transfer.is_ok());
    assert!(matches!(transfer.saved, Some(None)));
}

/// 再送を重ねながらでも、最後まで行けば成功する。
///
/// 「予算を使い切った後に受け取った ACK を失敗に覆さない」という性質そのものは
/// `RetryBudget` の単体テストが見る (ここでは予算の消費量を狙って作れないため)。
/// こちらは、多数の再送を挟んでも転送が完走し、保存イベントまで拾えることを見る
#[test]
fn a_transfer_that_needs_many_retries_still_completes() {
    // 予算 (64) をちょうど使い切るところまで再送を積んでから完走させる。
    //
    // 予算は次の再送を始める前に効かせるので、使い切っても「受け取った ACK」は
    // 有効なまま。ACK の後に予算を見る実装だと、ここで成功済みの転送が
    // TooManyRetries に化ける。9 チャンクを 7 回ずつ落として 63 回 + 端数
    let chunks = 9;
    let mut data_replies = Vec::new();
    for _ in 0..chunks {
        for _ in 0..7 {
            data_replies.push(Reply::Drop);
        }
        data_replies.push(Reply::Ok);
    }

    let device = MockRom::new(Script {
        data: data_replies,
        save_event: Some(0),
        ..Default::default()
    });

    let options = RomOptions {
        save_as: Some("game.nes".to_string()),
        ..Default::default()
    };
    let transfer = rom_client::send(
        &mut device.device(),
        &image(ROM_CHUNK * chunks),
        &options,
        None,
    )
    .expect("a transfer the device completed must not be reported as a failure");

    // 予算 (64) をほぼ使い切っていること。使い切らなければこのテストに
    // 意味がない
    assert_eq!(
        transfer.retries, 63,
        "the transfer should have spent nearly the whole budget"
    );
    // 保存イベントもちゃんと待っていること
    assert!(
        transfer.is_ok(),
        "the save event should have been collected"
    );
}

/// **転送は最初から最後まで同じソケットで通す。** firmware は BEGIN が来た
/// ポートを覚えて保存イベントをそこへ返すので、途中で開き直すと受け取れない
#[test]
fn the_whole_transfer_uses_one_source_port() {
    let device = MockRom::new(Script {
        save_event: Some(0),
        ..Default::default()
    });

    let options = RomOptions {
        save_as: Some("game.nes".to_string()),
        ..Default::default()
    };
    let transfer = rom_client::send(&mut device.device(), &image(ROM_CHUNK * 2), &options, None)
        .expect("transfer");

    assert!(transfer.is_ok(), "the save event must have been collected");
    assert_eq!(
        device.distinct_source_ports().len(),
        1,
        "BEGIN, DATA and END must all come from the same port: {:?}",
        device.distinct_source_ports()
    );
}

/// 保存を頼んでいなければ、保存イベントは待たない
#[test]
fn a_transfer_without_a_save_does_not_wait_for_one() {
    let device = MockRom::new(Script::default());

    let started = std::time::Instant::now();
    rom_client::send(
        &mut device.device(),
        &image(100),
        &RomOptions::default(),
        None,
    )
    .expect("transfer");

    // 保存イベントの待ち (6s) が入っていないこと
    assert!(
        started.elapsed() < Duration::from_secs(2),
        "it waited for a save event it never asked for"
    );
    let _ = device.total_requests();
}

// -------------------------------------------------- バイナリ経由の検証

mod through_the_binary {
    use super::*;
    use std::io::Write;
    use std::process::{Command, Stdio};

    fn binary() -> Command {
        Command::new(env!("CARGO_BIN_EXE_stackchan"))
    }

    #[test]
    fn a_transfer_reports_what_it_sent() {
        let device = MockRom::new(Script::default());
        let path = std::env::temp_dir().join("stackchan-test-rom.nes");
        std::fs::write(&path, image(3000)).expect("write");

        let out = binary()
            .args(["rom", "send", path.to_str().unwrap(), "--json"])
            .env("STACKCHAN_HOST", "127.0.0.1")
            .env("STACKCHAN_PORT", device.port.to_string())
            .output()
            .expect("run");

        assert_eq!(out.status.code(), Some(0));
        let text = String::from_utf8(out.stdout).expect("utf-8");
        assert!(text.contains("\"bytes\":3000"), "got: {text}");
        assert!(text.contains("\"chunks\":3"), "got: {text}");
        let _ = std::fs::remove_file(path);
    }

    /// 標準入力から読めること。`curl ... | stackchan rom send -` の形
    #[test]
    fn an_image_can_be_piped_in() {
        let device = MockRom::new(Script::default());

        let mut child = binary()
            .args(["rom", "send", "-", "--json"])
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
            .write_all(&image(500))
            .expect("write");

        let out = child.wait_with_output().expect("wait");
        assert_eq!(out.status.code(), Some(0));
        let text = String::from_utf8(out.stdout).expect("utf-8");
        assert!(text.contains("\"bytes\":500"), "got: {text}");
    }

    /// 保存先の無い `--no-load` は何もしない転送になるので、送る前に断る
    #[test]
    fn no_load_without_a_save_is_refused_locally() {
        let device = MockRom::new(Script::default());
        let path = std::env::temp_dir().join("stackchan-test-noload.nes");
        std::fs::write(&path, image(100)).expect("write");

        let out = binary()
            .args(["rom", "send", path.to_str().unwrap(), "--no-load"])
            .env("STACKCHAN_HOST", "127.0.0.1")
            .env("STACKCHAN_PORT", device.port.to_string())
            .output()
            .expect("run");

        assert_eq!(out.status.code(), Some(2));
        assert_eq!(device.total_requests(), 0, "nothing should have been sent");
        let _ = std::fs::remove_file(path);
    }

    /// `--progress` の途中で失敗しても、エラーは行頭から始まる。
    /// 進捗の行を閉じ損ねると `\rsending... 33%stackchan: ...` になり、
    /// GNU の `program: message` 書式が崩れる
    #[test]
    fn a_failure_during_progress_still_starts_its_own_line() {
        // BEGIN に答えないので、進捗を出したあとタイムアウトする
        let device = MockRom::new(Script {
            begin: Reply::Drop,
            ..Default::default()
        });
        let path = std::env::temp_dir().join("stackchan-test-progress.nes");
        std::fs::write(&path, image(100)).expect("write");

        let out = binary()
            .args(["rom", "send", path.to_str().unwrap(), "--progress"])
            .env("STACKCHAN_HOST", "127.0.0.1")
            .env("STACKCHAN_PORT", device.port.to_string())
            .output()
            .expect("run");

        assert_eq!(out.status.code(), Some(3));
        let text = String::from_utf8(out.stderr).expect("utf-8");
        let last = text.lines().last().unwrap_or_default();
        assert!(
            last.starts_with("stackchan: "),
            "the error must start its own line, got: {last:?}"
        );
        let _ = std::fs::remove_file(path);
    }

    /// 終わらない標準入力に、既に使用法エラーと判っている指定を繋いでも
    /// 待たされない
    #[test]
    fn an_impossible_request_is_refused_before_reading_stdin() {
        let device = MockRom::new(Script::default());

        let child = binary()
            .args(["rom", "send", "-", "--no-load"])
            .env("STACKCHAN_HOST", "127.0.0.1")
            .env("STACKCHAN_PORT", device.port.to_string())
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("spawn");

        // 何も書かないまま (パイプは開いたまま) 終わることを見る
        let out = child.wait_with_output().expect("wait");
        assert_eq!(out.status.code(), Some(2));
        assert_eq!(device.total_requests(), 0);
    }

    #[test]
    fn a_missing_file_is_reported_before_anything_is_sent() {
        let device = MockRom::new(Script::default());

        let out = binary()
            .args(["rom", "send", "/no/such/rom.nes"])
            .env("STACKCHAN_HOST", "127.0.0.1")
            .env("STACKCHAN_PORT", device.port.to_string())
            .output()
            .expect("run");

        assert_eq!(out.status.code(), Some(1));
        let text = String::from_utf8(out.stderr).expect("utf-8");
        assert!(text.starts_with("stackchan: "), "got: {text}");
        assert_eq!(device.total_requests(), 0);
    }

    /// 保存に失敗したら、転送が通っていても失敗として報告する
    #[test]
    fn a_failed_save_exits_nonzero() {
        let device = MockRom::new(Script {
            save_event: Some(3), // NoSpace
            ..Default::default()
        });
        let path = std::env::temp_dir().join("stackchan-test-save.nes");
        std::fs::write(&path, image(100)).expect("write");

        let out = binary()
            .args(["rom", "send", path.to_str().unwrap(), "--save", "game.nes"])
            .env("STACKCHAN_HOST", "127.0.0.1")
            .env("STACKCHAN_PORT", device.port.to_string())
            .output()
            .expect("run");

        assert_eq!(out.status.code(), Some(1));
        let text = String::from_utf8(out.stderr).expect("utf-8");
        assert!(text.contains("not saved"), "got: {text}");
        let _ = std::fs::remove_file(path);
    }
}
