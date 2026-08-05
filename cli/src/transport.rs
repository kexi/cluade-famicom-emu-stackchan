//! UDP の送受信と、再送・タイムアウトの戦略。
//!
//! 戦略は `tools/serve_web.py` から引き継いだもので、実運用で検証されている。
//! 主なものは 3 つ:
//!
//! 1. **ソケットは用途で分ける。** 応答を待たないもの (pins / reset / volume) は
//!    共有ソケット、応答を待つもの (debug / rom / sd) はリクエストごとに新しい
//!    ソケットを開く。同じソケットを使い回すと、捨てたリクエストへの遅れた ACK を
//!    次のリクエストの答えとして拾ってしまう。
//!
//! 2. **タイムアウトは deadline 基準。** 1 回の待ちごとに「締切までの残り」を
//!    再計算する。固定のタイムアウトを毎回設定し直すと、無関係なデータグラムが
//!    届くたびに待ち時間が延びる。
//!
//! 3. **冪等でない操作は再送しない。** firmware は per-seq の結果キャッシュを
//!    持たないので、成功した DELETE をもう一度送ると `NotFound` が返る。
//!    「送ったが結果が判らない」を「失敗した」と報告してはいけない。

use std::io;
use std::net::{SocketAddr, ToSocketAddrs, UdpSocket};
use std::time::{Duration, Instant};

use crate::exit::ExitCode;
use crate::proto::constants::DEFAULT_PORT;

/// 応答待ちの結果。
///
/// `Timeout` と `Unknown` を分けているのが要点。冪等な操作が答えを得られな
/// かったのは「届かなかった」だが、冪等でない操作でそうなったのは
/// 「実行されたかもしれない」で、呼び出し側の取るべき行動が違う
#[derive(Debug)]
pub enum TransportError {
    /// ホスト名が解決できない、ソケットが開けない等
    Io(io::Error),
    /// 締切までに答えが来なかった (冪等な操作なので再送は済んでいる)
    Timeout,
    /// 送ったが答えが来ず、実行されたかどうか判らない。
    /// 冪等でない操作 (DELETE / RENAME) でのみ起こる
    Unknown,
    /// デバイスが理由を添えて断った。届いているので Timeout ではない
    Refused(String),
}

impl TransportError {
    pub fn exit_code(&self) -> ExitCode {
        match self {
            // デバイスが断ったのは「失敗」。答えは返っているので Timeout ではない
            Self::Io(_) | Self::Refused(_) => ExitCode::Failure,
            Self::Timeout => ExitCode::Timeout,
            Self::Unknown => ExitCode::Unknown,
        }
    }
}

impl std::fmt::Display for TransportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(e) => write!(f, "{e}"),
            Self::Timeout => f.write_str("the device did not answer"),
            Self::Unknown => f.write_str(
                "the device did not answer; the request may or may not have taken effect",
            ),
            Self::Refused(message) => f.write_str(message),
        }
    }
}

impl From<io::Error> for TransportError {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

pub type Result<T> = std::result::Result<T, TransportError>;

/// 再送の可否を明示する。
///
/// これ自体はパケットと独立しているので、DELETE に `Idempotent` を渡すことは
/// まだできてしまう。操作から方針を導く変換を 1 か所に閉じるのは、SD の各 op を
/// 実装するときの仕事 (`SdOp::is_idempotent` を使う)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Retry {
    /// 同じリクエストを送り直してよい (LIST / LOAD / debug など)
    Idempotent { attempts: u8 },
    /// 一度だけ送る。答えが来なければ結果は不明 (DELETE / RENAME)
    Once,
}

impl Retry {
    fn attempts(self) -> u8 {
        match self {
            Self::Idempotent { attempts } => attempts.max(1),
            Self::Once => 1,
        }
    }

    /// 答えが得られなかったときのエラー。冪等なら「届かなかった」、
    /// そうでなければ「判らない」
    fn no_answer(self) -> TransportError {
        match self {
            Self::Idempotent { .. } => TransportError::Timeout,
            Self::Once => TransportError::Unknown,
        }
    }
}

