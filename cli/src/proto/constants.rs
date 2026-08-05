//! プロトコル定数。
//!
//! 権威は `m5stack/src/config.h` で、ここはその写し。各定数に対応する C++ 側の
//! 名前を書いてあるので、食い違いを疑ったときはそちらを引くこと
//! (`tools/serve_web.py` も同じ値を別に持っており、実装は三重にある)。

/// `UDP_PORT`
pub const DEFAULT_PORT: u16 = 5555;

/// リクエストの magic。`'N','P'` = NES Protocol
pub const MAGIC: [u8; 2] = *b"NP";

/// `UDP_PROTOCOL_VERSION`
pub const VERSION: u8 = 1;

/// `UDP_PACKET_SIZE`。全 type 共通のヘッダ長で、これ未満のデータグラムは
/// firmware が読まずに捨てる
pub const HEADER_SIZE: usize = 8;

/// `INPUT_TIMEOUT_MS`。これだけパケットが途絶えると firmware は両パッドを
/// 離した状態に戻す。送信側は押しっぱなしを表現するために再送し続ける必要がある
pub const INPUT_TIMEOUT_MS: u64 = 500;

// ------------------------------------------------------------------ パッド

/// NES のボタンビット (`NES_BTN_*`)。`Pad::setButtons` と同じ並び
pub mod button {
    pub const A: u8 = 0x01;
    pub const B: u8 = 0x02;
    pub const SELECT: u8 = 0x04;
    pub const START: u8 = 0x08;
    pub const UP: u8 = 0x10;
    pub const DOWN: u8 = 0x20;
    pub const LEFT: u8 = 0x40;
    pub const RIGHT: u8 = 0x80;
}

// -------------------------------------------------------------------- ピン

/// `UDP_PIN_PACKET_SIZE`
pub const PIN_PACKET_SIZE: usize = 14;

/// `PIN_MASK_VALID`。bit 0..59 が pin 1..60 に対応し、上位 4bit は firmware が
/// 落とす。送信前にこちらでも落としておかないと、「全 pin 正常」を `!0` で
/// 書いた値と、ブラウザが 60 pin から組み立てた値が別物に見える
pub const PIN_MASK_VALID: u64 = (1 << 60) - 1;

/// `PIN_MASK_ALL_OK`。正しく挿さったカートリッジ
pub const PIN_MASK_ALL_OK: u64 = PIN_MASK_VALID;

// ------------------------------------------------------------- 本体制御

/// `UDP_CTRL_RESET`。ワーク RAM を保持したままリセットベクタから起動し直す
pub const CTRL_RESET: u8 = 0x01;

/// `UDP_CTRL_VOLUME`。音量は byte[7]
pub const CTRL_VOLUME: u8 = 0x02;

/// `UDP_CTRL_MENU`。ROM 選択メニューを開く
pub const CTRL_MENU: u8 = 0x04;

// ---------------------------------------------------------------- デバッグ

/// `UDP_DEBUG_FLAG_WAVES`。APU の波形も要求する
pub const DEBUG_FLAG_WAVES: u8 = 0x01;

/// `UDP_DEBUG_HEADER`。`'N','D' | version | part | nparts | seq u16 LE`
pub const DEBUG_HEADER: usize = 7;

/// 応答の magic
pub const DEBUG_MAGIC: [u8; 2] = *b"ND";

/// `UDP_DEBUG_PARTS`。上限であって固定数ではない (実際の数は応答の nparts)
pub const DEBUG_MAX_PARTS: u8 = 4;

/// `UDP_DEBUG_CHUNK`
pub const DEBUG_CHUNK: usize = 1400;

/// 波形なしのスナップショット長
pub const DEBUG_SNAPSHOT_SIZE: usize = 2134;

/// 波形ありのスナップショット長
pub const DEBUG_SNAPSHOT_WITH_WAVES_SIZE: usize = 3814;

