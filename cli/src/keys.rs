//! 端末のキー入力をパッドに写す。
//!
//! **TTY はキーを離したことを教えてくれない。** 押している間ずっとイベントが
//! 来るわけでもなく、来るのは OS のキーリピートだけ。しかも最初のリピートまで
//! には数百ミリ秒の間があり、そのあいだは押していても何も届かない。
//! そこで「最後にキーが来てから N ms は押されたまま」とみなす (`DEFAULT_STICKY`)。
//!
//! この方式の限界も書いておく: 端末によっては複数キーを押しても最後の 1 つしか
//! リピートしないので、長い同時押しは維持できないことがある。確実に押し続けたい
//! ときは `input send` を使う。
//!
//! `crossterm` を足さずに `libc` の `tcsetattr` を直に呼ぶのは、必要なのが
//! raw mode だけだから。Windows 非対応になるが、配布は macOS と Linux だけ。

use std::io::Read;
use std::os::unix::io::AsRawFd;
use std::time::{Duration, Instant};

use crate::proto::constants::button;

/// キーが来なくなってから、押されたままとみなす時間。
///
/// **OS の「最初のリピートまでの遅れ」より長くする必要がある。** macOS の
/// 既定は約 375ms、Linux も 250-500ms 程度で、その間はキーを押していても
/// 何も届かない。短くすると、押しっぱなしにしているのに一度離れてから
/// また押される形になる。
///
/// 一方で長くすると、離してから実際に離れるまでが遅くなる。450ms は
/// 「最初のリピートまで待てる」と「離れるのが遅すぎない」の折り合い。
/// 端末やゲームに合わせて `--sticky` で変えられる
pub const DEFAULT_STICKY: Duration = Duration::from_millis(450);

/// 端末を raw mode にして、抜けるときに必ず戻す。
///
/// 戻し忘れると、コマンドが終わった後の端末がエコーもしない壊れた状態で
/// 残る。`Drop` に載せておけば panic でも戻る
pub struct RawMode {
    fd: i32,
    saved: libc::termios,
}

impl RawMode {
    /// 標準入力を raw mode にする
    pub fn enter() -> std::io::Result<Self> {
        let stdin = std::io::stdin();
        let fd = stdin.as_raw_fd();

        // SAFETY: fd は開いている標準入力で、termios は書き込み先として渡す
        let mut saved: libc::termios = unsafe { std::mem::zeroed() };
        let got = unsafe { libc::tcgetattr(fd, &mut saved) };
        let is_a_terminal = got == 0;
        if !is_a_terminal {
            return Err(std::io::Error::last_os_error());
        }

        let mut raw = saved;
        // 行単位のバッファとエコーを切る。1 キーずつ、画面に出さずに読む。
        //
        // **ISIG も切る。** 残しておくと Ctrl-C が SIGINT になってプロセスが
        // その場で終わり、`Drop` での端末の復帰も、パッドを離す送信も走らない。
        // 切っておけば 0x03 が普通のバイトとして読めて、後始末をしてから抜けられる
        raw.c_lflag &= !(libc::ICANON | libc::ECHO | libc::ISIG);
        // 待たずに返す。周期はこちらで測るので、read に待たせない
        // (VTIME=1 にすると入力が無いとき 0.1 秒ブロックし、指定したレートで
        // 送れなくなる)
        raw.c_cc[libc::VMIN] = 0;
        raw.c_cc[libc::VTIME] = 0;

        // SAFETY: raw は tcgetattr で得た値を書き換えたもの
        let set = unsafe { libc::tcsetattr(fd, libc::TCSANOW, &raw) };
        let is_set = set == 0;
        if !is_set {
            return Err(std::io::Error::last_os_error());
        }

        Ok(Self { fd, saved })
    }
}

impl Drop for RawMode {
    fn drop(&mut self) {
        // SAFETY: saved は enter() で取っておいた元の設定
        unsafe {
            libc::tcsetattr(self.fd, libc::TCSANOW, &self.saved);
        }
    }
}

