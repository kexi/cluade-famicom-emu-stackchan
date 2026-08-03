//! SD 操作を、応答を制御できるモックデバイスに対して検証する。
//!
//! 実機でパケットロスや BUSY を故意に起こすのは難しいが、ここでは思いどおりに
//! 作れる。検証したいのは通信の中身ではなく**方針**:
//!
//! - LIST は部分的な結果を返さない (1 パート落ちたら全体を捨てて再送)
//! - DELETE / RENAME は再送しない (成功が失敗に化ける)
//! - BUSY は試行回数を消費しない (答えであって失敗ではない)

use std::net::UdpSocket;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use stackchan::exit::ExitCode;
use stackchan::proto::constants::{SD_LIST_HEADER, SD_OP_LIST};
use stackchan::sd_client::{self, SdError};
use stackchan::transport::{Device, TransportError};

/// n 回目のリクエストにどう答えるか
#[derive(Clone)]
enum Behavior {
    /// 無応答 (再送を誘発)
    Drop,
    /// このステータスの ACK を 1 発返す
    Ack(u8),
    /// LIST の応答を、指定したパートだけ返す
    ListParts {
        /// (part, nparts, エントリ) の並び
        parts: Vec<(u8, u8, Vec<(String, u32)>)>,
        total: u16,
    },
    /// 同じ応答の中に無関係なデータグラムを混ぜてから LIST のパートを返す
    GarbageThenList {
        parts: Vec<(u8, u8, Vec<(String, u32)>)>,
        total: u16,
    },
}

/// 応答を script で制御するモックデバイス
struct MockDevice {
    port: u16,
    requests: Arc<Mutex<Vec<Vec<u8>>>>,
    count: Arc<AtomicUsize>,
    _handle: thread::JoinHandle<()>,
}

impl MockDevice {
    fn new(script: Vec<Behavior>) -> Self {
        let socket = UdpSocket::bind(("127.0.0.1", 0)).expect("bind");
        let port = socket.local_addr().expect("addr").port();
        socket
            .set_read_timeout(Some(Duration::from_secs(20)))
            .expect("timeout");

        let requests = Arc::new(Mutex::new(Vec::new()));
        let count = Arc::new(AtomicUsize::new(0));
        let seen = Arc::clone(&requests);
        let counter = Arc::clone(&count);

        let handle = thread::spawn(move || {
            let mut buffer = [0u8; 2048];
            let mut step = 0usize;
            loop {
                let Ok((n, from)) = socket.recv_from(&mut buffer) else {
                    return;
                };
                let request = buffer[..n].to_vec();
                seen.lock().unwrap().push(request.clone());
                counter.fetch_add(1, Ordering::SeqCst);

                // script を使い切ったら以降は無応答
                let Some(behavior) = script.get(step).cloned() else {
                    step += 1;
                    continue;
                };
                step += 1;

                // seq と op はリクエストから取る (エコーが要る)
                let seq = u16::from_le_bytes([request[4], request[5]]);
                let op = request[6];

                match behavior {
                    Behavior::Drop => {}
                    Behavior::Ack(status) => {
                        let s = seq.to_le_bytes();
                        let ack = [b'N', b'S', 1, op, s[0], s[1], status, 0];
                        let _ = socket.send_to(&ack, from);
                    }
                    Behavior::ListParts { parts, total } => {
                        for (part, nparts, entries) in parts {
                            let datagram = build_list_part(seq, part, nparts, total, &entries);
                            let _ = socket.send_to(&datagram, from);
                        }
                    }
                    Behavior::GarbageThenList { parts, total } => {
                        let _ = socket.send_to(b"XXXX not ours", from);
                        let _ = socket.send_to(b"NS\x02 wrong version", from);
                        for (part, nparts, entries) in parts {
                            let datagram = build_list_part(seq, part, nparts, total, &entries);
                            let _ = socket.send_to(&datagram, from);
                        }
                    }
                }
            }
        });

        Self {
            port,
            requests,
            count,
            _handle: handle,
        }
    }

    fn device(&self) -> Device {
        Device::new("127.0.0.1", self.port, 0).expect("device")
    }

    fn request_count(&self) -> usize {
        self.count.load(Ordering::SeqCst)
    }

    /// 送られてきたリクエストの op 一覧
    fn ops(&self) -> Vec<u8> {
        self.requests.lock().unwrap().iter().map(|r| r[6]).collect()
    }
}

