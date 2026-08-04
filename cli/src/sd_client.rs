//! SD カード操作の実行 (type 5)。
//!
//! **再送方針は操作から導く。** 呼び出し側に選ばせると、DELETE に
//! 「タイムアウトしたら再送」を渡せてしまう。firmware は per-seq の結果
//! キャッシュを持たないので、成功した DELETE をもう一度送ると `NotFound` が
//! 返り、成功が失敗に化ける。
//!
//! **`Busy` は「答え」であって失敗ではない。** 別の操作がカードを使っている
//! 間はこれが返るので、試行回数を消費せずに待ち直す (`serve_web.py` の
//! `SD_BUSY_DEADLINE_S` と同じ扱い)。そうしないと、少し待てば通る操作が
//! 3 回の再送を使い切って諦めることになる。

use std::thread;
use std::time::{Duration, Instant};

use crate::proto::sd::{self, SdAck, SdEntry, SdListPart, SdListing, SdOp};
use crate::proto::status::SdStatus;
use crate::transport::{Device, PartOutcome, Retry, TransportError};

/// `SD_TIMEOUT_S`
const TIMEOUT: Duration = Duration::from_secs(3);

/// `SD_RETRIES`。冪等な操作だけがこの回数を使う
const RETRIES: u8 = 3;

/// `SD_BUSY_DEADLINE_S`。ここまでは `Busy` を待つ
const BUSY_DEADLINE: Duration = Duration::from_secs(12);

/// `SD_BUSY_RETRY_S`
const BUSY_RETRY: Duration = Duration::from_millis(250);

/// SD 操作の失敗
#[derive(Debug)]
pub enum SdError {
    /// デバイスが理由を添えて断った
    Refused(SdStatus),
    /// 引数が線に載せられない。送る前に判るので使用法エラー扱いにする
    /// (`ctrl volume 256` や不正なピンマスクと同じ)
    Local(String),
    /// 通信の問題 (タイムアウト / 結果不明 / I/O)
    Transport(TransportError),
}

impl SdError {
    pub fn exit_code(&self) -> crate::exit::ExitCode {
        match self {
            // デバイスが断ったのは失敗、こちらの引数が悪いのは使用法エラー
            Self::Refused(_) => crate::exit::ExitCode::Failure,
            Self::Local(_) => crate::exit::ExitCode::Usage,
            Self::Transport(e) => e.exit_code(),
        }
    }
}

impl std::fmt::Display for SdError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Refused(status) => write!(f, "{status}"),
            Self::Local(message) => f.write_str(message),
            Self::Transport(e) => write!(f, "{e}"),
        }
    }
}

impl From<TransportError> for SdError {
    fn from(e: TransportError) -> Self {
        Self::Transport(e)
    }
}

pub type Result<T> = std::result::Result<T, SdError>;

/// 操作に応じた再送方針。**ここが唯一の決定点**で、呼び出し側は選べない
fn retry_for(op: SdOp) -> Retry {
    if op.is_idempotent() {
        return Retry::Idempotent { attempts: RETRIES };
    }
    Retry::Once
}

/// カード上の ROM を一覧する
pub fn list(device: &mut Device) -> Result<SdListing> {
    with_busy_retry(|| {
        let seq = device.next_seq();
        let packet = sd::list(seq);
        let timeout = device.timeout_or(TIMEOUT);

        let listing: BusyOr<SdListing> =
            device.request_parts(&packet, timeout, retry_for(SdOp::List), || {
                // パートは試行ごとに集め直す。1 つ落ちたら全体を捨てて再送する
                // ほうが、少なく見える一覧を返すよりよい
                let mut parts: Vec<SdListPart> = Vec::new();
                move |reply: &[u8]| {
                    // Busy なら後続のパートは来ない
                    if let Some(ack) = sd::parse_ack(reply, seq, SdOp::List) {
                        if ack.status.is_busy() {
                            return PartOutcome::Done(BusyOr::Busy);
                        }
                        let is_a_refusal = !ack.status.is_ok();
                        if is_a_refusal {
                            return PartOutcome::Refused(ack.status.to_string());
                        }
                    }

                    let Some(part) = sd::parse_list_part(reply, seq) else {
                        return PartOutcome::Ignore;
                    };
                    // 同じパートが 2 度届いても増やさない
                    let is_new = !parts.iter().any(|p| p.part == part.part);
                    if is_new {
                        parts.push(part);
                    }

                    let Some(listing) = sd::assemble_listing(parts.clone()) else {
                        return PartOutcome::NeedMore;
                    };
                    PartOutcome::Done(BusyOr::Value(listing))
                }
            })?;
        Ok(listing)
    })
}

/// 名前を指定して起動する
pub fn load(device: &mut Device, name: &str) -> Result<()> {
    simple_op(device, SdOp::Load, |seq| sd::load(seq, name))
}

/// 名前を指定して削除する
pub fn delete(device: &mut Device, name: &str) -> Result<()> {
    simple_op(device, SdOp::Delete, |seq| sd::delete(seq, name))
}

