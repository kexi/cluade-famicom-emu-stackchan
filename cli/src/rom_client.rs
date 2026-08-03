//! ROM 転送の実行 (type 4)。
//!
//! **ストップ&ウェイト。** 1 チャンクごとに ACK を待ってから次を送る。LAN の
//! 往復は 2-5ms なので 768KB のカートでも数秒で終わり、ウィンドウを持たせても
//! 複雑さが増えるだけ。
//!
//! **転送は 1 つのソケットで最後までやり切る。** SD 保存の結果は END の ACK とは
//! 別のデータグラムで返るが、firmware は BEGIN が来たポートに返すので、
//! ここで別のソケットを開くと違う場所で待つことになる。

use std::time::{Duration, Instant};

use crate::exit::ExitCode;
use crate::proto::constants::{ROM_CHUNK, ROM_OP_BEGIN, ROM_OP_DATA, ROM_OP_END};
use crate::proto::rom::{self, RomOptions};
use crate::proto::status::{RomStatus, SdStatus};
use crate::transport::{Device, TransportError};

/// `ROM_TIMEOUT_S`
const TIMEOUT: Duration = Duration::from_millis(300);

/// `ROM_RETRIES`。1 チャンクあたりの再送回数
const RETRIES: u8 = 8;

/// `ROM_MAX_TOTAL_RETRIES`。転送全体の再送上限。
///
/// チャンクごとの上限だけだと、毎回 1 回ずつ再送しながら進む転送が
/// いつまでも終わらない
const MAX_TOTAL_RETRIES: u32 = 64;

/// `SD_SAVE_TIMEOUT_S`。カードへの書き込みは core 1 のフレーム境界で走るので、
/// ACK より遅れて返る
const SAVE_TIMEOUT: Duration = Duration::from_secs(6);

/// 転送の失敗
#[derive(Debug)]
pub enum RomError {
    /// デバイスが理由を添えて断った
    Refused(RomStatus),
    /// 送る前に判ったもの (大きすぎる、名前が不正など)
    Local(String),
    /// 通信の問題
    Transport(TransportError),
    /// 再送が上限に達した。デバイスは答えていないので「届かない」側
    TooManyRetries,
    /// 順序ずれが収束しない。デバイスは答えているので「届かない」ではない
    NotConverging,
}

impl RomError {
    pub fn exit_code(&self) -> ExitCode {
        match self {
            Self::Refused(_) => ExitCode::Failure,
            Self::Local(_) => ExitCode::Usage,
            Self::Transport(e) => e.exit_code(),
            // 届いていないので「答えが来ない」に寄せる
            Self::TooManyRetries => ExitCode::Timeout,
            // 答えは来ているので失敗として扱う
            Self::NotConverging => ExitCode::Failure,
        }
    }
}

impl std::fmt::Display for RomError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Refused(status) => write!(f, "{status}"),
            Self::Local(message) => f.write_str(message),
            Self::Transport(e) => write!(f, "{e}"),
            Self::TooManyRetries => f.write_str("too many retries; the link is losing datagrams"),
            Self::NotConverging => {
                f.write_str("the device kept asking to rewind; the transfer made no progress")
            }
        }
    }
}

impl From<TransportError> for RomError {
    fn from(e: TransportError) -> Self {
        Self::Transport(e)
    }
}

pub type Result<T> = std::result::Result<T, RomError>;

/// 転送の結果
#[derive(Debug)]
pub struct Transfer {
    pub bytes: usize,
    pub chunks: usize,
    pub retries: u32,
    pub elapsed: Duration,
    /// SD 保存を頼んだ場合の結果。`None` は「返事が来なかった」で、
    /// 保存されたかどうかは判らない
    pub saved: Option<Option<SdStatus>>,
}

impl Transfer {
    /// 転送そのものは成功していても、保存を頼んで失敗していれば全体は失敗。
    /// イメージは届いているので、判定は保存の結果に従う
    pub fn is_ok(&self) -> bool {
        match &self.saved {
            None => true,
            Some(Some(status)) => status.is_ok(),
            Some(None) => false,
        }
    }
}

/// 進捗の通知。`(送信済みチャンク, 総チャンク)`
pub type Progress<'a> = &'a mut dyn FnMut(usize, usize);