fn build_list_part(
    seq: u16,
    part: u8,
    nparts: u8,
    total: u16,
    entries: &[(String, u32)],
) -> Vec<u8> {
    let s = seq.to_le_bytes();
    let mut reply = vec![b'N', b'S', 1, SD_OP_LIST, s[0], s[1], 0, 0];
    reply.push(part);
    reply.push(nparts);
    reply.extend_from_slice(&total.to_le_bytes());
    reply.extend_from_slice(&(entries.len() as u16).to_le_bytes());
    reply.extend_from_slice(&8_000_000_000u64.to_le_bytes());
    reply.extend_from_slice(&1_200_000_000u64.to_le_bytes());
    for (name, size) in entries {
        reply.extend_from_slice(&size.to_le_bytes());
        reply.push(name.len() as u8);
        reply.extend_from_slice(name.as_bytes());
    }
    debug_assert!(reply.len() >= SD_LIST_HEADER);
    reply
}

fn entry(name: &str, size: u32) -> (String, u32) {
    (name.to_string(), size)
}

// ------------------------------------------------------------------ LIST

#[test]
fn a_single_part_listing_is_returned() {
    let device = MockDevice::new(vec![Behavior::ListParts {
        parts: vec![(0, 1, vec![entry("a.nes", 100), entry("b.nes", 200)])],
        total: 2,
    }]);

    let listing = sd_client::list(&mut device.device()).expect("listing");
    assert_eq!(listing.entries.len(), 2);
    assert_eq!(listing.total_bytes, 8_000_000_000);
    assert_eq!(device.request_count(), 1);
}

/// 空のカードは「マウント済みで空」として返る。応答なしと区別できなければ
/// ならない
#[test]
fn an_empty_card_is_not_an_error() {
    let device = MockDevice::new(vec![Behavior::ListParts {
        parts: vec![(0, 1, vec![])],
        total: 0,
    }]);

    let listing = sd_client::list(&mut device.device()).expect("an empty card is a valid answer");
    assert!(listing.entries.is_empty());
}

/// **部分的な結果を返さない。** 1 パート落ちたら全体を捨てて再送する。
/// 「ROM が少なく見える」形で静かに嘘をつくより、1 往復遅いほうがよい
#[test]
fn a_dropped_part_causes_a_retry_not_a_short_listing() {
    let device = MockDevice::new(vec![
        // 1 回目: part 1 が落ちる
        Behavior::ListParts {
            parts: vec![(0, 2, vec![entry("a.nes", 1)])],
            total: 2,
        },
        // 2 回目: 揃う
        Behavior::ListParts {
            parts: vec![
                (0, 2, vec![entry("a.nes", 1)]),
                (1, 2, vec![entry("b.nes", 2)]),
            ],
            total: 2,
        },
    ]);

    let listing = sd_client::list(&mut device.device()).expect("the retry completed the listing");
    assert_eq!(
        listing.entries.len(),
        2,
        "a short listing must not be returned"
    );
    assert_eq!(device.request_count(), 2, "it should have retried");
}

#[test]
fn a_listing_that_never_completes_times_out() {
    let device = MockDevice::new(vec![
        Behavior::ListParts {
            parts: vec![(0, 2, vec![entry("a.nes", 1)])],
            total: 2,
        };
        3
    ]);

    let err = sd_client::list(&mut device.device()).expect_err("should have timed out");
    assert_eq!(err.exit_code(), ExitCode::Timeout);
    assert!(matches!(err, SdError::Transport(TransportError::Timeout)));
}

/// **同じ試行に**無関係なデータグラムが混ざっても、答えは拾える。
/// 別々の試行に分けると「1 回目が捨てられて 2 回目で成功した」だけになり、
/// 混在への耐性を確かめたことにならない
#[test]
fn garbage_in_the_same_reply_does_not_break_a_listing() {
    let device = MockDevice::new(vec![Behavior::GarbageThenList {
        parts: vec![(0, 1, vec![entry("a.nes", 1)])],
        total: 1,
    }]);

    let listing = sd_client::list(&mut device.device()).expect("listing");
    assert_eq!(listing.entries.len(), 1);
    assert_eq!(
        device.request_count(),
        1,
        "no retry should have been needed"
    );
}

