//! type 4: ROM 転送 (BEGIN / DATA* / END / ABORT)。
//!
//! `.nes` イメージは MTU をはるかに超えるのでチャンクに割って送る。実機は
//! PSRAM にステージングし、全体を検証してから初めてコアに渡す。途中で切れた
//! 転送が動作中のゲームを落とすことはない。
//!
//! 共通ヘッダの seq 位置 (`[4..5]`) は **session 番号**として使う。

use super::constants::{
    ROM_ACK_MAGIC, ROM_ACK_SIZE, ROM_BEGIN_SIZE, ROM_CHUNK, ROM_DATA_HEADER, ROM_END_SIZE,
    ROM_FLAG_NO_LOAD, ROM_FLAG_SAVE_SD, ROM_FLAG_SWAP, ROM_MAX_SIZE, ROM_OP_ABORT, ROM_OP_BEGIN,
    ROM_OP_DATA, ROM_OP_END, ROM_SAVE_EVENT_SIZE, SD_MAGIC,
};
use super::header::{PacketType, check_reply, read_u16_le, request};
use super::name;
use super::status::{RomStatus, SdStatus};

/// 転送の指定
#[derive(Debug, Clone, Default)]
pub struct RomOptions {
    /// CPU をリセットせずに差し替える (Web 版の `nes_swap_rom` と同じ)
    pub swap: bool,
    /// SD にも保存する。名前が要る
    pub save_as: Option<String>,
    /// ロードせず保存だけ行う (実行中のゲームを止めない)。
    ///
    /// `swap` と併せて指定しても害はない。`swap` は「ロードするときの方法」の
    /// 指定で、ロード自体をしないならただ無視されるだけなので、正規化も拒否も
    /// せずそのまま送る
    pub no_load: bool,
}

impl RomOptions {
    fn flags(&self) -> u8 {
        let mut flags = 0;
        if self.swap {
            flags |= ROM_FLAG_SWAP;
        }
        if self.save_as.is_some() {
            flags |= ROM_FLAG_SAVE_SD;
        }
        if self.no_load {
            flags |= ROM_FLAG_NO_LOAD;
        }
        flags
    }
}

/// CRC-32 (zlib / IEEE 802.3)。firmware が同じ多項式で検証する
pub fn checksum(data: &[u8]) -> u32 {
    crc32fast::hash(data)
}

/// データを送るのに必要なチャンク数
pub fn chunk_count(len: usize) -> usize {
    len.div_ceil(ROM_CHUNK)
}

/// BEGIN。
///
/// 名前を付けると 17 バイト以上になり、firmware は**長さで**新形式と判別する。
/// 名前が無いときは 16 バイトぴったりにしなければならない — 空の名前フィールドを
/// 付けて 17 バイトにすると、名前のない保存要求として解釈されうる
pub fn begin(session: u16, data: &[u8], options: &RomOptions) -> Result<Vec<u8>, String> {
    let is_too_big = data.len() > ROM_MAX_SIZE;
    if is_too_big {
        return Err(format!(
            "the image is {} bytes, the device accepts at most {ROM_MAX_SIZE}",
            data.len()
        ));
    }
    let is_empty = data.is_empty();
    if is_empty {
        return Err("the image is empty".to_string());
    }

    // NO_LOAD は「保存だけする」の意味なので、保存先が無ければ何も起きない。
    // firmware は転送を受け取って捨て、成功の ACK を返す — 利用者から見ると
    // 「成功したのに何も変わらない」になるので、送る前に断る
    let is_a_no_op = options.no_load && options.save_as.is_none();
    if is_a_no_op {
        return Err(
            "--no-load without --save would neither install nor store the image".to_string(),
        );
    }

    let mut packet = request(PacketType::Rom, session);
    packet[6] = ROM_OP_BEGIN;
    packet[7] = options.flags();
    packet.extend_from_slice(&(data.len() as u32).to_le_bytes());
    packet.extend_from_slice(&checksum(data).to_le_bytes());
    debug_assert_eq!(packet.len(), ROM_BEGIN_SIZE);

    let Some(save_as) = &options.save_as else {
        return Ok(packet);
    };
    packet.extend_from_slice(&name::field(save_as, "save name")?);
    Ok(packet)
}

