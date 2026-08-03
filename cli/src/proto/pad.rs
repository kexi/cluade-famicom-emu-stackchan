//! type 0: コントローラ入力。
//!
//! ```text
//! [0..1] 'N','P'
//! [2]    version
//! [3]    0
//! [4..5] seq u16 LE   — firmware は読まない
//! [6]    pad1 ボタンビット
//! [7]    pad2 ボタンビット
//! ```
//!
//! **`INPUT_TIMEOUT_MS` (500ms) 無音で両パッドが 0 に戻る。** 送信側が落ちても
//! ボタンが押しっぱなしにならないための仕組みで、裏を返すと「押している」状態は
//! 送り続けないと維持できない。
//!
//! pad1 は firmware 側で Grove とタッチの入力と OR される。pad2 は UDP のみ。

use super::constants::button;
use super::header::{PacketType, request};

/// パッドの状態を組み立てる。
///
/// `seq` は firmware が読まないが、既存の送信側 (`tools/procon_udp.py`) が
/// 16bit のカウンタを載せているので合わせておく
pub fn state(seq: u16, pad1: u8, pad2: u8) -> Vec<u8> {
    let mut packet = request(PacketType::Pad, seq);
    packet[6] = pad1;
    packet[7] = pad2;
    packet
}

/// 両パッドを離した状態。切断時に明示的に送って、押しっぱなしを残さない
pub fn release(seq: u16) -> Vec<u8> {
    state(seq, 0, 0)
}

/// ボタン名からビットを引く。`input send A B START` の解釈に使う。
///
/// 大文字小文字は問わない。十字キーは矢印の名前でも受ける
pub fn button_from_name(name: &str) -> Option<u8> {
    let bit = match name.to_ascii_uppercase().as_str() {
        "A" => button::A,
        "B" => button::B,
        "SELECT" => button::SELECT,
        "START" => button::START,
        "UP" => button::UP,
        "DOWN" => button::DOWN,
        "LEFT" => button::LEFT,
        "RIGHT" => button::RIGHT,
        _ => return None,
    };
    Some(bit)
}

/// `A+RIGHT` のような同時押しを 1 つのビットマスクにする
pub fn buttons_from_combo(combo: &str) -> Result<u8, String> {
    let mut bits = 0u8;
    for name in combo.split('+') {
        let trimmed = name.trim();
        let is_empty = trimmed.is_empty();
        if is_empty {
            return Err(format!("empty button name in '{combo}'"));
        }
        match button_from_name(trimmed) {
            Some(bit) => bits |= bit,
            None => return Err(format!("unknown button '{trimmed}'")),
        }
    }
    Ok(bits)
}

/// ビットマスクを人が読める名前に戻す。`--json` でない出力と、確認用
pub fn combo_to_names(bits: u8) -> String {
    let all = [
        (button::UP, "UP"),
        (button::DOWN, "DOWN"),
        (button::LEFT, "LEFT"),
        (button::RIGHT, "RIGHT"),
        (button::SELECT, "SELECT"),
        (button::START, "START"),
        (button::B, "B"),
        (button::A, "A"),
    ];
    let names: Vec<&str> = all
        .iter()
        .filter(|(bit, _)| bits & bit != 0)
        .map(|(_, name)| *name)
        .collect();
    if names.is_empty() {
        return "(none)".to_string();
    }
    names.join("+")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn state_matches_the_documented_layout() {
        let packet = state(0x0102, button::A, button::START);
        assert_eq!(packet, vec![b'N', b'P', 1, 0, 0x02, 0x01, 0x01, 0x08]);
        assert_eq!(packet.len(), 8);
    }

    #[test]
    fn release_clears_both_pads() {
        let packet = release(7);
        assert_eq!(packet[6], 0);
        assert_eq!(packet[7], 0);
    }

    #[test]
    fn button_names_are_case_insensitive() {
        assert_eq!(button_from_name("a"), Some(button::A));
        assert_eq!(button_from_name("A"), Some(button::A));
        assert_eq!(button_from_name("start"), Some(button::START));
        assert_eq!(button_from_name("nope"), None);
    }

    #[test]
    fn combos_or_their_bits_together() {
        assert_eq!(buttons_from_combo("A+RIGHT"), Ok(button::A | button::RIGHT));
        assert_eq!(
            buttons_from_combo(" a + right "),
            Ok(button::A | button::RIGHT)
        );
    }

    #[test]
    fn combos_reject_nonsense() {
        assert!(buttons_from_combo("A+").is_err());
        assert!(buttons_from_combo("+A").is_err());
        assert!(buttons_from_combo("A+NOPE").is_err());
        assert!(buttons_from_combo("").is_err());
    }

    #[test]
    fn combo_names_round_trip() {
        for combo in ["A", "A+RIGHT", "UP+DOWN+LEFT+RIGHT", "START+SELECT"] {
            let bits = buttons_from_combo(combo).unwrap();
            let back = buttons_from_combo(&combo_to_names(bits)).unwrap();
            assert_eq!(bits, back, "combo {combo}");
        }
    }

    #[test]
    fn no_buttons_reads_as_none() {
        assert_eq!(combo_to_names(0), "(none)");
    }
}
