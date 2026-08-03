//! 実機が返すステータスコード。
//!
//! `SdStatus` は `m5stack/src/sd_rom.h` の enum で、メニュー・type 4 の ACK・
//! type 5 の応答が同じ値を使う。`RomStatus` は `UDP_ROM_STATUS_*`。
//!
//! 表示文は `tools/serve_web.py` の `SD_STATUS_TEXT` / `ROM_STATUS_NAMES` と
//! 揃えてある。ユーザーが Web UI と CLI を行き来しても同じ言葉が出るように。

use std::fmt;

use crate::exit::ExitCode;

/// `SdStatus` (`m5stack/src/sd_rom.h`)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SdStatus {
    Ok,
    /// カードが無い、ファイルシステムが読めない、再マウント失敗
    NotMounted,
    NotFound,
    /// 空き容量が足りない
    NoSpace,
    /// `ROM_MAX_SIZE` 超過
    TooBig,
    /// iNES でない、または未対応マッパー
    BadRom,
    /// 空・長すぎ・サニタイズ後に別物になる名前
    BadName,
    /// ステージングバッファが他の転送に握られている
    Busy,
    /// マウント済みのカードで読み書きに失敗
    IoError,
    /// リネーム先が既にある
    Exists,
    /// firmware が知らない値を返した (プロトコルが食い違っている兆候)
    Unknown(u8),
}

impl SdStatus {
    pub fn from_wire(value: u8) -> Self {
        match value {
            0 => Self::Ok,
            1 => Self::NotMounted,
            2 => Self::NotFound,
            3 => Self::NoSpace,
            4 => Self::TooBig,
            5 => Self::BadRom,
            6 => Self::BadName,
            7 => Self::Busy,
            8 => Self::IoError,
            9 => Self::Exists,
            other => Self::Unknown(other),
        }
    }

    pub fn is_ok(self) -> bool {
        self == Self::Ok
    }

    /// 待てば解ける状態か。`Busy` だけは「答え」であって失敗ではないので、
    /// 再試行回数を消費せずに待ち直してよい (`serve_web.py` の
    /// `SD_BUSY_DEADLINE_S` と同じ扱い)
    pub fn is_busy(self) -> bool {
        self == Self::Busy
    }
}

impl fmt::Display for SdStatus {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let text = match self {
            Self::Ok => "ok",
            Self::NotMounted => "no SD card, or the card could not be mounted",
            Self::NotFound => "no such file on the card",
            Self::NoSpace => "not enough free space on the card",
            Self::TooBig => "the image is larger than the device accepts",
            Self::BadRom => "not an iNES image, or an unsupported mapper",
            Self::BadName => "unusable file name",
            Self::Busy => "the device is busy with another transfer",
            Self::IoError => "the card reported an I/O error",
            Self::Exists => "a file with that name already exists",
            Self::Unknown(code) => return write!(f, "unknown status {code} from the device"),
        };
        f.write_str(text)
    }
}

/// `UDP_ROM_STATUS_*` (`m5stack/src/config.h`)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RomStatus {
    Ok,
    /// ステージングバッファが他の転送に握られている
    Busy,
    /// `ROM_MAX_SIZE` 超過
    TooBig,
    /// ステージング用の確保に失敗
    Alloc,
    /// チャンク番号が期待とずれた。ACK の `expected` まで巻き戻して再送する
    Seq,
    /// 受け取った総量が BEGIN の宣言と合わない
    SizeMismatch,
    /// CRC 不一致
    Crc,
    /// iNES ヘッダとして読めない
    BadHeader,
    UnsupportedMapper,
    /// BEGIN を受けていないのに DATA / END が来た
    NoSession,
    Unknown(u8),
}

impl RomStatus {
    pub fn from_wire(value: u8) -> Self {
        match value {
            0 => Self::Ok,
            1 => Self::Busy,
            2 => Self::TooBig,
            3 => Self::Alloc,
            4 => Self::Seq,
            5 => Self::SizeMismatch,
            6 => Self::Crc,
            7 => Self::BadHeader,
            8 => Self::UnsupportedMapper,
            9 => Self::NoSession,
            other => Self::Unknown(other),
        }
    }

    pub fn is_ok(self) -> bool {
        self == Self::Ok
    }