/// DATA。`index` は 0 始まりのチャンク番号。
///
/// 上限を `debug_assert` ではなく `Result` で守るのは、release ビルドで
/// 1400 バイトを超えると **firmware 側の受信バッファ (12 + 1400) で切り詰められ、
/// 宣言長と実長が食い違って無応答になる**ため。手元の入力ミスが transport から
/// はタイムアウトに見え、再送を繰り返す形で現れてしまう
pub fn data(session: u16, index: u16, payload: &[u8]) -> Result<Vec<u8>, String> {
    let is_too_long = payload.len() > ROM_CHUNK;
    if is_too_long {
        return Err(format!(
            "chunk is {} bytes, the device accepts at most {ROM_CHUNK}",
            payload.len()
        ));
    }

    let mut packet = request(PacketType::Rom, session);
    packet[6] = ROM_OP_DATA;
    packet[7] = 0;
    packet.extend_from_slice(&index.to_le_bytes());
    packet.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    debug_assert_eq!(packet.len(), ROM_DATA_HEADER);
    packet.extend_from_slice(payload);
    Ok(packet)
}

/// END。全チャンクを送り終えたことを伝え、検証させる
pub fn end(session: u16) -> Vec<u8> {
    mark(session, ROM_OP_END)
}

/// ABORT。転送を諦めてステージングを解放させる
pub fn abort(session: u16) -> Vec<u8> {
    mark(session, ROM_OP_ABORT)
}

fn mark(session: u16, op: u8) -> Vec<u8> {
    let mut packet = request(PacketType::Rom, session);
    packet[6] = op;
    packet[7] = 0;
    debug_assert_eq!(packet.len(), ROM_END_SIZE);
    packet
}

/// ROM の ACK
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RomAck {
    pub op: u8,
    pub session: u16,
    /// エコーされたチャンク番号
    pub index: u16,
    pub status: RomStatus,
    /// firmware が次に待っているチャンク番号。`status` が `Seq` のとき、
    /// ここまで巻き戻して送り直す
    pub expected: u16,
}

/// ACK を読む。session と op の両方でフィルタする — 同じソケットに別の
/// 転送の ACK が届いても取り違えないため
pub fn parse_ack(reply: &[u8], expect_session: u16, expect_op: u8) -> Option<RomAck> {
    check_reply(reply, ROM_ACK_MAGIC, ROM_ACK_SIZE)?;

    let op = reply[3];
    let op_matches = op == expect_op;
    if !op_matches {
        return None;
    }
    let session = read_u16_le(reply, 4);
    let session_matches = session == expect_session;
    if !session_matches {
        return None;
    }

    Some(RomAck {
        op,
        session,
        index: read_u16_le(reply, 6),
        status: RomStatus::from_wire(reply[8]),
        expected: read_u16_le(reply, 9),
    })
}

/// SD 保存の結果。END の ACK とは**別のデータグラム**で、同じソケットに届く
/// (firmware は BEGIN が来たポートに返す)
pub fn parse_save_event(reply: &[u8], expect_session: u16) -> Option<SdStatus> {
    check_reply(reply, SD_MAGIC, ROM_SAVE_EVENT_SIZE)?;

    // [3] は type 4 のエコー。SD の ACK (type 5) と magic が同じなので、
    // ここで区別しないと取り違える
    let is_rom_event = reply[3] == PacketType::Rom as u8;
    if !is_rom_event {
        return None;
    }
    let session_matches = read_u16_le(reply, 4) == expect_session;
    if !session_matches {
        return None;
    }

    Some(SdStatus::from_wire(reply[6]))
}

#[cfg(test)]
mod tests {
    use super::super::constants::SD_NAME_MAX;
    use super::*;