/// 同じパートが 2 度届いても件数が増えない (UDP の重複)
#[test]
fn a_duplicated_part_does_not_inflate_the_listing() {
    let device = MockDevice::new(vec![Behavior::ListParts {
        parts: vec![
            (0, 2, vec![entry("a.nes", 1)]),
            (0, 2, vec![entry("a.nes", 1)]), // 重複
            (1, 2, vec![entry("b.nes", 2)]),
        ],
        total: 2,
    }]);

    let listing = sd_client::list(&mut device.device()).expect("listing");
    assert_eq!(listing.entries.len(), 2, "a duplicate must not add entries");
}

/// カードが無ければ、その理由が返る (タイムアウトではない)
#[test]
fn a_refused_listing_reports_the_reason() {
    // 1 = NotMounted
    let device = MockDevice::new(vec![Behavior::Ack(1)]);

    let err = sd_client::list(&mut device.device()).expect_err("should have been refused");
    assert_eq!(err.exit_code(), ExitCode::Failure, "a refusal is a failure");
    assert!(
        err.to_string().contains("SD card"),
        "the reason should be explained: {err}"
    );
    assert_eq!(device.request_count(), 1, "a refusal must not be retried");
}

// ------------------------------------------------------------------ LOAD

#[test]
fn load_succeeds_on_an_ok_ack() {
    let device = MockDevice::new(vec![Behavior::Ack(0)]);
    sd_client::load(&mut device.device(), "game.nes").expect("load");
    assert_eq!(device.request_count(), 1);
}

#[test]
fn load_reports_a_missing_file() {
    // 2 = NotFound
    let device = MockDevice::new(vec![Behavior::Ack(2)]);
    let err = sd_client::load(&mut device.device(), "nope.nes").expect_err("should have failed");
    assert_eq!(err.exit_code(), ExitCode::Failure);
    assert!(err.to_string().contains("no such file"), "got: {err}");
}

/// LOAD は冪等なので再送してよい
#[test]
fn load_is_retried_when_the_reply_is_lost() {
    let device = MockDevice::new(vec![Behavior::Drop, Behavior::Drop, Behavior::Ack(0)]);
    sd_client::load(&mut device.device(), "game.nes").expect("the third attempt was answered");
    assert_eq!(device.request_count(), 3);
}

// -------------------------------------------------------- DELETE / RENAME

/// **削除は再送しない。** firmware は per-seq の結果キャッシュを持たないので、
/// 成功した DELETE をもう一度送ると NotFound が返り、成功が失敗に化ける
#[test]
fn delete_is_never_retransmitted() {
    let device = MockDevice::new(vec![Behavior::Drop, Behavior::Ack(0), Behavior::Ack(0)]);

    let err = sd_client::delete(&mut device.device(), "game.nes")
        .expect_err("a lost reply must not be retried");

    assert_eq!(
        device.request_count(),
        1,
        "the request must be sent exactly once"
    );
    // 「失敗」ではなく「判らない」。呼び出し側は sd ls で確かめる
    assert_eq!(err.exit_code(), ExitCode::Unknown);
    assert!(
        err.to_string().contains("may or may not"),
        "the message must not read as a failure: {err}"
    );
}

#[test]
fn rename_is_never_retransmitted() {
    let device = MockDevice::new(vec![Behavior::Drop, Behavior::Ack(0)]);

    let err = sd_client::rename(&mut device.device(), "a.nes", "b.nes")
        .expect_err("a lost reply must not be retried");

    assert_eq!(device.request_count(), 1);
    assert_eq!(err.exit_code(), ExitCode::Unknown);
}

#[test]
fn delete_succeeds_on_an_ok_ack() {
    let device = MockDevice::new(vec![Behavior::Ack(0)]);
    sd_client::delete(&mut device.device(), "game.nes").expect("delete");
    assert_eq!(device.request_count(), 1);
}

#[test]
fn rename_reports_an_existing_target() {
    // 9 = Exists
    let device = MockDevice::new(vec![Behavior::Ack(9)]);
    let err = sd_client::rename(&mut device.device(), "a.nes", "b.nes").expect_err("should fail");
    assert_eq!(err.exit_code(), ExitCode::Failure);
    assert!(err.to_string().contains("already exists"), "got: {err}");
}