    /// 巻き戻して続行できる状態か。これだけは失敗ではなく、ACK の
    /// `expected` から送り直せば転送は続く
    pub fn is_resumable(self) -> bool {
        self == Self::Seq
    }

    /// 待てば解ける状態か。`SdStatus::is_busy` と対になる。
    ///
    /// `Busy` は「同じ操作をやり直す」、`Seq` は「`expected` まで戻って続ける」で
    /// 対処が違うため、別々に判定する
    pub fn is_busy(self) -> bool {
        self == Self::Busy
    }
}

impl fmt::Display for RomStatus {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let text = match self {
            Self::Ok => "ok",
            Self::Busy => "the device is busy with another transfer",
            Self::TooBig => "the image is larger than the device accepts",
            Self::Alloc => "the device could not allocate staging memory",
            Self::Seq => "chunk out of sequence",
            Self::SizeMismatch => "the received size did not match the declared size",
            Self::Crc => "checksum mismatch",
            Self::BadHeader => "not an iNES image",
            Self::UnsupportedMapper => "unsupported mapper",
            Self::NoSession => "no transfer is open on the device",
            Self::Unknown(code) => return write!(f, "unknown status {code} from the device"),
        };
        f.write_str(text)
    }
}

/// 実機が明示的に拒否した場合の終了コード。
///
/// デバイスは答えている (無応答ではない) ので、`Timeout` でも `Unknown` でもなく
/// 「失敗」として 1 を返す
pub fn refusal_exit_code() -> ExitCode {
    ExitCode::Failure
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sd_status_maps_every_documented_code() {
        let expected = [
            (0, SdStatus::Ok),
            (1, SdStatus::NotMounted),
            (2, SdStatus::NotFound),
            (3, SdStatus::NoSpace),
            (4, SdStatus::TooBig),
            (5, SdStatus::BadRom),
            (6, SdStatus::BadName),
            (7, SdStatus::Busy),
            (8, SdStatus::IoError),
            (9, SdStatus::Exists),
        ];
        for (wire, status) in expected {
            assert_eq!(SdStatus::from_wire(wire), status, "code {wire}");
        }
    }

    // 知らない値を握り潰して Ok に倒すと、失敗した操作が成功に見える
    #[test]
    fn sd_status_keeps_unknown_codes_distinct_from_ok() {
        let unknown = SdStatus::from_wire(200);
        assert_eq!(unknown, SdStatus::Unknown(200));
        assert!(!unknown.is_ok());
        assert!(unknown.to_string().contains("200"));
    }

    #[test]
    fn rom_status_maps_every_documented_code() {
        let expected = [
            (0, RomStatus::Ok),
            (1, RomStatus::Busy),
            (2, RomStatus::TooBig),
            (3, RomStatus::Alloc),
            (4, RomStatus::Seq),
            (5, RomStatus::SizeMismatch),
            (6, RomStatus::Crc),
            (7, RomStatus::BadHeader),
            (8, RomStatus::UnsupportedMapper),
            (9, RomStatus::NoSession),
        ];
        for (wire, status) in expected {
            assert_eq!(RomStatus::from_wire(wire), status, "code {wire}");
        }
    }

    #[test]
    fn rom_status_keeps_unknown_codes_distinct_from_ok() {
        let unknown = RomStatus::from_wire(99);
        assert_eq!(unknown, RomStatus::Unknown(99));
        assert!(!unknown.is_ok());
    }

    // Busy と Seq は「失敗」ではないという扱いが再送ポリシーの根拠になる。
    // ただし対処が違うので、混同しないことも確かめる
    #[test]
    fn busy_and_seq_are_recognised_as_recoverable() {
        assert!(SdStatus::Busy.is_busy());
        assert!(!SdStatus::NotFound.is_busy());

        assert!(RomStatus::Seq.is_resumable());
        assert!(!RomStatus::Crc.is_resumable());

        // Busy は「やり直す」、Seq は「巻き戻して続ける」
        assert!(RomStatus::Busy.is_busy());
        assert!(!RomStatus::Busy.is_resumable());
        assert!(!RomStatus::Seq.is_busy());
    }

    // 表示文が空だとエラーメッセージが `stackchan: ` だけになる
    #[test]
    fn every_status_has_a_message() {
        for wire in 0..=10u8 {
            assert!(!SdStatus::from_wire(wire).to_string().is_empty());
            assert!(!RomStatus::from_wire(wire).to_string().is_empty());
        }
    }
}
