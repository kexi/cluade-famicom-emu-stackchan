//! プロトコル定数が `m5stack/src/config.h` と一致することを検査する。
//!
//! 同じ仕様の実装がこのリポジトリには 3 つある — firmware (`config.h`)、
//! この CLI (`cli/src/proto/constants.rs`)、Python の中継 (`tools/serve_web.py`)。
//! 正本は `config.h` で、ここはその写しが古びていないことを機械的に確かめる。
//!
//! **値の食い違いは静かに壊れる。** 型が合っている限りコンパイルは通り、
//! テストも (両側が同じ間違った値を使うので) 通ってしまう。実機に繋いだ
//! ときだけ、パケットが黙って捨てられるか、別の type として解釈される。
//!
//! コード生成にしなかったのは、`config.h` の 600 行のうち UDP 定数は一部で、
//! 抽出スクリプトのほうが読む対象より複雑になるため。突き合わせだけなら
//! この 1 ファイルで足りる。

use std::collections::HashMap;
use std::path::PathBuf;

use stackchan::proto::constants as rust;

/// `config.h` の場所。`CARGO_MANIFEST_DIR` は `cli/` を指す
fn config_h() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("cli/ has a parent")
        .join("m5stack/src/config.h")
}

/// `constexpr <型> <名前> = <値>;` を拾う。
///
/// 値は 10 進か 16 進のリテラルだけを受ける。`(1ULL << 60) - 1` のような式は
/// 評価せずに捨てる — ここで C++ の式を解釈しはじめると、この検査自身が
/// 間違いうるコードになる。式で書かれた定数は個別のテストで見る
fn parse(source: &str) -> HashMap<String, u64> {
    let mut found = HashMap::new();

    for line in source.lines() {
        let Some(rest) = line.trim().strip_prefix("constexpr ") else {
            continue;
        };
        // <型> <名前> = <値>; の形。型に空白は入らない (unsigned long long は
        // config.h では使われておらず、uint64_t で書かれている)
        let Some((declaration, tail)) = rest.split_once('=') else {
            continue;
        };
        let mut words = declaration.split_whitespace();
        let (Some(_type), Some(name)) = (words.next(), words.next()) else {
            continue;
        };
        let is_a_plain_declaration = words.next().is_none();
        if !is_a_plain_declaration {
            continue;
        }

        let Some((literal, _)) = tail.split_once(';') else {
            continue;
        };
        let Some(value) = parse_literal(literal.trim()) else {
            continue;
        };
        found.insert(name.to_string(), value);
    }

    found
}

/// `0x1F`、`5555`、`64 * 1024`、`25000000` を読む。それ以外は `None`
fn parse_literal(raw: &str) -> Option<u64> {
    let cleaned = raw.trim_end_matches(['u', 'U', 'l', 'L']).trim();

    // 掛け算だけは通す。config.h に `64 * 1024` の形が実在する
    if let Some((left, right)) = cleaned.split_once('*') {
        let left = parse_literal(left.trim())?;
        let right = parse_literal(right.trim())?;
        return left.checked_mul(right);
    }

    let is_hex = cleaned.starts_with("0x") || cleaned.starts_with("0X");
    if is_hex {
        return u64::from_str_radix(&cleaned[2..], 16).ok();
    }
    cleaned.parse().ok()
}