/// ROM を送る
pub fn send(
    device: &mut Device,
    data: &[u8],
    options: &RomOptions,
    mut progress: Option<Progress<'_>>,
) -> Result<Transfer> {
    let started = Instant::now();

    let session =
        new_session().map_err(|e| RomError::Local(format!("cannot pick a transfer id: {e}")))?;
    let begin = rom::begin(session, data, options).map_err(RomError::Local)?;

    let chunks: Vec<&[u8]> = data.chunks(ROM_CHUNK).collect();
    // 再送の数え上げは 1 か所に閉じる。BEGIN / DATA / END のどこで嵩んでも
    // 「転送全体の上限」として同じように効かせるため
    let mut budget = RetryBudget::default();

    // 転送はここから最後まで同じソケットを使う。SD の保存イベントは
    // BEGIN が来たポートに返るので、途中で開き直すと受け取れない
    let mut session_socket = device.open_session()?;

    exchange(
        &mut session_socket,
        &begin,
        session,
        ROM_OP_BEGIN,
        &mut budget,
    )?;
    if let Some(report) = progress.as_deref_mut() {
        report(0, chunks.len());
    }

    let mut index: usize = 0;
    while index < chunks.len() {
        let packet = rom::data(session, index as u16, chunks[index]).map_err(RomError::Local)?;
        let ack = send_until_ack(
            &mut session_socket,
            &packet,
            session,
            ROM_OP_DATA,
            index as u16,
            &budget,
        )?;
        budget.add(ack.retries);

        // 順序がずれたら、デバイスが待っている位置まで巻き戻す。中断より
        // ましなのは、原因の多くが「ACK を落として 1 つ先に進んでしまった」
        // だから
        let is_out_of_order = ack.status.is_resumable();
        if is_out_of_order {
            let expected = ack.expected as usize;
            let is_past_the_end = expected > chunks.len();
            if is_past_the_end {
                return Err(RomError::Refused(RomStatus::Seq));
            }
            index = expected;
            // 巻き戻しは「デバイスが答えている」ので、収束しないことと
            // 「届かない」ことは別に数える
            budget.add_rewind()?;
            continue;
        }

        let is_a_refusal = !ack.status.is_ok();
        if is_a_refusal {
            return Err(RomError::Refused(ack.status));
        }

        index += 1;
        if let Some(report) = progress.as_deref_mut() {
            report(index, chunks.len());
        }
    }

    exchange(
        &mut session_socket,
        &rom::end(session),
        session,
        ROM_OP_END,
        &mut budget,
    )?;
    let retries = budget.total;

    // 保存を頼んだときだけ、別のデータグラムで結果が返る
    let saved = options
        .save_as
        .as_ref()
        .map(|_| await_save_event(&mut session_socket, session));

    Ok(Transfer {
        bytes: data.len(),
        chunks: chunks.len(),
        retries,
        elapsed: started.elapsed(),
        saved,
    })
}

/// ACK 待ちの結果
struct Ack {
    status: RomStatus,
    expected: u16,
    retries: u32,
}

/// 転送全体の再送を数える。
///
/// チャンクごとの上限だけだと、毎回 1 回ずつ再送しながら進む転送が
/// いつまでも終わらない。BEGIN / DATA / END のどこで嵩んでも同じ上限で
/// 効かせるため、加算を 1 か所に閉じる
#[derive(Default)]
struct RetryBudget {
    total: u32,
    rewinds: u32,
}

impl RetryBudget {
    /// あと何回再送してよいか。
    ///
    /// 上限は**次の再送を始める前**に効かせる。ACK を受け取った後に
    /// 「予算超過だから失敗」とすると、firmware 側では CRC も通って適用も
    /// 予約済みの転送を、こちらだけ失敗として報告することになる
    fn remaining(&self) -> u32 {
        MAX_TOTAL_RETRIES.saturating_sub(self.total)
    }

    /// 無応答による再送を数える。上限は `remaining` で送る前に効かせるので、
    /// ここは記録するだけ — 受け取った ACK を後から失敗に覆さない
    fn add(&mut self, retries: u32) {
        self.total += retries;
    }

    /// 巻き戻しを数える。デバイスは答えているので、無応答とは別の失敗にする
    fn add_rewind(&mut self) -> Result<()> {
        self.total += 1;
        self.rewinds += 1;
        let is_not_converging = self.rewinds > MAX_TOTAL_RETRIES;
        if is_not_converging {
            return Err(RomError::NotConverging);
        }
        let is_hopeless = self.total > MAX_TOTAL_RETRIES;
        if is_hopeless {
            return Err(RomError::TooManyRetries);
        }
        Ok(())
    }
}