// --------------------------------------------------------------- ROM 転送

/// `UDP_ROM_OP_*`
pub const ROM_OP_BEGIN: u8 = 0;
pub const ROM_OP_DATA: u8 = 1;
pub const ROM_OP_END: u8 = 2;
pub const ROM_OP_ABORT: u8 = 3;

/// `UDP_ROM_BEGIN_SIZE`。名前を持たない旧形式の BEGIN
pub const ROM_BEGIN_SIZE: usize = 16;

/// `UDP_ROM_BEGIN_NAMED_SIZE`。名前付き BEGIN の最小長。
///
/// **長さで判別する**のが仕様。旧クライアントはちょうど `ROM_BEGIN_SIZE` を
/// 送るので、それより長ければ新形式と一意に決まる。フラグで判別しようとすると
/// 旧ファームが予約ビットを取っておく必要があった
pub const ROM_BEGIN_NAMED_SIZE: usize = ROM_BEGIN_SIZE + 1;

/// `UDP_ROM_DATA_HEADER`
pub const ROM_DATA_HEADER: usize = 12;

/// `UDP_ROM_END_SIZE`。END / ABORT は共通ヘッダのみ
pub const ROM_END_SIZE: usize = HEADER_SIZE;

/// `UDP_ROM_CHUNK`。1500 バイト MTU に収まる大きさ
pub const ROM_CHUNK: usize = 1400;

/// `UDP_ROM_ACK_SIZE`
pub const ROM_ACK_SIZE: usize = 12;

/// ROM ACK の magic
pub const ROM_ACK_MAGIC: [u8; 2] = *b"NR";

/// `ROM_MAX_SIZE`。対応マッパー (0/1/2/3/4/24/26) の上限が 768KB 付近なので
/// 余裕を見た値
pub const ROM_MAX_SIZE: usize = 1024 * 1024;

/// `ROM_FLAG_SWAP`。CPU をリセットせずに差し替える
pub const ROM_FLAG_SWAP: u8 = 0x01;

/// `ROM_FLAG_SAVE_SD`。SD にも保存する。BEGIN 末尾のファイル名が要る。
///
/// **保存に失敗したときはロードもしない。**「カードに置いて起動する」の後半だけを
/// 叶えると、遊んでいたゲームを止めた上にカードにも残らず、再起動でどちらも失われる
pub const ROM_FLAG_SAVE_SD: u8 = 0x02;

/// `ROM_FLAG_NO_LOAD`。ロードせず保存だけ行う
pub const ROM_FLAG_NO_LOAD: u8 = 0x04;

/// `UDP_ROM_SAVE_EVENT_SIZE`。SD 保存の結果は END ACK とは**別のデータグラム**で
/// 返る。ACK は CRC が通った時点で UDP タスクが即返すのに対し、カードへの書き込みは
/// 後で core 1 のフレーム境界で走るため
pub const ROM_SAVE_EVENT_SIZE: usize = 8;

/// `ROM_SESSION_TIMEOUT_MS`
pub const ROM_SESSION_TIMEOUT_MS: u64 = 3000;

// --------------------------------------------------------------- SD 操作

/// `UDP_SD_OP_*`
pub const SD_OP_LIST: u8 = 0;
pub const SD_OP_LOAD: u8 = 1;
pub const SD_OP_DELETE: u8 = 2;
pub const SD_OP_RENAME: u8 = 3;

/// `UDP_SD_HEADER`
pub const SD_HEADER: usize = 8;

/// `UDP_SD_ACK_SIZE`
pub const SD_ACK_SIZE: usize = 8;

/// SD 応答の magic。ROM の保存イベントも同じ magic を使う
pub const SD_MAGIC: [u8; 2] = *b"NS";

/// `UDP_SD_LIST_HEADER`
pub const SD_LIST_HEADER: usize = 30;

/// `UDP_SD_CHUNK`
pub const SD_CHUNK: usize = 1400;

