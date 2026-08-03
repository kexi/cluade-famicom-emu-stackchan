//! 全 type に共通するリクエストヘッダ。
//!
//! `'N','P' | version | type | seq u16 LE | ...`
//!
//! **type を生の `u8` で受け取る API を置かないこと。** firmware の type
//! ディスパッチは `else` で PAD として処理するので、未知の type は黙って
//! コントローラ入力として解釈される (`m5stack/src/main.cpp:617-620`)。
//! たとえば `ctrl volume 200` を type 0 で送ってしまうと、`200 = 0xC8` が
//! pad2 に入って左右 + B が押されっぱなしになる。エラーにならないぶん質が悪い。
//!
//! そのため `PacketType` は enum で、ヘッダは必ずそれを経由して組み立てる。

use super::constants::{HEADER_SIZE, MAGIC, VERSION};

/// パケット種別 (`UDP_TYPE_*`)。
///
/// 生の数値からは作れない。firmware が未知の type をパッド扱いにする以上、
/// 「間違った type を組み立てられない」ことが型の役目になる
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PacketType {
    /// `UDP_TYPE_PAD`
    Pad = 0,
    /// `UDP_TYPE_PINS`
    Pins = 1,
    /// `UDP_TYPE_CTRL`
    Ctrl = 2,
    /// `UDP_TYPE_DEBUG`
    Debug = 3,
    /// `UDP_TYPE_ROM`
    Rom = 4,
    /// `UDP_TYPE_SD`
    Sd = 5,
}

impl PacketType {
    fn wire(self) -> u8 {
        self as u8
    }
}

/// 共通ヘッダを書いたバッファを返す。
///
/// `seq` は type によって扱いが違う: firmware が実際に読んで応答にエコーするのは
/// DEBUG と SD だけで、PAD / PINS / CTRL では読まれない。ROM は同じ位置を
/// session 番号に使う (`rom::begin` などを参照)
pub(crate) fn request(kind: PacketType, seq: u16) -> Vec<u8> {
    let mut packet = Vec::with_capacity(HEADER_SIZE);
    packet.extend_from_slice(&MAGIC);
    packet.push(VERSION);
    packet.push(kind.wire());
    packet.extend_from_slice(&seq.to_le_bytes());
    // [6] と [7] は type ごとの意味を持つ。呼び出し側が埋める
    packet.push(0);
    packet.push(0);
    debug_assert_eq!(packet.len(), HEADER_SIZE);
    packet
}

/// 応答の共通部分を検証する。
///
/// magic と version が合わなければ `None`。ソケットには無関係なデータグラムが
/// 届きうるので、ここを通らないものは黙って捨てる
pub(crate) fn check_reply(reply: &[u8], magic: [u8; 2], min_len: usize) -> Option<()> {
    let is_long_enough = reply.len() >= min_len;
    if !is_long_enough {
        return None;
    }
    let magic_matches = reply[0] == magic[0] && reply[1] == magic[1];
    if !magic_matches {
        return None;
    }
    let version_matches = reply[2] == VERSION;
    if !version_matches {
        return None;
    }
    Some(())
}

/// リトルエンディアンの u16 を読む。長さは呼び出し前に確認しておくこと
pub(crate) fn read_u16_le(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

/// リトルエンディアンの u32 を読む
pub(crate) fn read_u32_le(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

/// リトルエンディアンの u64 を読む
pub(crate) fn read_u64_le(bytes: &[u8], at: usize) -> u64 {
    let mut buf = [0u8; 8];
    buf.copy_from_slice(&bytes[at..at + 8]);
    u64::from_le_bytes(buf)
}

#[cfg(test)]
mod tests {
    use super::*;

    // type の数値は firmware と一致していなければならない。ずれると
    // 未知の type としてパッド扱いにフォールスルーする
    #[test]
    fn packet_types_match_the_firmware() {
        assert_eq!(PacketType::Pad.wire(), 0);
        assert_eq!(PacketType::Pins.wire(), 1);
        assert_eq!(PacketType::Ctrl.wire(), 2);
        assert_eq!(PacketType::Debug.wire(), 3);
        assert_eq!(PacketType::Rom.wire(), 4);
        assert_eq!(PacketType::Sd.wire(), 5);
    }

    #[test]
    fn request_lays_out_the_common_header() {
        let packet = request(PacketType::Ctrl, 0x1234);
        assert_eq!(packet, vec![b'N', b'P', 1, 2, 0x34, 0x12, 0, 0]);
    }

    #[test]
    fn request_is_always_the_documented_length() {
        for kind in [
            PacketType::Pad,
            PacketType::Pins,
            PacketType::Ctrl,
            PacketType::Debug,
            PacketType::Rom,
            PacketType::Sd,
        ] {
            assert_eq!(request(kind, 0).len(), HEADER_SIZE, "{kind:?}");
        }
    }

    #[test]
    fn check_reply_rejects_anything_unexpected() {
        let good = [b'N', b'S', 1, 0, 0, 0, 0, 0];
        assert!(check_reply(&good, *b"NS", 8).is_some());

        // 短すぎる
        assert!(check_reply(&good[..7], *b"NS", 8).is_none());
        // magic 違い (別の応答を取り違えない)
        assert!(check_reply(&good, *b"NR", 8).is_none());
        // version 違い
        let bad_version = [b'N', b'S', 2, 0, 0, 0, 0, 0];
        assert!(check_reply(&bad_version, *b"NS", 8).is_none());
        // 空
        assert!(check_reply(&[], *b"NS", 8).is_none());
    }

    #[test]
    fn little_endian_readers_match_the_wire_order() {
        // 最下位バイトが先頭に来る
        let bytes = [0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x89];
        assert_eq!(read_u16_le(&bytes, 0), 0x1234);
        assert_eq!(read_u32_le(&bytes, 0), 0x5678_1234);
        assert_eq!(read_u64_le(&bytes, 0), 0xCDEF_1234_5678_1234);
    }

    #[test]
    fn little_endian_readers_honour_the_offset() {
        let bytes = [0xFF, 0xFF, 0x34, 0x12];
        assert_eq!(read_u16_le(&bytes, 2), 0x1234);
    }
}