/// リネームする
pub fn rename(device: &mut Device, from: &str, to: &str) -> Result<()> {
    simple_op(device, SdOp::Rename, |seq| sd::rename(seq, from, to))
}

/// ACK 1 発で終わる操作
fn simple_op(
    device: &mut Device,
    op: SdOp,
    build: impl Fn(u16) -> std::result::Result<Vec<u8>, String>,
) -> Result<()> {
    with_busy_retry(|| {
        let seq = device.next_seq();
        let packet = build(seq).map_err(SdError::Local)?;
        let timeout = device.timeout_or(TIMEOUT);

        let ack: BusyOr<SdAck> = device.request(&packet, timeout, retry_for(op), |reply| {
            let ack = sd::parse_ack(reply, seq, op)?;
            // BUSY を待ち直してよいのは冪等な操作だけ。
            //
            // 「BUSY が返った = まだ実行されていない」は**このデータグラムに
            // ついては**正しいが、同じリクエストの別のコピーについては言えない。
            // UDP でパケットが複製されると、1 通目が処理中に 2 通目が届き、
            // firmware は処理中のリクエストの再送にも BUSY を返す
            // (`serve_web.py:811`)。そこで送り直すと、既に成功した DELETE の
            // 2 回目が NotFound を返し、成功が失敗に化ける
            let may_wait_it_out = ack.status.is_busy() && op.is_idempotent();
            if may_wait_it_out {
                return Some(BusyOr::Busy);
            }
            Some(BusyOr::Value(ack))
        })?;

        let BusyOr::Value(ack) = ack else {
            return Ok(BusyOr::Busy);
        };
        let is_a_refusal = !ack.status.is_ok();
        if is_a_refusal {
            return Err(SdError::Refused(ack.status));
        }
        Ok(BusyOr::Value(()))
    })
}

/// `Busy` かそれ以外か
enum BusyOr<T> {
    Busy,
    Value(T),
}

/// `Busy` の間は待ち直す。
///
/// **試行回数を消費しない。** `Busy` は「今は無理」という答えであって、
/// 届かなかったわけではない。再送の回数に数えると、少し待てば通る操作が
/// 3 回で諦めることになる。
///
/// **ここに来るのは冪等な操作 (LIST / LOAD) だけ。** DELETE / RENAME の
/// `Busy` は待ち直さず、そのまま拒否として返す — 理由は `simple_op` を参照
fn with_busy_retry<T>(mut attempt: impl FnMut() -> Result<BusyOr<T>>) -> Result<T> {
    let give_up_at = Instant::now() + BUSY_DEADLINE;
    loop {
        match attempt()? {
            BusyOr::Value(value) => return Ok(value),
            BusyOr::Busy => {
                thread::sleep(BUSY_RETRY);
                // 眠った**後**に見る。前だけだと、期限の直前に BUSY を受けた
                // ときにもう 1 試行を始めてしまい、それが無応答なら期限を
                // 大きく越えて待つことになる
                let is_out_of_patience = Instant::now() >= give_up_at;
                if is_out_of_patience {
                    return Err(SdError::Refused(SdStatus::Busy));
                }
            }
        }
    }
}

/// 一覧を名前順に整える。firmware も名前順で返すが、パートを跨ぐと
/// 保証がないので表示前に揃える
pub fn sorted(mut entries: Vec<SdEntry>) -> Vec<SdEntry> {
    entries.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
    entries
}

#[cfg(test)]
mod tests {
    use super::*;

    // 再送方針は操作から一意に決まる。ここが崩れると、DELETE の再送で
    // 成功が失敗に化ける
    #[test]
    fn retry_policy_is_derived_from_the_operation() {
        assert_eq!(
            retry_for(SdOp::List),
            Retry::Idempotent { attempts: RETRIES }
        );
        assert_eq!(
            retry_for(SdOp::Load),
            Retry::Idempotent { attempts: RETRIES }
        );
        assert_eq!(retry_for(SdOp::Delete), Retry::Once);
        assert_eq!(retry_for(SdOp::Rename), Retry::Once);
    }

    #[test]
    fn sorting_is_case_insensitive() {
        let entries = vec![
            SdEntry {
                name: "Zelda.nes".to_string(),
                size: 1,
            },
            SdEntry {
                name: "asteroids.nes".to_string(),
                size: 2,
            },
        ];
        let names: Vec<String> = sorted(entries).into_iter().map(|e| e.name).collect();
        assert_eq!(names, vec!["asteroids.nes", "Zelda.nes"]);
    }

    #[test]
    fn refusals_are_failures_not_timeouts() {
        let err = SdError::Refused(SdStatus::NotFound);
        assert_eq!(err.exit_code(), crate::exit::ExitCode::Failure);
        assert_eq!(err.to_string(), SdStatus::NotFound.to_string());
    }

    // 「送ったが結果が判らない」は失敗ではない
    #[test]
    fn an_unknown_outcome_keeps_its_exit_code() {
        let err = SdError::Transport(TransportError::Unknown);
        assert_eq!(err.exit_code(), crate::exit::ExitCode::Unknown);
    }
}
