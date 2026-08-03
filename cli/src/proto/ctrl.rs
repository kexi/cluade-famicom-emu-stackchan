//! type 2: 本体制御 (リセット / 音量 / メニュー)。
//!
//! ```text
//! [0..1] 'N','P'
//! [2]    version
//! [3]    2
//! [4..5] seq u16 LE   — firmware は読まない
//! [6]    cmd ビットマスク
//! [7]    音量 0-255 (bit1 のときだけ意味を持つ)
//! ```
//!
//! cmd のビットは独立していて、同時に指定できる。firmware はどれもラッチする
//! だけで、実際の処理は core 1 のフレーム境界で走る。

use super::constants::{CTRL_MENU, CTRL_RESET, CTRL_VOLUME};
use super::header::{PacketType, request};

/// 音量の基準値 (`SPEAKER_VOLUME_BASE`)。Web UI のスライダー 1.0 がこれに当たり、
/// 0..1.5 が 0..192 に写る
pub const VOLUME_BASE: u8 = 128;

/// リセット。実機の RESET ボタンと同じ意味論で、ワーク RAM は保持したまま
/// リセットベクタから起動し直す
pub fn reset(seq: u16) -> Vec<u8> {
    command(seq, CTRL_RESET, 0)
}

/// 音量を設定する
pub fn volume(seq: u16, level: u8) -> Vec<u8> {
    command(seq, CTRL_VOLUME, level)
}

/// ROM 選択メニューを開く (ゲーム中のみ意味を持つ)
pub fn menu(seq: u16) -> Vec<u8> {
    command(seq, CTRL_MENU, 0)
}

/// ピンを戻すときのリセット付きマスク送信で使う、複数ビットの同時指定。
///
/// CPU バス系のピンを抜くとエミュレート中のプログラムが暴走することがあり、
/// **端子を戻すだけでは復帰しない** (リセットベクタを読み直す必要がある)
pub fn command(seq: u16, cmd: u8, level: u8) -> Vec<u8> {
    let mut packet = request(PacketType::Ctrl, seq);
    packet[6] = cmd;
    packet[7] = level;
    packet
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reset_sets_only_the_reset_bit() {
        let packet = reset(0x0102);
        assert_eq!(packet, vec![b'N', b'P', 1, 2, 0x02, 0x01, 0x01, 0x00]);
    }

    #[test]
    fn volume_carries_the_level_in_the_last_byte() {
        let packet = volume(0, 192);
        assert_eq!(packet[6], CTRL_VOLUME);
        assert_eq!(packet[7], 192);
    }

    #[test]
    fn menu_sets_only_the_menu_bit() {
        let packet = menu(0);
        assert_eq!(packet[6], CTRL_MENU);
    }

    // ビットが独立しているので、リセットと音量を 1 発で送れる
    #[test]
    fn commands_can_be_combined() {
        let packet = command(0, CTRL_RESET | CTRL_VOLUME, 64);
        assert_eq!(packet[6], 0x03);
        assert_eq!(packet[7], 64);
    }

    // type を間違えるとパッド扱いにフォールスルーする。volume 200 が
    // pad2 = 0xC8 (右 + 左 + B) になる事故が起きるので、type は必ず 2
    #[test]
    fn every_control_packet_uses_the_control_type() {
        for packet in [reset(0), volume(0, 200), menu(0)] {
            assert_eq!(packet[3], 2, "control packets must not be sent as type 0");
        }
    }

    #[test]
    fn packets_are_the_common_header_length() {
        assert_eq!(reset(0).len(), 8);
        assert_eq!(volume(0, 0).len(), 8);
        assert_eq!(menu(0).len(), 8);
    }
}
