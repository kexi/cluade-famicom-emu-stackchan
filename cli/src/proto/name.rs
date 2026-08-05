//! ファイル名の検証。SD の各 op と ROM の保存名で共通に使う。
//!
//! 検査をクライアント側でも行うのは、通らないと判っている往復を省くためだが、
//! **NUL の拒否だけは安全のために必須**。firmware の `readNameField`
//! (`m5stack/src/main.cpp:181`) は長さぶんを `memcpy` してから `out[len] = '\0'`
//! を書き、以降は C 文字列として扱う。つまり名前の途中に NUL があると、
//! firmware から見える名前はそこで切れる。
//!
//! Rust の `&str` は NUL を含めるので、`"game.nes\0other"` を `sd rm` に渡すと
//! **利用者が指定したのとは別のファイル (`game.nes`) が消える**。長さや文字種の
//! 検査だけでは防げない。

use super::constants::SD_NAME_MAX;

/// 名前を検証する。通れば線上に載せてよい
pub fn validate(name: &str, what: &str) -> Result<(), String> {
    let bytes = name.as_bytes();

    let is_empty = bytes.is_empty();
    if is_empty {
        return Err(format!("{what} is empty"));
    }

    // NUL は firmware から見える名前を途中で切る。別のファイルを指す名前に
    // 化けるので、長さ検査より先に弾く
    let has_nul = bytes.contains(&0);
    if has_nul {
        return Err(format!(
            "{what} contains a NUL byte; the device would act on a different file"
        ));
    }

    let is_too_long = bytes.len() > SD_NAME_MAX;
    if is_too_long {
        return Err(format!(
            "{what} is {} bytes, the device accepts at most {SD_NAME_MAX}",
            bytes.len()
        ));
    }

    Ok(())
}

/// 一覧に載ってきた名前が、firmware のサニタイザ
/// (`sdRomSanitizeName`, `m5stack/src/sd_rom.cpp:599`) の**文字種と長さの制約**に
/// 合うか。
///
/// サニタイザは拡張子の付与や正規化もするが、そこまでは見ない — 対応拡張子が
/// 増えたときに CLI が先に壊れる形の結合を避けるため。文字種と長さだけを見るのは、
/// そこが「表示だけできて LOAD も DELETE もできない行」を作らないための条件だから。
///
/// 通らない名前は正常な応答ではないので、パートごと捨てて再送させる
pub fn is_listable(name: &str) -> bool {
    let bytes = name.as_bytes();
    let has_a_sane_length = !bytes.is_empty() && bytes.len() <= SD_NAME_MAX;
    if !has_a_sane_length {
        return false;
    }
    // サニタイザは許可外のバイトを `_` に置き換えるので、載ってくる名前は
    // この文字種だけでできている
    bytes
        .iter()
        .all(|&b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
}

/// `nameLen u8 | name[nameLen]` を組み立てる
pub fn field(name: &str, what: &str) -> Result<Vec<u8>, String> {
    validate(name, what)?;
    let bytes = name.as_bytes();
    let mut out = Vec::with_capacity(1 + bytes.len());
    out.push(bytes.len() as u8);
    out.extend_from_slice(bytes);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ordinary_names_pass() {
        assert!(validate("game.nes", "file name").is_ok());
        // firmware は `[A-Za-z0-9._-]` を許すので `-` 始まりは実在しうる
        assert!(validate("-x.nes", "file name").is_ok());
    }

    // 名前の途中の NUL は firmware から見える名前を切り、別のファイルを指す
    #[test]
    fn embedded_nul_is_rejected() {
        let err = validate("game.nes\0other", "file name").unwrap_err();
        assert!(err.contains("NUL"), "the message should say why: {err}");

        assert!(validate("\0", "file name").is_err());
        assert!(validate("a\0b", "file name").is_err());
        assert!(validate("game.nes\0", "file name").is_err());
    }

    #[test]
    fn empty_names_are_rejected() {
        assert!(validate("", "file name").is_err());
    }

    #[test]
    fn length_is_measured_in_bytes() {
        assert!(validate(&"x".repeat(SD_NAME_MAX), "file name").is_ok());
        assert!(validate(&"x".repeat(SD_NAME_MAX + 1), "file name").is_err());

        // 3 バイト x 21 = 63
        assert!(validate(&"あ".repeat(21), "file name").is_ok());
        assert!(validate(&"あ".repeat(22), "file name").is_err());
    }

    #[test]
    fn listable_accepts_what_the_sanitiser_produces() {
        for name in ["game.nes", "-x.nes", "a_b-c.1.nes", "rom.nes"] {
            assert!(is_listable(name), "{name} should be listable");
        }
    }

    #[test]
    fn listable_rejects_what_the_sanitiser_would_have_replaced() {
        // サニタイザは許可外を `_` にするので、これらが載ってくることはない
        for name in ["a/b.nes", "a b.nes", "あ.nes", "a\0b", ""] {
            assert!(!is_listable(name), "{name:?} should not be listable");
        }
        assert!(!is_listable(&"x".repeat(SD_NAME_MAX + 1)));
    }

    #[test]
    fn field_is_length_prefixed() {
        assert_eq!(field("ab", "file name").unwrap(), vec![2, b'a', b'b']);
    }

    // 検証を通らない名前でフィールドを作らない
    #[test]
    fn field_rejects_what_validate_rejects() {
        assert!(field("", "file name").is_err());
        assert!(field("a\0b", "file name").is_err());
    }
}