/// C++ の名前 → この CLI が持っている値。
///
/// 名前が違うのは、CLI 側では `UDP_` の接頭辞が意味を持たないため
/// (モジュールが `proto` なので `proto::constants::DEFAULT_PORT` で読める)
fn mirrored() -> Vec<(&'static str, u64)> {
    vec![
        ("UDP_PORT", rust::DEFAULT_PORT as u64),
        ("UDP_PROTOCOL_VERSION", rust::VERSION as u64),
        ("UDP_PACKET_SIZE", rust::HEADER_SIZE as u64),
        ("INPUT_TIMEOUT_MS", rust::INPUT_TIMEOUT_MS),
        ("UDP_PIN_PACKET_SIZE", rust::PIN_PACKET_SIZE as u64),
        ("UDP_CTRL_RESET", rust::CTRL_RESET as u64),
        ("UDP_CTRL_VOLUME", rust::CTRL_VOLUME as u64),
        ("UDP_CTRL_MENU", rust::CTRL_MENU as u64),
        ("UDP_DEBUG_FLAG_WAVES", rust::DEBUG_FLAG_WAVES as u64),
        ("UDP_DEBUG_HEADER", rust::DEBUG_HEADER as u64),
        ("UDP_DEBUG_PARTS", rust::DEBUG_MAX_PARTS as u64),
        ("UDP_DEBUG_CHUNK", rust::DEBUG_CHUNK as u64),
        ("UDP_ROM_OP_BEGIN", rust::ROM_OP_BEGIN as u64),
        ("UDP_ROM_OP_DATA", rust::ROM_OP_DATA as u64),
        ("UDP_ROM_OP_END", rust::ROM_OP_END as u64),
        ("UDP_ROM_OP_ABORT", rust::ROM_OP_ABORT as u64),
        ("UDP_ROM_BEGIN_SIZE", rust::ROM_BEGIN_SIZE as u64),
        ("UDP_ROM_DATA_HEADER", rust::ROM_DATA_HEADER as u64),
        ("UDP_ROM_END_SIZE", rust::ROM_END_SIZE as u64),
        ("UDP_ROM_CHUNK", rust::ROM_CHUNK as u64),
        ("UDP_ROM_ACK_SIZE", rust::ROM_ACK_SIZE as u64),
        ("UDP_SD_HEADER", rust::SD_HEADER as u64),
        ("UDP_SD_ACK_SIZE", rust::SD_ACK_SIZE as u64),
        ("UDP_SD_LIST_HEADER", rust::SD_LIST_HEADER as u64),
        ("UDP_SD_CHUNK", rust::SD_CHUNK as u64),
        ("SD_ROM_MAX_FILES", rust::SD_MAX_FILES as u64),
        ("ROM_SESSION_TIMEOUT_MS", rust::ROM_SESSION_TIMEOUT_MS),
    ]
}

#[test]
fn every_mirrored_constant_matches_the_firmware() {
    let source = std::fs::read_to_string(config_h()).expect("config.h is readable from cli/");
    let firmware = parse(&source);

    let mut wrong = Vec::new();
    for (name, ours) in mirrored() {
        let Some(&theirs) = firmware.get(name) else {
            wrong.push(format!(
                "{name}: not found in config.h (renamed or removed?)"
            ));
            continue;
        };
        if ours != theirs {
            wrong.push(format!("{name}: config.h has {theirs}, the CLI has {ours}"));
        }
    }

    assert!(
        wrong.is_empty(),
        "the CLI's copy of the protocol has drifted from m5stack/src/config.h:\n  {}",
        wrong.join("\n  ")
    );
}

/// パーサが実際に値を拾えていることを確かめる。
///
/// 何も拾えなくても上のテストは「見つからない」で落ちるが、拾えた数が
/// 少しだけ減る壊れ方 (型の書式が変わった等) は気づきにくい
#[test]
fn the_parser_finds_the_constants_it_is_supposed_to() {
    let source = std::fs::read_to_string(config_h()).expect("config.h is readable from cli/");
    let firmware = parse(&source);

    for (name, _) in mirrored() {
        assert!(
            firmware.contains_key(name),
            "the parser did not find {name} in config.h"
        );
    }
}

#[test]
fn the_parser_reads_the_literal_forms_config_h_uses() {
    assert_eq!(parse_literal("5555"), Some(5555));
    assert_eq!(parse_literal("0x01"), Some(1));
    assert_eq!(parse_literal("0xFF"), Some(255));
    assert_eq!(parse_literal("64 * 1024"), Some(65536));
    assert_eq!(parse_literal("25000000"), Some(25_000_000));
    // 式は評価しない
    assert_eq!(parse_literal("(1ULL << 60) - 1"), None);
}

/// 式で書かれていてパーサが読まない定数は、値を直に書いて縛る。
///
/// `PIN_MASK_VALID` は `config.h` で `(1ULL << 60) - 1`、CLI で `(1 << 60) - 1`。
/// どちらも式なので突き合わせから漏れるが、これは 60 ピンという仕様そのもの
#[test]
fn the_pin_mask_covers_exactly_sixty_pins() {
    assert_eq!(rust::PIN_MASK_VALID, (1u64 << 60) - 1);
    assert_eq!(rust::PIN_MASK_VALID.count_ones(), 60);
    assert_eq!(rust::PIN_MASK_ALL_OK, rust::PIN_MASK_VALID);
}
