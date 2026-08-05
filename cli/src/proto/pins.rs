//! type 1: カセット端子の導通状態。
//!
//! ```text
//! [0..1]  'N','P'
//! [2]     version
//! [3]     1
//! [4..5]  seq u16 LE   — firmware は読まない
//! [6..13] pin mask u64 LE
//! ```
//!
//! mask の bit(n-1) が pin n の導通。60 本すべて 1 なら正しく挿さった状態。
//!
//! **パッドと違いタイムアウトしない。** 接触不良は物理的な状態で、送信側が
//! 止まっても勝手に直るものではないため。復帰は全ビット 1 を明示的に送る。

use super::constants::{PIN_MASK_ALL_OK, PIN_MASK_VALID, PIN_PACKET_SIZE};
use super::header::{PacketType, request};

/// ピンマスクを組み立てる。
///
/// 上位 4bit は firmware が落とすので、ここでも落としてから載せる。そうしないと
/// 「送った値」と「適用された値」が食い違い、`--verbose` の出力が嘘になる
pub fn mask(seq: u16, pins: u64) -> Vec<u8> {
    let mut packet = request(PacketType::Pins, seq);
    // マスクは [6] から始まる。共通ヘッダの [6]/[7] は type ごとの意味を持つ
    // 場所で、ここでは mask の下位 2 バイトがそこに載る — 後ろに継ぎ足すと
    // 16 バイトになり、firmware が読む位置とずれる
    packet.truncate(6);
    packet.extend_from_slice(&(pins & PIN_MASK_VALID).to_le_bytes());
    debug_assert_eq!(packet.len(), PIN_PACKET_SIZE);
    packet
}

/// 全ピン正常 (まっすぐ挿さった状態)
pub fn all_ok(seq: u16) -> Vec<u8> {
    mask(seq, PIN_MASK_ALL_OK)
}

/// 指定したピンだけを切ったマスクを作る。
///
/// デバイスはマスクの読み出し API を持たないので、CLI は現在値を知りえない。
/// よって「今の状態から N 番を切る」は表現できず、**常に全ピン正常からの差分**に
/// なる。累積したいなら `mask` に完全な値を渡すこと
pub fn break_pins(pins: &[u8]) -> Result<u64, String> {
    let mut value = PIN_MASK_ALL_OK;
    for &pin in pins {
        value &= !pin_bit(pin)?;
    }
    Ok(value)
}

/// 指定したピンだけを導通させたマスクを作る (それ以外はすべて切る)
pub fn only_pins(pins: &[u8]) -> Result<u64, String> {
    let mut value = 0u64;
    for &pin in pins {
        value |= pin_bit(pin)?;
    }
    Ok(value)
}

/// ピン番号 (1..=60) をビットに変換する
fn pin_bit(pin: u8) -> Result<u64, String> {
    let in_range = (1..=60).contains(&pin);
    if !in_range {
        return Err(format!("pin {pin} is out of range (1-60)"));
    }
    Ok(1u64 << (pin - 1))
}

/// マスクから切れているピン番号を並べる。表示用
pub fn broken_pins(value: u64) -> Vec<u8> {
    (1..=60u8)
        .filter(|&pin| value & (1u64 << (pin - 1)) == 0)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mask_matches_the_documented_layout() {
        let packet = mask(0x0102, 0x0F);
        assert_eq!(packet.len(), PIN_PACKET_SIZE);
        assert_eq!(&packet[0..6], &[b'N', b'P', 1, 1, 0x02, 0x01]);
        // u64 LE で 0x0F
        assert_eq!(&packet[6..14], &[0x0F, 0, 0, 0, 0, 0, 0, 0]);
    }

    // マスクは [6] から始まる。共通ヘッダの予約バイトを挟んで後ろに置くと
    // 16 バイトになり、firmware が読む位置 (packet[6 + i]) とずれる
    #[test]
    fn the_mask_starts_at_byte_six_with_no_padding() {
        // 全バイトが判別できる値を入れて、載る位置を 1 バイトずつ確かめる
        let packet = mask(0, 0x0807_0605_0403_0201);
        assert_eq!(packet.len(), 14, "a pin packet is 14 bytes, not 16");
        assert_eq!(&packet[6..14], &[1, 2, 3, 4, 5, 6, 7, 8]);
    }

    // 上位 4bit を落とさないと、`!0` で書いた「全部正常」と 60 pin から
    // 組み立てた値が別物になる
    #[test]
    fn mask_drops_the_reserved_top_bits() {
        let packet = mask(0, u64::MAX);
        let sent = u64::from_le_bytes(packet[6..14].try_into().unwrap());
        assert_eq!(sent, PIN_MASK_VALID);
        assert_eq!(sent, all_ok_value());
    }

    fn all_ok_value() -> u64 {
        let packet = all_ok(0);
        u64::from_le_bytes(packet[6..14].try_into().unwrap())
    }

    #[test]
    fn all_ok_lights_every_pin() {
        assert_eq!(all_ok_value().count_ones(), 60);
        assert!(broken_pins(all_ok_value()).is_empty());
    }

    #[test]
    fn breaking_pins_clears_exactly_those_bits() {
        let value = break_pins(&[25, 29]).unwrap();
        assert_eq!(broken_pins(value), vec![25, 29]);
        assert_eq!(value.count_ones(), 58);
    }

    #[test]
    fn only_pins_leaves_everything_else_broken() {
        let value = only_pins(&[1, 60]).unwrap();
        assert_eq!(value.count_ones(), 2);
        assert_eq!(broken_pins(value).len(), 58);
    }

    // 範囲外は静かに無視せずエラーにする。pin 0 や 61 を受け付けると
    // シフト量が壊れる
    #[test]
    fn pin_numbers_are_range_checked() {
        assert!(break_pins(&[0]).is_err());
        assert!(break_pins(&[61]).is_err());
        assert!(break_pins(&[1]).is_ok());
        assert!(break_pins(&[60]).is_ok());
    }

    #[test]
    fn the_first_and_last_pins_map_to_the_edge_bits() {
        assert_eq!(pin_bit(1).unwrap(), 1);
        assert_eq!(pin_bit(60).unwrap(), 1u64 << 59);
    }
}
