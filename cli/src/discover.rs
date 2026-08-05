//! LAN 上の機体を mDNS で探す。
//!
//! firmware は `_nes._udp` を広告する。`--host` を知らないところから始める
//! 唯一の入口で、他のコマンドと違ってデバイスのアドレスを要らない。
//!
//! **見つからないことは失敗ではない。** LAN に 1 台も無いのと、探索そのものが
//! 動かないのは別のこと。前者は空の一覧を返す

use std::collections::HashMap;
use std::net::IpAddr;
use std::time::{Duration, Instant};

use flume::RecvTimeoutError;
use mdns_sd::{ServiceDaemon, ServiceEvent};

use crate::exit::ExitCode;

/// firmware が広告するサービス種別
pub const SERVICE: &str = "_nes._udp.local.";

/// 見つかった機体
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Found {
    /// `stackchan-a1b2c3.local` の形
    pub hostname: String,
    pub addresses: Vec<IpAddr>,
    pub port: u16,
}

impl Found {
    /// `--host` に渡せる文字列。
    ///
    /// mDNS 名を優先するのは、DHCP でアドレスが変わっても追随できるため。
    /// ただし `.local` の解決が塞がれている環境 (VPN や隔離された Wi-Fi) では
    /// 名前だけが引けないことがあるので、アドレスも出す
    pub fn best_host(&self) -> String {
        self.hostname.clone()
    }

    /// 実際に機体へ届きうるアドレスだけを返す。
    ///
    /// mDNS はそのホストが持つアドレスを全部載せてくるので、`--host` に渡しても
    /// 届かないものが混ざる。除くのは:
    ///
    /// - **ループバック** … 自分自身を指す
    /// - **IPv6 のリンクローカル (`fe80::/10`)** … スコープ (どのインタフェース
    ///   から出るか) が要るが、ここではそれを捨てているので使えない
    /// - **unspecified / マルチキャスト** … 宛先にならない
    ///
    /// IPv4 のリンクローカル (`169.254.0.0/16`) は**残す**。IPv6 と違って
    /// スコープを添える必要がなく、同じリンク上ならそのまま届く。DHCP が
    /// 無い環境ではこれしか無いこともある
    pub fn reachable_addresses(&self) -> Vec<IpAddr> {
        self.addresses
            .iter()
            .filter(|address| {
                let is_usable = match address {
                    IpAddr::V4(v4) => {
                        !v4.is_loopback() && !v4.is_unspecified() && !v4.is_multicast()
                    }
                    IpAddr::V6(v6) => {
                        // `is_unicast_link_local` は unstable なので自前で見る
                        let is_link_local = (v6.segments()[0] & 0xFFC0) == 0xFE80;
                        !v6.is_loopback()
                            && !v6.is_unspecified()
                            && !v6.is_multicast()
                            && !is_link_local
                    }
                };
                is_usable
            })
            .copied()
            .collect()
    }
}

#[derive(Debug)]
pub enum DiscoverError {
    /// mDNS のデーモンが起動できない (ソケットが開けない等)
    Unavailable(String),
}

impl DiscoverError {
    pub fn exit_code(&self) -> ExitCode {
        ExitCode::Failure
    }
}

impl std::fmt::Display for DiscoverError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Unavailable(message) => write!(f, "cannot browse the network: {message}"),
        }
    }
}

pub type Result<T> = std::result::Result<T, DiscoverError>;