/// 1 往復。非 OK ならそこで止める
fn exchange(
    socket: &mut crate::transport::Session,
    packet: &[u8],
    session: u16,
    op: u8,
    budget: &mut RetryBudget,
) -> Result<()> {
    // BEGIN と END はチャンク番号を持たないので 0 がエコーされる
    let ack = send_until_ack(socket, packet, session, op, 0, budget)?;
    budget.add(ack.retries);
    let is_a_refusal = !ack.status.is_ok();
    if is_a_refusal {
        return Err(RomError::Refused(ack.status));
    }
    Ok(())
}

/// ACK が来るまで送り直す。
///
/// **無関係なデータグラムで締切を消費しない。** 同じソケットに他の応答が
/// 混ざっても、締切の内側で読み続ける。
///
/// `chunk` はエコーされるチャンク番号。**これを照合しないと、遅れて届いた
/// 前のチャンクの ACK を今のチャンクの答えとして受け取ってしまう** —
/// ストップ&ウェイトのつもりが 1 つずれたまま進み、最後に SizeMismatch で
/// 落ちる (`serve_web.py` の `parse_rom_ack` は index を返すだけで照合して
/// いないので、そこは踏襲しない)
fn send_until_ack(
    socket: &mut crate::transport::Session,
    packet: &[u8],
    session: u16,
    op: u8,
    chunk: u16,
    budget: &RetryBudget,
) -> Result<Ack> {
    // 予算は**次の再送を始める前**に見る。最初の送信は必ず行うので、
    // 予算を使い切っていても成功しうる往復は封じない。
    //
    // 使い切った状態で無応答なら、この関数は再送せず Timeout を返し、
    // 呼び出し側がそれを転送の失敗として扱う。ACK を受け取った後に
    // 「予算超過だから失敗」と覆すことはしない — firmware 側では既に
    // 適用が予約されているかもしれないため
    let allowed = (budget.remaining() + 1).min(RETRIES as u32) as u8;
    for attempt in 0..allowed.max(1) {
        let found = socket.exchange(packet, TIMEOUT, |reply| {
            let ack = rom::parse_ack(reply, session, op)?;
            // ステータスによらず番号で照合する。firmware は順序ずれの通知でも
            // **受け取ったチャンク番号をそのままエコーする** (`main.cpp:395`)
            // ので、番号が違う SEQ は「今の要求への特別な答え」ではなく、
            // 前の DATA への遅れた ACK。受けると古い `expected` へ飛ぶ
            let is_this_chunk = ack.index == chunk;
            if !is_this_chunk {
                return None;
            }
            Some(Ack {
                status: ack.status,
                expected: ack.expected,
                retries: attempt as u32,
            })
        })?;
        if let Some(ack) = found {
            return Ok(ack);
        }
    }
    Err(RomError::Transport(TransportError::Timeout))
}

/// SD 保存の結果を待つ。
///
/// 無音は例外にせず `None` で返す。「送れたが保存結果が判らない」と
/// 「転送に失敗した」は別のことで、前者を失敗として報告すると、実際には
/// カードに載っているのに載っていないと誤解させる
fn await_save_event(socket: &mut crate::transport::Session, session: u16) -> Option<SdStatus> {
    socket
        .exchange(&[], SAVE_TIMEOUT, |reply| {
            rom::parse_save_event(reply, session)
        })
        .ok()
        .flatten()
}