/// 名前の最大バイト数。`SD_ROM_NAME_MAX` (64) は NUL 込みなので、線上に載る
/// 実効長はこれ
pub const SD_NAME_MAX: usize = 63;

/// `SD_ROM_MAX_FILES`。一覧に載る本数の上限
pub const SD_MAX_FILES: usize = 64;

/// `UDP_SD_ENTRY_MAX`。1 エントリの最悪バイト数 (size u32 + nameLen u8 + 名前)
pub const SD_ENTRY_MAX: usize = 4 + 1 + 64;

/// 1 データグラムに入るエントリ数。firmware の `perPart` と同じ式
/// (`m5stack/src/main.cpp:1419`) で導く — 定数を直書きすると、チャンク長や
/// 名前長が動いたときに黙ってずれる
pub const SD_ENTRIES_PER_PART: usize = (SD_CHUNK - SD_LIST_HEADER) / SD_ENTRY_MAX;

/// LIST 応答のパート数の上限。firmware の `nparts` と同じ式
pub const SD_MAX_PARTS: usize = SD_MAX_FILES.div_ceil(SD_ENTRIES_PER_PART);

#[cfg(test)]
mod tests {
    use super::*;

    // ボタンビットは 8bit を隙間なく 1 つずつ占める。重複や欠けがあると
    // 「A を押したのに B が入る」形で静かに壊れる
    #[test]
    fn button_bits_cover_each_bit_exactly_once() {
        let all = [
            button::A,
            button::B,
            button::SELECT,
            button::START,
            button::UP,
            button::DOWN,
            button::LEFT,
            button::RIGHT,
        ];
        let mut seen = 0u8;
        for bit in all {
            assert_eq!(bit.count_ones(), 1, "{bit:#04x} is not a single bit");
            assert_eq!(seen & bit, 0, "{bit:#04x} is used twice");
            seen |= bit;
        }
        assert_eq!(seen, 0xFF, "the eight buttons must fill the byte");
    }

    #[test]
    fn pin_mask_covers_sixty_pins() {
        assert_eq!(PIN_MASK_VALID.count_ones(), 60);
        assert_eq!(PIN_MASK_ALL_OK, PIN_MASK_VALID);
        // 上位 4bit は無効。ここが立つ値を送ると firmware 側で落とされ、
        // 送った値と適用された値が食い違う
        assert_eq!(PIN_MASK_VALID >> 60, 0);
    }

    // 名前付き BEGIN は長さで判別する。この 2 つが等しくなると旧形式と
    // 区別できなくなる
    #[test]
    fn named_begin_is_longer_than_the_bare_one() {
        assert!(ROM_BEGIN_NAMED_SIZE > ROM_BEGIN_SIZE);
    }

    // 分割応答は 1 データグラムに最低 1 エントリ入らないと前に進めない
    // (config.h の static_assert と同じ主張)
    #[test]
    fn a_list_datagram_holds_at_least_one_entry() {
        let entry_max = 4 + 1 + SD_NAME_MAX + 1; // size u32 | nameLen u8 | name (NUL 込みの余地)
        assert!(SD_LIST_HEADER + entry_max <= SD_CHUNK);
    }

    #[test]
    fn control_bits_are_distinct() {
        assert_eq!(CTRL_RESET & CTRL_VOLUME, 0);
        assert_eq!(CTRL_RESET & CTRL_MENU, 0);
        assert_eq!(CTRL_VOLUME & CTRL_MENU, 0);
    }

    #[test]
    fn rom_flags_are_distinct() {
        assert_eq!(ROM_FLAG_SWAP & ROM_FLAG_SAVE_SD, 0);
        assert_eq!(ROM_FLAG_SWAP & ROM_FLAG_NO_LOAD, 0);
        assert_eq!(ROM_FLAG_SAVE_SD & ROM_FLAG_NO_LOAD, 0);
    }
}