/// 解決したアドレスを持ち回してよい時間。
///
/// **毎回引き直す実装から変えた理由は実測。** macOS の `getaddrinfo` は
/// `.local` を 1 回引くのに約 5 秒かかる (`192.168.x.x` は 0ms)。60Hz で
/// 送るコマンドは 1 秒ぶんの入力に 5 分かかり、ROM 転送は 48KB に 3 分かかって
/// いた — どちらも実機で踏んだ。
///
/// キャッシュせずに済ませられないのは、`send` が呼ばれる頻度が op によって
/// 桁違いだから。`ctrl reset` の 1 発と `input procon` の毎秒 120 発を
/// 同じ経路が扱う。
///
/// 5 秒にしたのは、これが「解決 1 回ぶんの時間」だから。これより短くすると
/// 解決が解決を追いかけ、長くすると DHCP でアドレスが変わったときに古い
/// 宛先へ送り続ける窓が広がる。DHCP のリース更新は分単位なので、5 秒の
/// 取りこぼしはユーザーが「反応しない」と感じる前に自然に直る
const ADDRESS_TTL: Duration = Duration::from_secs(5);

/// デバイスとの通信。
///
/// ホストは文字列のまま持ち、解決結果は `ADDRESS_TTL` だけ使い回す。
/// `SocketAddr` を永続的にキャッシュすると、DHCP でアドレスが変わったときに
/// 黙って古い宛先へ送り続ける (`serve_web.py` は `sendto` に文字列を渡して
/// 毎回 OS に解決させており、その意図を期限付きで保つ)
pub struct Device {
    host: String,
    port: u16,
    seq: u16,
    /// 応答を待たない送信専用。開きっぱなしでよい
    sender: UdpSocket,
    verbose: u8,
    /// `--timeout` の指定。op ごとの既定を置き換える
    timeout: Option<Duration>,
    /// 直近の解決結果と、それを引いた時刻
    resolved: Option<(SocketAddr, Instant)>,
}

impl Device {
    pub fn new(host: &str, port: u16, verbose: u8) -> Result<Self> {
        // 0 番ポートで bind すると OS が空きを割り当てる
        let sender = UdpSocket::bind(("0.0.0.0", 0))?;
        Ok(Self {
            host: host.to_string(),
            port,
            seq: 0,
            sender,
            verbose,
            timeout: None,
            resolved: None,
        })
    }

    pub fn with_default_port(host: &str, verbose: u8) -> Result<Self> {
        Self::new(host, DEFAULT_PORT, verbose)
    }

    /// 1 回の応答待ちの締切を上書きする。
    ///
    /// 置き換えるのは「1 回の応答をどれだけ待つか」だけで、再送間隔や
    /// BUSY を待つ上限には触らない。1 つの数字で全部を殴らせないため —
    /// ROM の 0.3s と SD の BUSY 12s は意味が違い、前者を伸ばしたいときに
    /// 後者まで伸びると転送が固まったまま返らなくなる
    pub fn set_timeout(&mut self, timeout: Option<Duration>) {
        self.timeout = timeout;
    }

    /// op ごとの既定に `--timeout` を被せる
    pub fn timeout_or(&self, default: Duration) -> Duration {
        self.timeout.unwrap_or(default)
    }

    pub fn host(&self) -> &str {
        &self.host
    }

    /// 次の seq を取る。16bit で巻き戻る
    pub fn next_seq(&mut self) -> u16 {
        self.seq = self.seq.wrapping_add(1);
        self.seq
    }

    fn target(&self) -> (&str, u16) {
        (self.host.as_str(), self.port)
    }

    /// 送り先のアドレス。`ADDRESS_TTL` を過ぎていれば引き直す。
    ///
    /// 解決に失敗したときに古いアドレスへ倒さないのは、失敗の理由が
    /// 「機体が居なくなった」のときに、居ない宛先へ送り続けて成功したように
    /// 見えるため。エラーを返して呼び出し側に判断させる
    fn address(&mut self) -> Result<SocketAddr> {
        let is_fresh = self
            .resolved
            .is_some_and(|(_, at)| at.elapsed() < ADDRESS_TTL);
        if is_fresh {
            return Ok(self.resolved.expect("just checked").0);
        }

        let mut addrs = self.target().to_socket_addrs()?;
        let Some(addr) = addrs.next() else {
            return Err(TransportError::Io(io::Error::new(
                io::ErrorKind::NotFound,
                format!("could not resolve '{}'", self.host),
            )));
        };
        self.resolved = Some((addr, Instant::now()));
        Ok(addr)
    }

