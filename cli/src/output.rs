//! 出力。既定は人が読む形、`--json` で機械可読。
//!
//! **成功も失敗も同じ形式で出す。** 成功だけ JSON、失敗だけテキストにすると、
//! 呼び出し側は 2 通りのパーサを持つことになる。`--json` のときはエラーも
//! stdout に JSON で出しつつ、同じ内容を stderr にも 1 行書く (パイプの先が
//! 読むのは stdout、人が見るのは端末という使い分けが両立する)。
//!
//! これが効くのは**引数を解釈し終えた後の実行エラー**だけ。引数そのものが
//! 通らなかった場合 (未知のオプション、必須引数の不足) は `--json` の有無に
//! かかわらず stdout を汚さず stderr のみに出す — その時点では出力形式の
//! 指定が有効かどうかも確定していないため。
//!
//! **終了コードは `--json` の有無で変えない。** 出力形式の指定が成否の判定を
//! 変えてはいけない。

use std::io::Write;

use crate::exit::ExitCode;

/// 出力の指定
#[derive(Debug, Clone, Copy)]
pub struct Output {
    pub json: bool,
    /// 成功時の stdout を抑える。終了コードだけ見たいとき用
    pub quiet: bool,
}

impl Output {
    pub fn new(json: bool, quiet: bool) -> Self {
        Self { json, quiet }
    }

    /// 成功を報告する。`text` は人向け、`json` は機械向け
    pub fn success(&self, text: &str, json: &str) {
        if self.quiet {
            return;
        }
        if self.json {
            println!("{json}");
            return;
        }
        let is_empty = text.is_empty();
        if is_empty {
            return;
        }
        println!("{text}");
    }

    /// エラーを報告して終了コードを返す。
    ///
    /// `--quiet` でも黙らせない — 失敗を隠すと、呼び出し側は終了コードだけを
    /// 頼りに原因を推測することになる
    pub fn failure(&self, message: &str, code: ExitCode) -> ExitCode {
        let program = env!("CARGO_PKG_NAME");
        if self.json {
            println!(
                "{{\"ok\":false,\"error\":{},\"exit\":{}}}",
                quote(message),
                code.code()
            );
        }
        let mut stderr = std::io::stderr();
        let _ = writeln!(stderr, "{program}: {message}");
        code
    }
}

/// JSON の文字列リテラルとして安全な形に包む。
///
/// SD のファイル名など、こちらが決めていない文字列を載せるので、
/// エスケープを省くと壊れた JSON を出しうる
pub fn quote(value: &str) -> String {
    let mut out = String::with_capacity(value.len() + 2);
    out.push('"');
    for c in value.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            // 制御文字は \u 形式でなければ JSON として不正
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// バイト数を人が読める形にする
pub fn human_bytes(bytes: u64) -> String {
    const UNITS: [(&str, u64); 4] = [
        ("GB", 1_000_000_000),
        ("MB", 1_000_000),
        ("KB", 1_000),
        ("B", 1),
    ];
    for (unit, scale) in UNITS {
        let is_big_enough = bytes >= scale;
        if is_big_enough {
            let is_whole = scale == 1;
            if is_whole {
                return format!("{bytes} {unit}");
            }
            return format!("{:.1} {unit}", bytes as f64 / scale as f64);
        }
    }
    "0 B".to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quote_escapes_what_would_break_json() {
        assert_eq!(quote("plain"), "\"plain\"");
        assert_eq!(quote("say \"hi\""), "\"say \\\"hi\\\"\"");
        assert_eq!(quote("back\\slash"), "\"back\\\\slash\"");
        assert_eq!(quote("two\nlines"), "\"two\\nlines\"");
        assert_eq!(quote("tab\there"), "\"tab\\there\"");
    }

    // 制御文字を素通しすると JSON として不正になる
    #[test]
    fn quote_escapes_control_characters() {
        assert_eq!(quote("\u{1}"), "\"\\u0001\"");
        assert_eq!(quote("\u{1f}"), "\"\\u001f\"");
    }

    #[test]
    fn quote_keeps_non_ascii_as_is() {
        // UTF-8 はそのまま載せてよい
        assert_eq!(quote("あ"), "\"あ\"");
    }

    #[test]
    fn human_bytes_picks_a_readable_unit() {
        assert_eq!(human_bytes(0), "0 B");
        assert_eq!(human_bytes(512), "512 B");
        assert_eq!(human_bytes(1_500), "1.5 KB");
        assert_eq!(human_bytes(40_976), "41.0 KB");
        assert_eq!(human_bytes(1_200_000_000), "1.2 GB");
    }
}