/// 転送の session 番号を引く。
///
/// **時計から作ってはいけない。** firmware は同じ session の BEGIN を
/// 「ACK を落とした再送」とみなし、新しいサイズと CRC を採らずに OK を返す
/// (`m5stack/src/main.cpp:295`)。送信元アドレスは同一性に含まれないので、
/// 別のプロセスが同じ番号を引くと、後から来た DATA が前の転送に混ざる。
/// 時計の小数部は分解能次第で近接起動が同じ値になりうる。
///
/// 予測不能性は要らないが、一様に散る必要はある。OS の乱数を使う
/// 転送の session 番号を引く。
///
/// **時計から作ってはいけない。** firmware は同じ session の BEGIN を
/// 「ACK を落とした再送」とみなし、新しいサイズと CRC を採らずに OK を返す
/// (`m5stack/src/main.cpp:295`)。送信元アドレスは同一性に含まれないので、
/// 別のプロセスが同じ番号を引くと、後から来た DATA が前の転送に混ざる。
/// 時計の小数部は分解能次第で近接起動が同じ値になりうる。
///
/// **乱数が引けなければ転送しない。** 衝突は別の転送を壊すので、怪しい番号で
/// 始めるより、始めないほうがよい
pub(crate) fn new_session() -> std::io::Result<u16> {
    use std::io::Read;

    let mut file = std::fs::File::open("/dev/urandom")?;
    loop {
        let mut bytes = [0u8; 2];
        file.read_exact(&mut bytes)?;
        let value = u16::from_le_bytes(bytes);
        // 0 は firmware が「session なし」に使う。1 に寄せると 1 だけ確率が
        // 倍になるので引き直す
        let is_usable = value != 0;
        if is_usable {
            return Ok(value);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sessions_are_never_zero() {
        for _ in 0..100 {
            assert_ne!(new_session().expect("urandom"), 0);
        }
    }

    // session が衝突すると、firmware は新しい BEGIN を「再送」とみなして
    // 前の転送を続けてしまう (main.cpp:295)。時計から作ると近接起動で
    // 同じ値になりうるので、散っていることを確かめる
    #[test]
    fn sessions_are_spread_out() {
        let seen: std::collections::HashSet<u16> =
            (0..200).map(|_| new_session().expect("urandom")).collect();
        // 200 回引いて 16bit に散っていれば、重複はごくわずかのはず
        assert!(
            seen.len() > 190,
            "sessions are not spread out: {} distinct out of 200",
            seen.len()
        );
    }

    #[test]
    fn a_transfer_without_a_save_is_judged_by_the_transfer() {
        let transfer = Transfer {
            bytes: 1,
            chunks: 1,
            retries: 0,
            elapsed: Duration::ZERO,
            saved: None,
        };
        assert!(transfer.is_ok());
    }

    // 保存を頼んだなら、判定は保存の結果に従う。イメージは届いていても、
    // 求められたのは「カードに置くこと」
    #[test]
    fn a_transfer_with_a_save_is_judged_by_the_save() {
        let ok = Transfer {
            bytes: 1,
            chunks: 1,
            retries: 0,
            elapsed: Duration::ZERO,
            saved: Some(Some(SdStatus::Ok)),
        };
        assert!(ok.is_ok());

        let failed = Transfer {
            saved: Some(Some(SdStatus::NoSpace)),
            ..ok_transfer()
        };
        assert!(!failed.is_ok());

        // 返事が来なかった場合も成功とは言えない
        let unknown = Transfer {
            saved: Some(None),
            ..ok_transfer()
        };
        assert!(!unknown.is_ok());
    }

    fn ok_transfer() -> Transfer {
        Transfer {
            bytes: 1,
            chunks: 1,
            retries: 0,
            elapsed: Duration::ZERO,
            saved: Some(Some(SdStatus::Ok)),
        }
    }

    // 予算は「次の再送を始める前」に効かせる。使い切っていても最初の送信は
    // 必ず許す — ACK を受け取れる往復を封じないため
    #[test]
    fn an_exhausted_budget_still_allows_one_send() {
        let mut budget = RetryBudget::default();
        assert_eq!(budget.remaining(), MAX_TOTAL_RETRIES);

        budget.add(MAX_TOTAL_RETRIES);
        assert_eq!(budget.remaining(), 0, "the budget should be spent");

        // 使い切っても超過しても、記録するだけで失敗にはしない。
        // 受け取った ACK を後から覆さないため
        budget.add(10);
        assert_eq!(budget.remaining(), 0);
    }

    // 巻き戻しの発散だけは、答えが返っているので別の失敗として扱う
    #[test]
    fn endless_rewinding_is_a_failure_not_a_timeout() {
        let mut budget = RetryBudget::default();
        let mut last = Ok(());
        for _ in 0..(MAX_TOTAL_RETRIES + 2) {
            last = budget.add_rewind();
        }
        let err = last.expect_err("it must give up eventually");
        assert!(matches!(err, RomError::NotConverging), "got {err:?}");
        assert_eq!(err.exit_code(), ExitCode::Failure);
    }

    #[test]
    fn errors_map_to_the_documented_exit_codes() {
        assert_eq!(
            RomError::Refused(RomStatus::Crc).exit_code(),
            ExitCode::Failure
        );
        assert_eq!(
            RomError::Local("too big".into()).exit_code(),
            ExitCode::Usage
        );
        assert_eq!(RomError::TooManyRetries.exit_code(), ExitCode::Timeout);
        assert_eq!(
            RomError::Transport(TransportError::Unknown).exit_code(),
            ExitCode::Unknown
        );
    }
}