    // zlib と同じ値を出すことを既知のベクタで確かめる。ここがずれると
    // 転送が毎回 CRC エラーで落ちる
    #[test]
    fn checksum_matches_the_standard_vector() {
        assert_eq!(checksum(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn begin_without_a_name_is_exactly_sixteen_bytes() {
        let packet = begin(0x1234, b"rom", &RomOptions::default()).unwrap();
        assert_eq!(packet.len(), ROM_BEGIN_SIZE);
        assert_eq!(&packet[0..8], &[b'N', b'P', 1, 4, 0x34, 0x12, 0, 0]);
        assert_eq!(&packet[8..12], &3u32.to_le_bytes());
        assert_eq!(&packet[12..16], &checksum(b"rom").to_le_bytes());
    }

    // 名前付きは長さで判別される。空の名前フィールドを付けて 17 バイトに
    // してしまうと、名前のない保存要求として解釈されうる
    #[test]
    fn begin_with_a_name_is_longer_and_carries_it() {
        let options = RomOptions {
            save_as: Some("game.nes".to_string()),
            ..Default::default()
        };
        let packet = begin(1, b"rom", &options).unwrap();
        assert!(packet.len() > ROM_BEGIN_SIZE);
        assert_eq!(packet[16], 8);
        assert_eq!(&packet[17..], b"game.nes");
        assert_eq!(packet[7] & ROM_FLAG_SAVE_SD, ROM_FLAG_SAVE_SD);
    }

    #[test]
    fn flags_are_set_from_the_options() {
        let bare = begin(1, b"x", &RomOptions::default()).unwrap();
        assert_eq!(bare[7], 0);

        let swap = RomOptions {
            swap: true,
            ..Default::default()
        };
        assert_eq!(begin(1, b"x", &swap).unwrap()[7], ROM_FLAG_SWAP);

        let save_only = RomOptions {
            save_as: Some("a.nes".to_string()),
            no_load: true,
            ..Default::default()
        };
        assert_eq!(
            begin(1, b"x", &save_only).unwrap()[7],
            ROM_FLAG_SAVE_SD | ROM_FLAG_NO_LOAD
        );
    }

    #[test]
    fn begin_enforces_the_size_limit() {
        let just_fits = vec![0u8; ROM_MAX_SIZE];
        assert!(begin(1, &just_fits, &RomOptions::default()).is_ok());

        let one_too_many = vec![0u8; ROM_MAX_SIZE + 1];
        assert!(begin(1, &one_too_many, &RomOptions::default()).is_err());
    }

    #[test]
    fn begin_rejects_an_empty_image() {
        assert!(begin(1, b"", &RomOptions::default()).is_err());
    }

    #[test]
    fn begin_validates_the_save_name() {
        let empty = RomOptions {
            save_as: Some(String::new()),
            ..Default::default()
        };
        assert!(begin(1, b"x", &empty).is_err());

        let too_long = RomOptions {
            save_as: Some("x".repeat(SD_NAME_MAX + 1)),
            ..Default::default()
        };
        assert!(begin(1, b"x", &too_long).is_err());
    }

    #[test]
    fn data_matches_the_documented_layout() {
        let packet = data(0x1234, 5, b"payload").unwrap();
        assert_eq!(&packet[0..8], &[b'N', b'P', 1, 4, 0x34, 0x12, 1, 0]);
        assert_eq!(&packet[8..10], &5u16.to_le_bytes());
        assert_eq!(&packet[10..12], &7u16.to_le_bytes());
        assert_eq!(&packet[12..], b"payload");
    }

    // len が payload の実長と食い違うと firmware が拒む
    #[test]
    fn data_length_field_matches_the_payload() {
        for size in [1, 100, ROM_CHUNK] {
            let payload = vec![0xABu8; size];
            let packet = data(1, 0, &payload).unwrap();
            let declared = read_u16_le(&packet, 10) as usize;
            assert_eq!(declared, size);
            assert_eq!(packet.len(), ROM_DATA_HEADER + size);
        }
    }

    // 上限超過は release ビルドでも弾く。通してしまうと firmware の受信バッファで
    // 切り詰められ、宣言長と食い違って無応答になる = 手元のミスがタイムアウトに見える
    #[test]
    fn data_refuses_an_oversized_chunk() {
        let too_big = vec![0u8; ROM_CHUNK + 1];
        assert!(data(1, 0, &too_big).is_err());
        assert!(data(1, 0, &vec![0u8; ROM_CHUNK]).is_ok());
    }

    // 名前の途中の NUL は firmware から見える名前を切り、別のファイルに保存される
    #[test]
    fn begin_rejects_a_save_name_with_an_embedded_nul() {
        let sneaky = RomOptions {
            save_as: Some("game.nes\0other".to_string()),
            ..Default::default()
        };
        assert!(begin(1, b"x", &sneaky).is_err());
    }

    // 保存先が無い NO_LOAD は、firmware がイメージを捨てて成功を返す no-op
    #[test]
    fn begin_refuses_a_transfer_that_would_do_nothing() {
        let pointless = RomOptions {
            no_load: true,
            save_as: None,
            ..Default::default()
        };
        assert!(begin(1, b"x", &pointless).is_err());

        // 保存先があれば意味がある
        let save_only = RomOptions {
            no_load: true,
            save_as: Some("a.nes".to_string()),
            ..Default::default()
        };
        assert!(begin(1, b"x", &save_only).is_ok());
    }

    #[test]
    fn end_and_abort_are_the_bare_header() {
        assert_eq!(end(0x1234), vec![b'N', b'P', 1, 4, 0x34, 0x12, 2, 0]);
        assert_eq!(abort(0x1234), vec![b'N', b'P', 1, 4, 0x34, 0x12, 3, 0]);
    }

    #[test]
    fn chunk_count_covers_the_remainder() {
        assert_eq!(chunk_count(0), 0);
        assert_eq!(chunk_count(1), 1);
        assert_eq!(chunk_count(ROM_CHUNK), 1);
        assert_eq!(chunk_count(ROM_CHUNK + 1), 2);
        assert_eq!(chunk_count(ROM_CHUNK * 3), 3);
    }

    fn ack_bytes(op: u8, session: u16, index: u16, status: u8, expected: u16) -> Vec<u8> {
        let s = session.to_le_bytes();
        let i = index.to_le_bytes();
        let e = expected.to_le_bytes();
        vec![
            b'N', b'R', 1, op, s[0], s[1], i[0], i[1], status, e[0], e[1], 0,
        ]
    }

    #[test]
    fn parse_ack_reads_every_field() {
        let reply = ack_bytes(ROM_OP_DATA, 0x1234, 7, 4, 5);
        let ack = parse_ack(&reply, 0x1234, ROM_OP_DATA).unwrap();
        assert_eq!(ack.session, 0x1234);
        assert_eq!(ack.index, 7);
        assert_eq!(ack.status, RomStatus::Seq);
        assert_eq!(ack.expected, 5);
        assert!(ack.status.is_resumable());
    }

    // 別の転送の ACK を拾わない
    #[test]
    fn parse_ack_rejects_a_foreign_session_or_op() {
        let reply = ack_bytes(ROM_OP_DATA, 100, 0, 0, 1);
        assert!(parse_ack(&reply, 101, ROM_OP_DATA).is_none());
        assert!(parse_ack(&reply, 100, ROM_OP_END).is_none());
        assert!(parse_ack(&reply, 100, ROM_OP_DATA).is_some());
    }

    #[test]
    fn parse_ack_rejects_malformed_datagrams() {
        let good = ack_bytes(ROM_OP_END, 1, 0, 0, 0);
        assert!(parse_ack(&good[..11], 1, ROM_OP_END).is_none());

        let mut bad_magic = good.clone();
        bad_magic[1] = b'S';
        assert!(parse_ack(&bad_magic, 1, ROM_OP_END).is_none());
    }

    #[test]
    fn save_event_reports_the_card_status() {
        let s = 0x1234u16.to_le_bytes();
        let reply = vec![b'N', b'S', 1, 4, s[0], s[1], 3, 0];
        assert_eq!(parse_save_event(&reply, 0x1234), Some(SdStatus::NoSpace));
    }

    // SD の ACK (type 5) と magic が同じなので、type で区別できないと
    // 無関係な応答を保存結果として読んでしまう
    #[test]
    fn save_event_is_not_confused_with_an_sd_ack() {
        let s = 1u16.to_le_bytes();
        let sd_ack = vec![b'N', b'S', 1, 5, s[0], s[1], 0, 0];
        assert!(parse_save_event(&sd_ack, 1).is_none());

        let wrong_session = vec![b'N', b'S', 1, 4, 9, 0, 0, 0];
        assert!(parse_save_event(&wrong_session, 1).is_none());
    }
}