    fn trace(&self, message: &str) {
        if self.verbose > 0 {
            eprintln!("stackchan: {message}");
        }
    }

    fn trace_bytes(&self, label: &str, bytes: &[u8]) {
        if self.verbose > 1 {
            let hex: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
            eprintln!("stackchan: {label} {} bytes: {hex}", bytes.len());
        }
    }

    /// 応答を待たずに送る。pins / reset / volume 用。
    ///
    /// 届いたかどうかは判らない。UDP なので確認する術がなく、これらの操作は
    /// firmware 側も ACK を返さない
    pub fn send(&mut self, packet: &[u8]) -> Result<()> {
        self.trace_bytes("send", packet);
        let to = self.address()?;
        self.sender.send_to(packet, to)?;
        Ok(())
    }

    /// 応答を待つ。専用のソケットを開いて、締切まで読み続ける。
    ///
    /// `accept` は届いたデータグラムを見て、
    /// - `Some(値)` … これが答え。返す
    /// - `None` … 無関係なデータグラム。**締切を消費せずに**読み続ける
    ///
    /// を返す。無関係なものでウィンドウを消費しないのが要点で、そうしないと
    /// 別の送信元からの 1 発で本来の ACK を取り逃す
    pub fn request<T>(
        &mut self,
        packet: &[u8],
        timeout: Duration,
        retry: Retry,
        mut accept: impl FnMut(&[u8]) -> Option<T>,
    ) -> Result<T> {
        // リクエストごとに新しいソケットを開く。共有すると、前のリクエストへの
        // 遅れた応答を今の答えとして拾ってしまう
        let socket = UdpSocket::bind(("0.0.0.0", 0))?;
        let mut buffer = vec![0u8; 2048];

        for attempt in 0..retry.attempts() {
            if attempt > 0 {
                self.trace(&format!(
                    "no answer, retrying ({}/{})",
                    attempt + 1,
                    retry.attempts()
                ));
            }
            self.trace_bytes("send", packet);
            let to = self.address()?;
            socket.send_to(packet, to)?;

            let deadline = Instant::now() + timeout;
            loop {
                let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
                    break;
                };
                let is_expired = remaining.is_zero();
                if is_expired {
                    break;
                }
                socket.set_read_timeout(Some(remaining))?;

                let received = match socket.recv(&mut buffer) {
                    Ok(n) => n,
                    Err(e) if is_timeout(&e) => break,
                    Err(e) => return Err(e.into()),
                };

                self.trace_bytes("recv", &buffer[..received]);
                // 無関係なデータグラムは締切を消費せずに読み飛ばす
                if let Some(value) = accept(&buffer[..received]) {
                    return Ok(value);
                }
            }
        }