/// 名前の検査は送る前に済ませる。通らないと判っている往復を省く
#[test]
fn an_unusable_name_never_reaches_the_device() {
    let device = MockDevice::new(vec![Behavior::Ack(0)]);
    let err = sd_client::delete(&mut device.device(), "game.nes\0other")
        .expect_err("an embedded NUL must be refused locally");
    assert!(matches!(err, SdError::Local(_)), "got {err:?}");
    assert_eq!(device.request_count(), 0, "nothing should have been sent");
}

// ------------------------------------------------------------------ BUSY

/// **BUSY は試行回数を消費しない。** 「今は無理」という答えであって、
/// 届かなかったわけではない。数えると、少し待てば通る操作が諦めてしまう
#[test]
fn busy_is_waited_out_without_spending_retries() {
    // 7 = Busy。再送上限 (3) を超える回数だけ返してから成功させる
    let mut script = vec![Behavior::Ack(7); 6];
    script.push(Behavior::Ack(0));
    let device = MockDevice::new(script);

    sd_client::load(&mut device.device(), "game.nes").expect("busy should have been waited out");
    assert_eq!(
        device.request_count(),
        7,
        "busy replies must not count against the retry budget"
    );
}

/// **冪等でない操作の BUSY は待ち直さない。**
///
/// 「BUSY が返った = まだ実行されていない」は、そのデータグラムについては
/// 正しくても、同じリクエストの別のコピーについては言えない。UDP でパケットが
/// 複製されると 1 通目が処理中に 2 通目が届き、firmware は処理中のリクエストの
/// 再送にも BUSY を返す。そこで送り直すと、既に成功した DELETE の 2 回目が
/// NotFound を返して成功が失敗に化ける
#[test]
fn a_busy_delete_is_not_sent_again() {
    let device = MockDevice::new(vec![Behavior::Ack(7), Behavior::Ack(0)]);

    let err = sd_client::delete(&mut device.device(), "game.nes")
        .expect_err("a busy delete must not be retried");

    assert_eq!(
        device.request_count(),
        1,
        "the request must be sent exactly once"
    );
    assert!(err.to_string().contains("busy"), "got: {err}");
}

#[test]
fn a_busy_rename_is_not_sent_again() {
    let device = MockDevice::new(vec![Behavior::Ack(7), Behavior::Ack(0)]);
    sd_client::rename(&mut device.device(), "a.nes", "b.nes")
        .expect_err("a busy rename must not be retried");
    assert_eq!(device.request_count(), 1);
}

/// BUSY が続いたら期限で諦める。無限には待たない
#[test]
fn an_endlessly_busy_operation_gives_up() {
    // 期限のあいだ BUSY を返し続け、それ以降は無応答になる。期限切れの後に
    // 1 つでも要求を出せば、無応答ぶん (3s x 3 回) 余計に待つことになるので、
    // 経過時間が「sleep の前だけで期限を見る」実装との差になる
    let busy_replies = 60; // 250ms x 60 = 15s 分。期限 (12s) より多い
    let device = MockDevice::new(vec![Behavior::Ack(7); busy_replies]);

    let started = std::time::Instant::now();
    let err =
        sd_client::load(&mut device.device(), "game.nes").expect_err("it must not wait forever");
    let waited = started.elapsed();

    assert_eq!(err.exit_code(), ExitCode::Failure);
    assert!(err.to_string().contains("busy"), "got: {err}");
    // 期限 (12s) + 待ち間隔 1 回ぶんに収まること
    assert!(
        waited < Duration::from_secs(14),
        "an attempt was started after the deadline: {waited:?}"
    );
}

// ----------------------------------------------------- バイナリ経由の検証