/// キーの割り当て
#[derive(Debug, Clone, Copy)]
pub struct Layout {
    /// A / B だけを使う。PORT.C の Dual Button Unit を網越しに再現する形
    pub two_button: bool,
}

/// 押されたキーからボタンを引く。
///
/// 矢印キーはエスケープシーケンス (`ESC [ A` など) で来るので、`Decoder` が
/// 組み立ててからここに渡す
pub fn button_for(key: Key, layout: Layout) -> Option<u8> {
    let bit = match key {
        Key::Char('z') | Key::Char('Z') => button::B,
        Key::Char('x') | Key::Char('X') => button::A,
        // 二ボタンモードでは十字も Start も受けない (Dual Button Unit と同じ)
        _ if layout.two_button => return None,
        Key::Char('\r') | Key::Char('\n') => button::START,
        Key::Char(' ') => button::SELECT,
        Key::Up => button::UP,
        Key::Down => button::DOWN,
        Key::Left => button::LEFT,
        Key::Right => button::RIGHT,
        _ => return None,
    };
    Some(bit)
}

/// 読み取ったキー
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Key {
    Char(char),
    Up,
    Down,
    Left,
    Right,
    /// Ctrl-C。抜ける合図
    Interrupt,
}

/// バイト列からキーを組み立てる。矢印キーは 3 バイトのエスケープシーケンス
#[derive(Default)]
pub struct Decoder {
    pending: Vec<u8>,
}

impl Decoder {
    /// 届いたバイトを流し込んで、読み取れたキーを返す
    pub fn feed(&mut self, bytes: &[u8]) -> Vec<Key> {
        self.pending.extend_from_slice(bytes);
        let mut keys = Vec::new();

        while !self.pending.is_empty() {
            let first = self.pending[0];

            let is_interrupt = first == 0x03;
            if is_interrupt {
                self.pending.remove(0);
                keys.push(Key::Interrupt);
                continue;
            }

            let is_escape = first == 0x1B;
            if !is_escape {
                self.pending.remove(0);
                keys.push(Key::Char(first as char));
                continue;
            }

            // 矢印は `ESC [ A` (CSI) と `ESC O A` (SS3) の 2 通りで来る。
            // 後者は端末が application cursor mode のときの形で、`less` や
            // 一部の端末設定で使われる
            let has_enough = self.pending.len() >= 3;
            if !has_enough {
                break;
            }
            let introducer = self.pending[1];
            let is_an_arrow_sequence = introducer == b'[' || introducer == b'O';
            if !is_an_arrow_sequence {
                // 知らないシーケンスは ESC だけ捨てて、続きは普通に読む
                self.pending.remove(0);
                continue;
            }

            let final_byte = self.pending[2];
            let key = match final_byte {
                b'A' => Some(Key::Up),
                b'B' => Some(Key::Down),
                b'C' => Some(Key::Right),
                b'D' => Some(Key::Left),
                _ => None,
            };
            if let Some(key) = key {
                self.pending.drain(..3);
                keys.push(key);
                continue;
            }

            // 知らない CSI は終端 (0x40..=0x7E) まで読み飛ばす。3 バイトだけ
            // 捨てると、残りが普通の文字として押されたことになる
            let is_csi = introducer == b'[';
            if !is_csi {
                self.pending.drain(..3);
                continue;
            }
            let end = self.pending[2..]
                .iter()
                .position(|b| (0x40..=0x7E).contains(b));
            let Some(end) = end else {
                // 終端がまだ来ていない。次のバイトを待つ
                break;
            };
            self.pending.drain(..3 + end);
        }
        keys
    }
}

/// 押されているボタンを、キーが来なくなってから `sticky` のあいだ保つ。
///
/// TTY はキーを離したことを教えてくれないので、こうするしかない
pub struct Held {
    sticky: Duration,
    /// (ボタン, 最後に見た時刻)
    pressed: Vec<(u8, Instant)>,
}

impl Held {
    pub fn new(sticky: Duration) -> Self {
        Self {
            sticky,
            pressed: Vec::new(),
        }
    }

