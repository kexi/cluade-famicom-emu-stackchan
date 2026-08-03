//! `tools/serve_web.py` が組み立てるバイト列と一致することを確かめる。
//!
//! プロトコルの実装は `m5stack/src/config.h` (権威)、`tools/serve_web.py`、
//! そしてこのクレートの三重にある。コード生成で 1 つにまとめるより、
//! 「既に実機と通信できている実装」の出力そのものを固定値として置くほうが、
//! 食い違いを直接踏める。
//!
//! 期待値は Python 側を実際に走らせて採取したもの。更新するときは
//! `tools/serve_web.py` の `build_*` を叩いて hex を採り直すこと。

use stackchan::proto::constants::{
    CTRL_RESET, CTRL_VOLUME, DEBUG_FLAG_WAVES, PIN_MASK_ALL_OK, button,
};
use stackchan::proto::{ctrl, debug, pad, pins, rom, sd};

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn pin_packets_match_serve_web() {
    assert_eq!(
        hex(&pins::mask(0x0102, 0x0F)),
        "4e50010102010f00000000000000"
    );
    assert_eq!(
        hex(&pins::mask(0, PIN_MASK_ALL_OK)),
        "4e5001010000ffffffffffffff0f"
    );
}

#[test]
fn control_packets_match_serve_web() {
    assert_eq!(hex(&ctrl::reset(0x0102)), "4e50010202010100");
    assert_eq!(hex(&ctrl::volume(0, 192)), "4e500102000002c0");
    // ビットの値そのものも Python 側と同じ
    assert_eq!(CTRL_RESET, 0x01);
    assert_eq!(CTRL_VOLUME, 0x02);
}

#[test]
fn rom_packets_match_serve_web() {
    let options = rom::RomOptions::default();
    assert_eq!(
        hex(&rom::begin(0x1234, b"rom", &options).unwrap()),
        "4e5001043412000003000000a10f5279"
    );

    // 保存のみ (flags = SAVE_SD)
    let named = rom::RomOptions {
        save_as: Some("game.nes".to_string()),
        ..Default::default()
    };
    assert_eq!(
        hex(&rom::begin(1, b"rom", &named).unwrap()),
        "4e5001040100000203000000a10f52790867616d652e6e6573"
    );

    // swap のみ (flags = SWAP)。名前が無いので 16 バイトのまま
    let swap = rom::RomOptions {
        swap: true,
        ..Default::default()
    };
    assert_eq!(
        hex(&rom::begin(1, b"rom", &swap).unwrap()),
        "4e5001040100000103000000a10f5279"
    );

    // 保存だけしてロードしない (flags = SAVE_SD | NO_LOAD)
    let save_only = rom::RomOptions {
        save_as: Some("a.nes".to_string()),
        no_load: true,
        ..Default::default()
    };
    assert_eq!(
        hex(&rom::begin(1, b"rom", &save_only).unwrap()),
        "4e5001040100000603000000a10f527905612e6e6573"
    );

    assert_eq!(
        hex(&rom::data(0x1234, 5, b"payload").unwrap()),
        "4e50010434120100050007007061796c6f6164"
    );
    assert_eq!(hex(&rom::end(0x1234)), "4e50010434120200");
    assert_eq!(hex(&rom::abort(0x1234)), "4e50010434120300");
}

#[test]
fn sd_packets_match_serve_web() {
    assert_eq!(hex(&sd::list(0x0102)), "4e50010502010000");
    assert_eq!(
        hex(&sd::load(1, "game.nes").unwrap()),
        "4e500105010001000867616d652e6e6573"
    );
    assert_eq!(
        hex(&sd::rename(1, "a.nes", "b.nes").unwrap()),
        "4e5001050100030005612e6e657305622e6e6573"
    );
}

#[test]
fn debug_requests_match_serve_web() {
    assert_eq!(
        hex(&debug::request_snapshot(0x1234, false)),
        "4e50010334120000"
    );
    assert_eq!(hex(&debug::request_snapshot(1, true)), "4e50010301000100");
    assert_eq!(DEBUG_FLAG_WAVES, 0x01);
}

/// パッドは `tools/procon_udp.py` の `build_packet` に対応する。
/// type 欄が 0 なのは旧 `reserved` 名義との互換
#[test]
fn pad_packets_match_procon_udp() {
    assert_eq!(
        hex(&pad::state(0x0102, button::A, button::START)),
        "4e50010002010108"
    );
}

/// CRC は zlib と同じ多項式でなければ、転送が毎回 CRC エラーで落ちる
#[test]
fn checksum_matches_zlib() {
    // Python: zlib.crc32(b"rom") = 0x795 20f a1 -> LE で a10f5279
    assert_eq!(rom::checksum(b"rom"), 0x7952_0FA1);
    assert_eq!(rom::checksum(b"123456789"), 0xCBF4_3926);
}