        Err(retry.no_answer())
    }

    /// 分割された応答を集める。debug の複数パートと SD の LIST 用。
    ///
    /// **`new_collector` は試行ごとに呼ばれる。** 集めかけの状態を試行をまたいで
    /// 持ち越すと、試行 1 の part 0 と試行 2 の part 1 を混ぜてしまう。SD の
    /// LIST は再送の合間にカードの中身が変わりうるし、debug は要求ごとに別の
    /// フレームのスナップショットを作るので、それは別々の時点のデータを 1 つの
    /// 答えとして返すことになる。
    ///
    /// 部分的な結果を返してはいけないのも同じ理由で、1 パート落ちたら全体を
    /// 捨てて再送させるほうが、少なく見える一覧を返すよりよい
    pub fn request_parts<T, C>(
        &mut self,
        packet: &[u8],
        timeout: Duration,
        retry: Retry,
        mut new_collector: impl FnMut() -> C,
    ) -> Result<T>
    where
        C: FnMut(&[u8]) -> PartOutcome<T>,
    {
        let mut buffer = vec![0u8; 2048];
        // 諦めた試行のソケットは、この呼び出しが終わるまで閉じずに持っておく。
        // UDP には TIME_WAIT が無いので、閉じた直後の bind に OS が同じ
        // エフェメラルポートを割り当てうる。そうなると遅れたパートが新しい
        // ソケットに届き、下で防ごうとしている混在がそのまま起きる。
        // 開いたままにしておけばポートは占有され、確実に別の番号になる
        let mut retired = Vec::new();

        for attempt in 0..retry.attempts() {
            if attempt > 0 {
                self.trace(&format!(
                    "incomplete reply, retrying ({}/{})",
                    attempt + 1,
                    retry.attempts()
                ));
            }
            // 試行ごとに集めかけを捨てる。さらにソケットも開き直す:
            // 前の試行のパートがネットワーク上で遅れていると、同じソケットを
            // 使い回している限り新しい collector に届いてしまう。リクエストは
            // seq も含めて同一なので、collector 側では区別できない
            let socket = UdpSocket::bind(("0.0.0.0", 0))?;
            let mut collect = new_collector();

            self.trace_bytes("send", packet);
            let to = self.address()?;
            socket.send_to(packet, to)?;

            let deadline = Instant::now() + timeout;
            let mut refused = None;
            loop {
                let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
                    break;
                };
                let is_expired = remaining.is_zero();
                if is_expired {
                    break;
                }
                socket.set_read_timeout(Some(remaining))?;

                let received = match socket.recv(&mut buffer) {
                    Ok(n) => n,
                    Err(e) if is_timeout(&e) => break,
                    Err(e) => return Err(e.into()),
                };

                self.trace_bytes("recv", &buffer[..received]);
                match collect(&buffer[..received]) {
                    PartOutcome::Done(value) => return Ok(value),
                    PartOutcome::NeedMore | PartOutcome::Ignore => continue,
                    // デバイスが明示的に断った。再送しても同じ答えなので、
                    // タイムアウトではなくその理由を返す
                    PartOutcome::Refused(message) => {
                        refused = Some(message);
                        break;
                    }
                }
            }
            if let Some(message) = refused {
                return Err(TransportError::Refused(message));
            }
            // 諦めた試行のポートを次の bind に取られないよう、閉じずに残す
            retired.push(socket);
        }

        Err(retry.no_answer())
    }

    /// 何度もやり取りする一連の手続き用にソケットを開く。
    ///
    /// ROM 転送のように BEGIN から保存イベントまでを 1 本で通す必要がある
    /// ときに使う。firmware は BEGIN が来たポートに返すので、途中で開き直すと
    /// 違う場所で待つことになる
    pub fn open_session(&self) -> Result<Session> {
        let socket = UdpSocket::bind(("0.0.0.0", 0))?;
        Ok(Session {
            socket,
            host: self.host.clone(),
            port: self.port,
            buffer: vec![0u8; 2048],
            verbose: self.verbose,
            timeout: self.timeout,
            // 解決済みなら引き継ぐ。転送のたびに 1 回ぶん引き直すのを避ける
            resolved: self.resolved,
        })
    }

    /// ホスト名が解決できるかを先に確かめる。
    ///
    /// 送信だけなら `send_to` が失敗した時点で判るが、`.local` が引けない
    /// 環境ではエラーが「送れなかった」ではなく「答えが来ない」に見えて
    /// しまうことがあるので、必要な場面では明示的に呼ぶ。
    ///
    /// 結果は捨てずにキャッシュへ入れる — 捨てると直後の `send` が
    /// もう一度引き直し、`.local` では 5 秒を 2 回払うことになる
    pub fn resolve(&mut self) -> Result<()> {
        self.address()?;
        Ok(())
    }
}

/// 同じソケットで複数回やり取りする一連の手続き。
///
/// `Device::request` が 1 往復ごとにソケットを開き直すのに対し、こちらは
/// 開いたまま使う。ROM 転送では BEGIN / DATA / END / 保存イベントが同じ
/// ポートに返る必要があるため
pub struct Session {
    socket: UdpSocket,
    host: String,
    port: u16,
    buffer: Vec<u8>,
    verbose: u8,
    /// `--timeout` の指定。開いた `Device` から引き継ぐ
    timeout: Option<Duration>,
    /// 直近の解決結果と、それを引いた時刻 (`Device` と同じ扱い)
    resolved: Option<(SocketAddr, Instant)>,
}