    /// キーを見たことを記録する
    pub fn saw(&mut self, bit: u8, now: Instant) {
        if let Some(entry) = self.pressed.iter_mut().find(|(b, _)| *b == bit) {
            entry.1 = now;
            return;
        }
        self.pressed.push((bit, now));
    }

    /// 今押されているとみなすボタン
    pub fn buttons(&mut self, now: Instant) -> u8 {
        self.pressed
            .retain(|(_, last)| now.duration_since(*last) < self.sticky);
        self.pressed.iter().fold(0, |bits, (b, _)| bits | b)
    }
}

/// 標準入力から読めるだけ読む。raw mode の VTIME でブロックしすぎない
pub fn read_available(buffer: &mut [u8]) -> std::io::Result<usize> {
    let mut stdin = std::io::stdin();
    match stdin.read(buffer) {
        Ok(n) => Ok(n),
        Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => Ok(0),
        Err(e) => Err(e),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn full() -> Layout {
        Layout { two_button: false }
    }

    #[test]
    fn z_and_x_are_the_two_buttons() {
        assert_eq!(button_for(Key::Char('z'), full()), Some(button::B));
        assert_eq!(button_for(Key::Char('x'), full()), Some(button::A));
        // 大文字でも同じ
        assert_eq!(button_for(Key::Char('Z'), full()), Some(button::B));
    }

    #[test]
    fn the_arrows_are_the_dpad() {
        assert_eq!(button_for(Key::Up, full()), Some(button::UP));
        assert_eq!(button_for(Key::Down, full()), Some(button::DOWN));
        assert_eq!(button_for(Key::Left, full()), Some(button::LEFT));
        assert_eq!(button_for(Key::Right, full()), Some(button::RIGHT));
    }

    #[test]
    fn enter_and_space_are_start_and_select() {
        assert_eq!(button_for(Key::Char('\r'), full()), Some(button::START));
        assert_eq!(button_for(Key::Char(' '), full()), Some(button::SELECT));
    }

    /// 二ボタンモードは A と B だけ。PORT.C の Dual Button Unit と同じ
    #[test]
    fn two_button_mode_ignores_everything_but_a_and_b() {
        let two = Layout { two_button: true };
        assert_eq!(button_for(Key::Char('z'), two), Some(button::B));
        assert_eq!(button_for(Key::Char('x'), two), Some(button::A));
        assert_eq!(button_for(Key::Up, two), None);
        assert_eq!(button_for(Key::Char('\r'), two), None);
    }

    #[test]
    fn unknown_keys_press_nothing() {
        assert_eq!(button_for(Key::Char('q'), full()), None);
    }

    #[test]
    fn plain_characters_are_decoded_one_at_a_time() {
        let mut decoder = Decoder::default();
        assert_eq!(decoder.feed(b"zx"), vec![Key::Char('z'), Key::Char('x')]);
    }

    /// 矢印キーは `ESC [ A` の 3 バイト。途中まで届いた状態で誤読しない
    #[test]
    fn arrows_are_assembled_from_their_escape_sequence() {
        let mut decoder = Decoder::default();
        assert_eq!(decoder.feed(b"\x1b[A"), vec![Key::Up]);
        assert_eq!(decoder.feed(b"\x1b[B"), vec![Key::Down]);
        assert_eq!(decoder.feed(b"\x1b[C"), vec![Key::Right]);
        assert_eq!(decoder.feed(b"\x1b[D"), vec![Key::Left]);
    }

    #[test]
    fn a_partial_escape_sequence_waits_for_the_rest() {
        let mut decoder = Decoder::default();
        // ESC だけでは何も出さない
        assert!(decoder.feed(b"\x1b").is_empty());
        assert!(decoder.feed(b"[").is_empty());
        assert_eq!(decoder.feed(b"A"), vec![Key::Up]);
    }

    #[test]
    fn ctrl_c_is_recognised_as_an_interrupt() {
        let mut decoder = Decoder::default();
        assert_eq!(decoder.feed(&[0x03]), vec![Key::Interrupt]);
    }

    /// 端末が application cursor mode のときは `ESC O A` で来る
    #[test]
    fn ss3_arrows_are_understood_too() {
        let mut decoder = Decoder::default();
        assert_eq!(decoder.feed(b"\x1bOA"), vec![Key::Up]);
        assert_eq!(decoder.feed(b"\x1bOB"), vec![Key::Down]);
        assert_eq!(decoder.feed(b"\x1bOC"), vec![Key::Right]);
        assert_eq!(decoder.feed(b"\x1bOD"), vec![Key::Left]);
    }

    /// 知らない CSI は終端まで読み飛ばす。3 バイトだけ捨てると、残りが
    /// 普通の文字として押されたことになる
    #[test]
    fn an_unknown_escape_sequence_is_skipped_whole() {
        let mut decoder = Decoder::default();
        // F5 (`ESC [ 1 5 ~`) の後ろに z が続く
        assert_eq!(decoder.feed(b"\x1b[15~z"), vec![Key::Char('z')]);
        // マウスレポートのような長いものも
        assert_eq!(decoder.feed(b"\x1b[<0;10;20Mx"), vec![Key::Char('x')]);
    }

    #[test]
    fn an_incomplete_unknown_sequence_waits_for_its_end() {
        let mut decoder = Decoder::default();
        assert!(decoder.feed(b"\x1b[15").is_empty());
        assert_eq!(decoder.feed(b"~z"), vec![Key::Char('z')]);
    }

    /// ESC が途中まで届いた状態で Ctrl-C が来ても、抜けられなければ困る
    #[test]
    fn ctrl_c_after_a_partial_escape_is_still_seen() {
        let mut decoder = Decoder::default();
        assert!(decoder.feed(b"\x1b").is_empty());
        // ESC + 0x03 は矢印ではないので、ESC を捨てて 0x03 を読む
        let keys = decoder.feed(&[0x03, 0x03]);
        assert!(
            keys.contains(&Key::Interrupt),
            "Ctrl-C must survive a partial escape: {keys:?}"
        );
    }

    /// **TTY はキーを離したことを教えてくれない。** 最後に見てから
    /// `sticky` のあいだは押されたままとみなす
    #[test]
    fn a_key_stays_held_until_it_goes_quiet() {
        let sticky = Duration::from_millis(100);
        let mut held = Held::new(sticky);
        let start = Instant::now();

        held.saw(button::A, start);
        assert_eq!(held.buttons(start), button::A);
        // まだ黙っていない
        assert_eq!(held.buttons(start + Duration::from_millis(50)), button::A);
        // 黙ったので離れる
        assert_eq!(held.buttons(start + Duration::from_millis(150)), 0);
    }

    #[test]
    fn seeing_a_key_again_extends_the_hold() {
        let mut held = Held::new(Duration::from_millis(100));
        let start = Instant::now();

        held.saw(button::A, start);
        held.saw(button::A, start + Duration::from_millis(80));
        // 2 回目から数えるので、まだ押されている
        assert_eq!(held.buttons(start + Duration::from_millis(150)), button::A);
    }

    #[test]
    fn several_keys_can_be_held_at_once() {
        let mut held = Held::new(Duration::from_millis(100));
        let now = Instant::now();

        held.saw(button::A, now);
        held.saw(button::RIGHT, now);
        assert_eq!(held.buttons(now), button::A | button::RIGHT);
    }

    /// 既定は OS の「最初のリピートまでの遅れ」(macOS ≒ 375ms、Linux は
    /// 250-500ms) より長い必要がある。短いと、押しっぱなしにしているのに
    /// 一度離れてからまた押される形になる
    #[test]
    fn the_default_sticky_window_outlasts_the_initial_repeat_delay() {
        assert!(
            DEFAULT_STICKY > Duration::from_millis(375),
            "a held key would drop out before the OS starts repeating"
        );
        // 長すぎると、離してから実際に離れるまでが遅い
        assert!(DEFAULT_STICKY <= Duration::from_millis(600));
    }
}
