//! Switch Pro コントローラの読み取り。
//!
//! `tools/procon_udp.py` の hidapi バックエンドを移したもの。SDL 側は移さない:
//! macOS で壊れた状態を返すことがあり、そのために「10ms x 20 サンプルで検査して
//! フォールバック」というヒューリスティクスを抱えている
//! (`procon_udp.py:635`)。実際に使われているのは hid だけで、`justfile:53` も
//! `--backend hid` を明示している。
//!
//! **レポートの分解はここに閉じ込めて、HID なしでも試せるようにする。**
//! 実機を繋がないと確かめられないのは接続とハンドシェイクだけにしたい。

use std::time::Duration;

use crate::proto::constants::button;

/// Pro コントローラの USB ID
pub const VENDOR_ID: u16 = 0x057E;
pub const PRODUCT_ID: u16 = 0x2009;

/// 標準入力レポートの ID
pub const REPORT_ID_STANDARD: u8 = 0x30;

/// ハンドシェイクのコマンド
pub const USB_CMD_HANDSHAKE: u8 = 0x02;
pub const USB_CMD_FORCE_FULL_MODE: u8 = 0x04;
pub const SUBCMD_SET_INPUT_REPORT_MODE: u8 = 0x03;

/// byte[4] の bit7 は充電グリップのフラグでボタンではない。USB 接続では
/// ずっと 1 のままなので、落とさないと押しっぱなしに見える
const SHARED_BYTE_BUTTON_MASK: u8 = 0x3F;

/// HOME は NES のパッドビットに居場所がないので、メニューを開く制御として
/// 別に送る
const SHARED_HOME: u8 = 0x10;

/// 左スティックは 12bit パック。中心は 2048 付近で、振り切りが約 ±1400 なので
/// 500 は軸のしきい値 0.5 に相当する
const STICK_CENTER: i32 = 2048;
const STICK_THRESHOLD: i32 = 500;

/// ハンドシェイクの各段で待つ時間
pub const HANDSHAKE_STEP: Duration = Duration::from_millis(150);
pub const HANDSHAKE_SETTLE: Duration = Duration::from_millis(300);
/// レポートが流れ始めるまで待つ上限
pub const HANDSHAKE_DEADLINE: Duration = Duration::from_secs(2);

/// 1 レポートから読み取った状態
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PadState {
    /// NES のボタンビット
    pub buttons: u8,
    /// HOME が押されているか。押した瞬間にメニューを開く
    pub home: bool,
}

/// 0x30 レポートを NES のボタンに直す。
///
/// 対応は `procon_udp.py` の `decode_standard_report` と同じ:
/// - `report[3]` 右側: A / B / Y (Y は連射用に B と重複)
/// - `report[4]` 共有: minus=SELECT / plus=START / HOME
/// - `report[5]` 左側: 十字
/// - `report[6..9]` 左スティック (十字と OR する)
pub fn decode_report(report: &[u8]) -> Option<PadState> {
    let is_usable = report.len() >= 12 && report[0] == REPORT_ID_STANDARD;
    if !is_usable {
        return None;
    }

    let right = report[3];
    let shared = report[4] & SHARED_BYTE_BUTTON_MASK;
    let left = report[5];

    let mut buttons = 0u8;
    // 右側
    if right & 0x08 != 0 {
        buttons |= button::A;
    }
    // B と Y の両方を B に割り当てる (連射しやすいように)
    if right & 0x04 != 0 || right & 0x01 != 0 {
        buttons |= button::B;
    }
    // 共有
    if shared & 0x01 != 0 {
        buttons |= button::SELECT;
    }
    if shared & 0x02 != 0 {
        buttons |= button::START;
    }
    // 左側 (十字)
    if left & 0x02 != 0 {
        buttons |= button::UP;
    }
    if left & 0x01 != 0 {
        buttons |= button::DOWN;
    }
    if left & 0x08 != 0 {
        buttons |= button::LEFT;
    }
    if left & 0x04 != 0 {
        buttons |= button::RIGHT;
    }

    // 左スティックは十字と OR する (排他にしない)
    let stick_x = (report[6] as i32) | (((report[7] & 0x0F) as i32) << 8);
    let stick_y = ((report[7] >> 4) as i32) | ((report[8] as i32) << 4);
    if stick_x < STICK_CENTER - STICK_THRESHOLD {
        buttons |= button::LEFT;
    }
    if stick_x > STICK_CENTER + STICK_THRESHOLD {
        buttons |= button::RIGHT;
    }
    // **Y は上が正**。SDL の軸とは向きが逆
    if stick_y > STICK_CENTER + STICK_THRESHOLD {
        buttons |= button::UP;
    }
    if stick_y < STICK_CENTER - STICK_THRESHOLD {
        buttons |= button::DOWN;
    }

    Some(PadState {
        buttons,
        home: (report[4] & SHARED_HOME) != 0,
    })
}