/// ここまでは `sd_client` を直接呼んでいるので、stdout/stderr の出し分けや
/// 実プロセスの終了コードは通っていない。呼び出し側が実際に見るのはそちら
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

    /// 一覧の JSON を 1 行そのまま固定する。
    ///
    /// 断片の検索や自前のパーサでは「JSON として正しい」ことを保証できない
    /// (引用符の閉じ忘れや末尾カンマを見逃す)。出力は決まった形なので、
    /// 期待する文字列そのものと突き合わせるほうが確実で、変化にも気づける。
    /// エスケープの正しさは `output::quote` の単体テストが受け持つ
    #[test]
    fn a_listing_prints_the_expected_json_line() {
        let device = MockDevice::new(vec![Behavior::ListParts {
            parts: vec![(0, 1, vec![entry("b.nes", 2), entry("a.nes", 1)])],
            total: 2,
        }]);

        let out = run(device.port, &["sd", "ls", "--json"]);
        assert_eq!(out.status.code(), Some(0));
        assert!(out.stderr.is_empty(), "nothing should go to stderr");

        let text = String::from_utf8(out.stdout).expect("utf-8");
        // 名前順に並び替えられていること込みで固定する
        assert_eq!(
            text.trim(),
            concat!(
                r#"{"ok":true,"files":["#,
                r#"{"name":"a.nes","size":1},"#,
                r#"{"name":"b.nes","size":2}"#,
                r#"],"total":2,"totalBytes":8000000000,"freeBytes":1200000000}"#,
            )
        );
    }

    #[test]
    fn a_refusal_exits_one_with_the_reason_on_stderr() {
        // 1 = NotMounted
        let device = MockDevice::new(vec![Behavior::Ack(1)]);
        let out = run(device.port, &["sd", "load", "game.nes"]);

        assert_eq!(out.status.code(), Some(1));
        assert!(out.stdout.is_empty(), "stdout must stay clean");
        let text = String::from_utf8(out.stderr).expect("utf-8");
        assert!(text.starts_with("stackchan: "), "got: {text}");
        assert!(
            text.contains("SD card"),
            "the reason should be shown: {text}"
        );
    }

    /// 冪等な操作が答えを得られなければ「届かなかった」= 3
    #[test]
    fn a_lost_reply_to_an_idempotent_op_exits_three() {
        let device = MockDevice::new(vec![Behavior::Drop; 5]);
        let out = run(device.port, &["sd", "load", "game.nes"]);
        assert_eq!(out.status.code(), Some(3));
    }

    /// 冪等でない操作が答えを得られなければ「判らない」= 4。
    /// これを 1 (失敗) にすると、呼び出し側は「消えていない」と誤解する
    #[test]
    fn a_lost_reply_to_a_delete_exits_four() {
        let device = MockDevice::new(vec![Behavior::Drop; 5]);
        let out = run(device.port, &["sd", "rm", "game.nes"]);

        assert_eq!(out.status.code(), Some(4));
        let text = String::from_utf8(out.stderr).expect("utf-8");
        assert!(
            text.contains("may or may not"),
            "it must not read as a failure: {text}"
        );
    }

    /// 引数が悪いのは使用法エラー (2)。デバイスには何も送らない
    #[test]
    fn an_unusable_name_exits_two_without_sending_anything() {
        let device = MockDevice::new(vec![Behavior::Ack(0)]);

        for name in ["", &"x".repeat(64)] {
            let out = run(device.port, &["sd", "rm", name]);
            assert_eq!(out.status.code(), Some(2), "name {name:?}");
        }
        assert_eq!(device.request_count(), 0, "nothing should have been sent");
    }

    /// `--json` を付けたら失敗も JSON。終了コードは変わらない
    #[test]
    fn json_mode_reports_a_refusal_as_json() {
        let device = MockDevice::new(vec![Behavior::Ack(2)]); // NotFound
        let out = run(device.port, &["sd", "rm", "nope.nes", "--json"]);

        assert_eq!(out.status.code(), Some(1));
        let text = String::from_utf8(out.stdout).expect("utf-8");
        assert!(text.contains("\"ok\":false"), "got: {text}");
        assert!(text.contains("\"exit\":1"), "got: {text}");
    }
}

/// 送ったリクエストの op が正しいこと。type を間違えると firmware が
/// パッド扱いにフォールスルーする
#[test]
fn each_operation_sends_its_own_op_code() {
    let device = MockDevice::new(vec![Behavior::Ack(0)]);
    sd_client::load(&mut device.device(), "game.nes").expect("load");
    assert_eq!(device.ops(), vec![1], "LOAD is op 1");

    let device = MockDevice::new(vec![Behavior::Ack(0)]);
    sd_client::delete(&mut device.device(), "game.nes").expect("delete");
    assert_eq!(device.ops(), vec![2], "DELETE is op 2");

    let device = MockDevice::new(vec![Behavior::Ack(0)]);
    sd_client::rename(&mut device.device(), "a.nes", "b.nes").expect("rename");
    assert_eq!(device.ops(), vec![3], "RENAME is op 3");
}