/// `timeout` のあいだ探して、見つかったものを返す。
///
/// mDNS は「全部見つけた」と言える瞬間が無いので、時間で区切るしかない。
/// 早めに切ると遠い機体を取りこぼし、長くすると待たされる
pub fn browse(timeout: Duration) -> Result<Vec<Found>> {
    let daemon = ServiceDaemon::new().map_err(|e| DiscoverError::Unavailable(format!("{e}")))?;
    let receiver = daemon
        .browse(SERVICE)
        .map_err(|e| DiscoverError::Unavailable(format!("{e}")))?;

    // 同じ機体が何度も届くので、名前で束ねる
    let mut found: HashMap<String, Found> = HashMap::new();
    let deadline = Instant::now() + timeout;

    loop {
        let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
            break;
        };
        let is_expired = remaining.is_zero();
        if is_expired {
            break;
        }

        let event = match receiver.recv_timeout(remaining) {
            Ok(event) => event,
            // 期限が来ただけ。集まったものを返す
            Err(RecvTimeoutError::Timeout) => break,
            // デーモンが消えた = 探索そのものが動いていない。「見つからなかった」
            // と区別して失敗にする
            Err(RecvTimeoutError::Disconnected) => {
                let _ = daemon.shutdown();
                return Err(DiscoverError::Unavailable(
                    "the mDNS responder stopped".to_string(),
                ));
            }
        };

        match event {
            ServiceEvent::ServiceResolved(info) => {
                // `stackchan-a1b2c3.local.` の末尾の `.` を落とす。`--host` に
                // そのまま渡せる形にしておく
                let hostname = info.get_hostname().trim_end_matches('.').to_string();
                // アドレスはスコープ付き (link-local の場合にインタフェースを
                // 持つ) で返るので、素の IP に落とす
                let mut addresses: Vec<IpAddr> = info
                    .get_addresses()
                    .iter()
                    .map(|scoped| scoped.to_ip_addr())
                    .collect();
                addresses.sort();
                addresses.dedup();

                // 同じ機体は fullname で束ねる。resolved はそのときのキャッシュ
                // から組み立てられるので、追記ではなく置き換える
                found.insert(
                    info.get_fullname().to_string(),
                    Found {
                        hostname,
                        addresses,
                        port: info.get_port(),
                    },
                );
            }
            // 探索中に居なくなった機体は返さない。goodbye を無視すると、
            // もう応答しないアドレスを勧めることになる
            ServiceEvent::ServiceRemoved(_, fullname) => {
                found.remove(&fullname);
            }
            _ => continue,
        }
    }

    // 明示的に閉じる。落ちても探索の結果は変わらないので無視してよい
    let _ = daemon.shutdown();

    let mut devices: Vec<Found> = found.into_values().collect();
    devices.sort_by(|a, b| a.hostname.cmp(&b.hostname));
    Ok(devices)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_service_type_matches_what_the_firmware_advertises() {
        // firmware は MDNS.addService("_nes", "_udp", UDP_PORT) を呼ぶ
        assert_eq!(SERVICE, "_nes._udp.local.");
    }

    #[test]
    fn a_found_device_offers_a_host_that_can_be_passed_back() {
        let found = Found {
            hostname: "stackchan-a1b2c3.local".to_string(),
            addresses: vec!["192.168.1.42".parse().unwrap()],
            port: 5555,
        };
        assert_eq!(found.best_host(), "stackchan-a1b2c3.local");
    }

    // mDNS はそのホストの全アドレスを載せてくる。ループバックやリンクローカルを
    // 並べても `--host` には渡せないので、実際に届くものだけを出す
    #[test]
    fn only_addresses_that_can_reach_the_device_are_offered() {
        let found = Found {
            hostname: "stackchan-a1b2c3.local".to_string(),
            addresses: vec![
                "127.0.0.1".parse().unwrap(),
                "192.168.1.42".parse().unwrap(),
                "::1".parse().unwrap(),
                "fe80::1".parse().unwrap(),
                "169.254.1.1".parse().unwrap(),
                "2400:4051:bb61:9e00::1".parse().unwrap(),
            ],
            port: 5555,
        };

        // 並びは入力のまま (呼び出し側が先頭を選ぶので、順序は変えない)
        let usable = found.reachable_addresses();
        assert_eq!(
            usable,
            vec![
                "192.168.1.42".parse::<IpAddr>().unwrap(),
                // IPv4 のリンクローカルは残す (スコープ無しで届く)
                "169.254.1.1".parse::<IpAddr>().unwrap(),
                "2400:4051:bb61:9e00::1".parse::<IpAddr>().unwrap(),
            ],
            "loopback and IPv6 link-local addresses cannot reach the device"
        );
    }

    // IPv6 のリンクローカルだけを落とす理由は、スコープを捨てているから。
    // IPv4 のそれは同じリンク上ならそのまま届く
    #[test]
    fn ipv4_link_local_is_kept_but_ipv6_link_local_is_not() {
        let found = Found {
            hostname: "stackchan-a1b2c3.local".to_string(),
            addresses: vec!["169.254.1.1".parse().unwrap(), "fe80::1".parse().unwrap()],
            port: 5555,
        };
        assert_eq!(
            found.reachable_addresses(),
            vec!["169.254.1.1".parse::<IpAddr>().unwrap()]
        );
    }

    #[test]
    fn unusable_kinds_of_address_are_dropped() {
        let found = Found {
            hostname: "stackchan-a1b2c3.local".to_string(),
            addresses: vec![
                "0.0.0.0".parse().unwrap(),
                "224.0.0.251".parse().unwrap(),
                "::".parse().unwrap(),
                "ff02::fb".parse().unwrap(),
            ],
            port: 5555,
        };
        assert!(
            found.reachable_addresses().is_empty(),
            "unspecified and multicast addresses are not destinations"
        );
    }

    // 探索そのものが動かないのは失敗。見つからないことは失敗ではない
    // (その場合は空の一覧が返る)
    #[test]
    fn only_a_broken_browser_is_an_error() {
        let err = DiscoverError::Unavailable("no route".to_string());
        assert_eq!(err.exit_code(), ExitCode::Failure);
        assert!(err.to_string().contains("cannot browse"));
    }

    /// LAN に何も無い環境でも、時間内に返ること。CI でも走る
    #[test]
    fn browsing_an_empty_network_returns_nothing_rather_than_hanging() {
        let started = Instant::now();
        // 結果は問わない。機体がいれば見つかるし、multicast を塞いだコンテナや
        // 使えるインターフェースの無い環境では Unavailable が返る — どちらも
        // 環境の違いであって、ここで見たい「期限を守る」とは別のこと
        let _ = browse(Duration::from_millis(300));
        assert!(
            started.elapsed() < Duration::from_secs(3),
            "browsing should respect its deadline"
        );
    }
}
