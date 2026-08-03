//! 終了コード規約。
//!
//! GNU は 0 と「0 以外」しか規定していないので、1 以外の値の割り当ては各コマンドの
//! 裁量 (`diff` の 0/1/2、`grep` の 0/1/2 と同じ)。ここで 3 と 4 を 1 から分けて
//! いるのは、呼び出し側 — とりわけスクリプトや AI — が「送ったが結果が判らない」
//! を「失敗した」と取り違えないようにするため。
//!
//! DELETE / RENAME は再送できない (ファームに per-seq の結果キャッシュが無く、
//! 2 回目の DELETE は NotFound、2 回目の RENAME は Exists を返して成功が失敗に
//! 化ける)。そのため 1 回送って無応答なら、実行されたかどうかは原理的に判らない。
//! これを 1 (失敗) として返すと、呼び出し側は「消えていない」と誤解して別の手を
//! 打ってしまう。4 を見たら `sd ls` で確かめる、が正しい反応になる。

/// `--help` の EXIT STATUS 節に出す一覧。実装と表示を 1 か所に閉じ込めるため、
/// この定数から生成する
pub const EXIT_STATUS_HELP: &str = "\
Exit status:
  0  success
  1  the device refused the request, or the operation failed locally
  2  usage error (bad or missing arguments)
  3  the device did not answer in time
  4  the request was sent but the outcome is unknown; it may have taken effect";

/// 終了コード。`u8` ではなく専用の型にしているのは、`std::process::exit` に
/// 生の数値を渡す箇所を 1 つに絞り、規約の外の値が紛れ込まないようにするため
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExitCode {
    Success = 0,
    Failure = 1,
    Usage = 2,
    Timeout = 3,
    Unknown = 4,
}

impl ExitCode {
    pub fn code(self) -> i32 {
        self as i32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // 規約の数値そのものが対外的な約束なので、値を変えたらテストが落ちるようにする
    #[test]
    fn codes_match_the_documented_convention() {
        assert_eq!(ExitCode::Success.code(), 0);
        assert_eq!(ExitCode::Failure.code(), 1);
        assert_eq!(ExitCode::Usage.code(), 2);
        assert_eq!(ExitCode::Timeout.code(), 3);
        assert_eq!(ExitCode::Unknown.code(), 4);
    }

    // EXIT STATUS の記述と enum が食い違うと、--help が嘘をつくことになる
    #[test]
    fn help_text_documents_every_code() {
        for code in ["  0  ", "  1  ", "  2  ", "  3  ", "  4  "] {
            assert!(
                EXIT_STATUS_HELP.contains(code),
                "EXIT STATUS help is missing {code:?}"
            );
        }
    }
}