impl Session {
    /// op ごとの既定に `--timeout` を被せる
    pub fn timeout_or(&self, default: Duration) -> Duration {
        self.timeout.unwrap_or(default)
    }

    /// 送り先のアドレス。`Device::address` と同じ理由で期限付きに持ち回す。
    ///
    /// ROM 転送はチャンクごとにここを通るので、毎回引き直すと `.local` では
    /// 1 チャンクあたり 5 秒が上乗せされる (48KB で実測 194 秒だった)
    fn address(&mut self) -> Result<SocketAddr> {
        let is_fresh = self
            .resolved
            .is_some_and(|(_, at)| at.elapsed() < ADDRESS_TTL);
        if is_fresh {
            return Ok(self.resolved.expect("just checked").0);
        }

        let mut addrs = (self.host.as_str(), self.port).to_socket_addrs()?;
        let Some(addr) = addrs.next() else {
            return Err(TransportError::Io(io::Error::new(
                io::ErrorKind::NotFound,
                format!("could not resolve '{}'", self.host),
            )));
        };
        self.resolved = Some((addr, Instant::now()));
        Ok(addr)
    }

    /// 1 往復。`packet` が空なら送らずに待つだけ (保存イベントのように、
    /// こちらから促さずに届くものを受けるとき)。
    ///
    /// 締切までに `accept` が `Some` を返さなければ `Ok(None)`。無関係な
    /// データグラムでは締切を消費しない
    pub fn exchange<T>(
        &mut self,
        packet: &[u8],
        timeout: Duration,
        mut accept: impl FnMut(&[u8]) -> Option<T>,
    ) -> Result<Option<T>> {
        let should_send = !packet.is_empty();
        if should_send {
            if self.verbose > 1 {
                let hex: String = packet.iter().map(|b| format!("{b:02x}")).collect();
                eprintln!("stackchan: send {} bytes: {hex}", packet.len());
            }
            let to = self.address()?;
            self.socket.send_to(packet, to)?;
        }

        let deadline = Instant::now() + timeout;
        loop {
            let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
                return Ok(None);
            };
            let is_expired = remaining.is_zero();
            if is_expired {
                return Ok(None);
            }
            self.socket.set_read_timeout(Some(remaining))?;

            let received = match self.socket.recv(&mut self.buffer) {
                Ok(n) => n,
                Err(e) if is_timeout(&e) => return Ok(None),
                Err(e) => return Err(e.into()),
            };

            if self.verbose > 1 {
                let hex: String = self.buffer[..received]
                    .iter()
                    .map(|b| format!("{b:02x}"))
                    .collect();
                eprintln!("stackchan: recv {received} bytes: {hex}");
            }
            // 無関係なデータグラムは締切を消費せずに読み飛ばす
            if let Some(value) = accept(&self.buffer[..received]) {
                return Ok(Some(value));
            }
        }
    }
}

/// 分割応答を集めるときの、1 データグラムに対する判断
pub enum PartOutcome<T> {
    /// 揃った
    Done(T),
    /// この応答の一部。まだ足りない
    NeedMore,
    /// 無関係なデータグラム
    Ignore,
    /// デバイスが明示的に断った (非 Ok のステータスが返った等)。
    ///
    /// 再送しても同じ答えが返るので打ち切るが、これは「届かなかった」では
    /// **ない**。タイムアウトに丸めると、理由が判っているのに「答えが来ない」と
    /// 報告することになる
    Refused(String),
}