/// サブコマンドのフレームを組み立てる。
///
/// 64 バイト固定長で、rumble のニュートラル 8 バイトが埋め草として要る。
/// 下位ニブルにパケットカウンタを載せる
pub fn subcommand(counter: u8, id: u8, argument: u8) -> [u8; 64] {
    // rumble のニュートラル。これを省くとコントローラが受け付けない
    const RUMBLE_NEUTRAL: [u8; 8] = [0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40];

    let mut frame = [0u8; 64];
    frame[0] = 0x01;
    frame[1] = counter & 0x0F;
    frame[2..10].copy_from_slice(&RUMBLE_NEUTRAL);
    frame[10] = id;
    frame[11] = argument;
    frame
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 0x30 レポートを組み立てる。スティックは中心に置く
    fn report(right: u8, shared: u8, left: u8) -> Vec<u8> {
        let mut r = vec![0u8; 12];
        r[0] = REPORT_ID_STANDARD;
        r[3] = right;
        r[4] = shared;
        r[5] = left;
        // 中心 (2048) を 12bit パックで置く
        r[6] = (STICK_CENTER & 0xFF) as u8;
        r[7] = ((STICK_CENTER >> 8) & 0x0F) as u8 | (((STICK_CENTER & 0x0F) as u8) << 4);
        r[8] = (STICK_CENTER >> 4) as u8;
        r
    }

    #[test]
    fn the_face_buttons_map_to_the_nes_pad() {
        assert_eq!(
            decode_report(&report(0x08, 0, 0)).unwrap().buttons,
            button::A
        );
        assert_eq!(
            decode_report(&report(0x04, 0, 0)).unwrap().buttons,
            button::B
        );
        // Y も B に割り当てる (連射用)
        assert_eq!(
            decode_report(&report(0x01, 0, 0)).unwrap().buttons,
            button::B
        );
    }

    #[test]
    fn minus_and_plus_are_select_and_start() {
        assert_eq!(
            decode_report(&report(0, 0x01, 0)).unwrap().buttons,
            button::SELECT
        );
        assert_eq!(
            decode_report(&report(0, 0x02, 0)).unwrap().buttons,
            button::START
        );
    }

    #[test]
    fn the_dpad_maps_to_the_directions() {
        assert_eq!(
            decode_report(&report(0, 0, 0x02)).unwrap().buttons,
            button::UP
        );
        assert_eq!(
            decode_report(&report(0, 0, 0x01)).unwrap().buttons,
            button::DOWN
        );
        assert_eq!(
            decode_report(&report(0, 0, 0x08)).unwrap().buttons,
            button::LEFT
        );
        assert_eq!(
            decode_report(&report(0, 0, 0x04)).unwrap().buttons,
            button::RIGHT
        );
    }

    /// byte[4] の bit7 は充電グリップのフラグ。USB 接続ではずっと 1 なので、
    /// 落とさないと何かが押しっぱなしに見える
    #[test]
    fn the_charging_grip_flag_is_not_a_button() {
        let state = decode_report(&report(0, 0x80, 0)).unwrap();
        assert_eq!(state.buttons, 0, "bit 7 must not become a button press");
    }

    #[test]
    fn home_is_reported_separately_from_the_pad() {
        let state = decode_report(&report(0, SHARED_HOME, 0)).unwrap();
        assert!(state.home);
        // HOME は NES のパッドビットには入らない
        assert_eq!(state.buttons, 0);
    }

    /// スティックは 12bit パックで、**Y は上が正** (SDL とは逆)
    #[test]
    fn the_left_stick_is_decoded_with_y_growing_upward() {
        let pack = |x: i32, y: i32| {
            let mut r = report(0, 0, 0);
            r[6] = (x & 0xFF) as u8;
            r[7] = (((x >> 8) & 0x0F) as u8) | (((y & 0x0F) as u8) << 4);
            r[8] = ((y >> 4) & 0xFF) as u8;
            r
        };

        let up = decode_report(&pack(STICK_CENTER, STICK_CENTER + 1000)).unwrap();
        assert_eq!(up.buttons, button::UP, "a raised stick is UP");

        let down = decode_report(&pack(STICK_CENTER, STICK_CENTER - 1000)).unwrap();
        assert_eq!(down.buttons, button::DOWN);

        let left = decode_report(&pack(STICK_CENTER - 1000, STICK_CENTER)).unwrap();
        assert_eq!(left.buttons, button::LEFT);

        let right = decode_report(&pack(STICK_CENTER + 1000, STICK_CENTER)).unwrap();
        assert_eq!(right.buttons, button::RIGHT);
    }

    #[test]
    fn a_centred_stick_presses_nothing() {
        assert_eq!(decode_report(&report(0, 0, 0)).unwrap().buttons, 0);
    }

    /// スティックと十字は OR する (排他にしない)
    #[test]
    fn the_stick_and_dpad_are_combined() {
        let mut r = report(0, 0, 0x08); // 十字の左
        // スティックは右
        let x = STICK_CENTER + 1000;
        r[6] = (x & 0xFF) as u8;
        r[7] = (((x >> 8) & 0x0F) as u8) | (((STICK_CENTER & 0x0F) as u8) << 4);
        r[8] = ((STICK_CENTER >> 4) & 0xFF) as u8;

        let state = decode_report(&r).unwrap();
        assert_eq!(state.buttons, button::LEFT | button::RIGHT);
    }

    #[test]
    fn other_reports_are_ignored() {
        // 別の ID
        let mut wrong_id = report(0x08, 0, 0);
        wrong_id[0] = 0x21;
        assert!(decode_report(&wrong_id).is_none());
        // 短すぎる
        assert!(decode_report(&report(0, 0, 0)[..11]).is_none());
        assert!(decode_report(&[]).is_none());
    }

    /// サブコマンドは 64 バイト固定長で、rumble のニュートラルが要る。
    /// 省くとコントローラが受け付けない
    #[test]
    fn a_subcommand_frame_is_the_expected_shape() {
        let frame = subcommand(5, SUBCMD_SET_INPUT_REPORT_MODE, REPORT_ID_STANDARD);
        assert_eq!(frame.len(), 64);
        assert_eq!(frame[0], 0x01);
        assert_eq!(frame[1], 5, "the packet counter goes in the low nibble");
        assert_eq!(
            &frame[2..10],
            &[0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40]
        );
        assert_eq!(frame[10], SUBCMD_SET_INPUT_REPORT_MODE);
        assert_eq!(frame[11], REPORT_ID_STANDARD);
    }

    #[test]
    fn the_packet_counter_wraps_within_a_nibble() {
        assert_eq!(subcommand(0x1F, 0, 0)[1], 0x0F);
    }
}