fn is_timeout(e: &io::Error) -> bool {
    matches!(
        e.kind(),
        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn idempotent_operations_report_a_timeout() {
        let retry = Retry::Idempotent { attempts: 3 };
        assert_eq!(retry.attempts(), 3);
        assert!(matches!(retry.no_answer(), TransportError::Timeout));
        assert_eq!(retry.no_answer().exit_code(), ExitCode::Timeout);
    }

    // 冪等でない操作が答えを得られなかったら「失敗」ではなく「判らない」。
    // 呼び出し側が「消えていない」と誤解すると、別の手を打ってしまう
    #[test]
    fn non_idempotent_operations_report_an_unknown_outcome() {
        let retry = Retry::Once;
        assert_eq!(retry.attempts(), 1, "a non-idempotent request is sent once");
        assert!(matches!(retry.no_answer(), TransportError::Unknown));
        assert_eq!(retry.no_answer().exit_code(), ExitCode::Unknown);
    }

    // attempts=0 を渡されても 1 回は送る (黙って何もしないより送る)
    #[test]
    fn at_least_one_attempt_is_always_made() {
        assert_eq!(Retry::Idempotent { attempts: 0 }.attempts(), 1);
    }

    #[test]
    fn errors_map_to_the_documented_exit_codes() {
        let io_error = TransportError::Io(io::Error::other("boom"));
        assert_eq!(io_error.exit_code(), ExitCode::Failure);
        assert_eq!(TransportError::Timeout.exit_code(), ExitCode::Timeout);
        assert_eq!(TransportError::Unknown.exit_code(), ExitCode::Unknown);
    }

    // 「判らない」は「失敗した」と読めてはいけない
    #[test]
    fn the_unknown_message_says_it_may_have_taken_effect() {
        let text = TransportError::Unknown.to_string();
        assert!(text.contains("may or may not"), "got: {text}");
    }

    #[test]
    fn seq_wraps_at_sixteen_bits() {
        let mut device = Device::new("127.0.0.1", 5555, 0).unwrap();
        device.seq = u16::MAX;
        assert_eq!(device.next_seq(), 0);
    }

    // ホストは文字列のまま持つ。SocketAddr にキャッシュすると DHCP の
    // アドレス変更を追えない
    #[test]
    fn the_host_is_kept_as_written() {
        let device = Device::new("stackchan-a1b2c3.local", 5555, 0).unwrap();
        assert_eq!(device.host(), "stackchan-a1b2c3.local");
    }

    #[test]
    fn resolve_reports_a_name_it_cannot_look_up() {
        let mut device = Device::new("no-such-host.invalid", 5555, 0).unwrap();
        assert!(device.resolve().is_err());
    }

    #[test]
    fn resolve_accepts_an_address_literal() {
        let mut device = Device::new("127.0.0.1", 5555, 0).unwrap();
        assert!(device.resolve().is_ok());
    }

    /// 解決結果を持ち回さないと `.local` では 1 回 5 秒が毎送信に乗る
    /// (実測: macOS の getaddrinfo)。ROM 転送 48KB が 194 秒かかっていた
    #[test]
    fn the_address_is_reused_within_its_ttl() {
        let mut device = Device::new("127.0.0.1", 5555, 0).unwrap();
        let first = device.address().expect("127.0.0.1 resolves");
        let at = device.resolved.expect("resolving stores the address").1;

        let second = device.address().expect("still resolves");
        assert_eq!(first, second);
        assert_eq!(
            device.resolved.expect("still cached").1,
            at,
            "the address was looked up again inside its TTL"
        );
    }

    /// 期限が切れたら引き直す。切れないと DHCP でアドレスが変わったときに
    /// 古い宛先へ送り続ける
    #[test]
    fn the_address_is_looked_up_again_once_it_expires() {
        let mut device = Device::new("127.0.0.1", 5555, 0).unwrap();
        device.address().expect("127.0.0.1 resolves");

        // 期限切れを作る。実時間を待たずに、記録した時刻を過去へずらす
        let (addr, at) = device.resolved.expect("resolving stores the address");
        let expired = at
            .checked_sub(ADDRESS_TTL + Duration::from_secs(1))
            .expect("the test clock can go back");
        device.resolved = Some((addr, expired));

        device.address().expect("still resolves");
        assert_ne!(
            device.resolved.expect("still cached").1,
            expired,
            "an expired address was not looked up again"
        );
    }

    /// 解決できなくなったら、古いアドレスへ倒さずにエラーを返す。
    /// 倒すと「機体が居なくなった」ときに送り続けて成功に見える
    #[test]
    fn a_name_that_stops_resolving_is_an_error_not_a_stale_address() {
        let mut device = Device::new("no-such-host.invalid", 5555, 0).unwrap();
        assert!(device.address().is_err());
        assert!(device.resolved.is_none(), "a failure must not be cached");
    }
}
