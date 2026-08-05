//! `stackchan discover` — LAN 上の機体を探す。
//!
//! `--host` を知らないところから始める唯一のコマンド。

use std::time::Duration;

use clap::Args;

use stackchan::discover::{self, Found};
use stackchan::output::quote;

use super::{CommandResult, GlobalArgs};

#[derive(Args, Debug)]
pub struct DiscoverArgs {
    /// How long to listen, in seconds
    ///
    /// mDNS has no moment where everything has certainly answered, so this
    /// is a trade: too short misses a distant device, too long waits around.
    #[arg(short = 't', long, default_value_t = 2.0)]
    pub timeout: f64,
}

pub fn run(args: &DiscoverArgs, global: &GlobalArgs) -> CommandResult {
    let is_sane = args.timeout > 0.0 && args.timeout <= 60.0;
    if !is_sane {
        return Err((
            format!(
                "timeout must be between 0 and 60 seconds, got {}",
                args.timeout
            ),
            stackchan::exit::ExitCode::Usage,
        ));
    }

    let found = discover::browse(Duration::from_secs_f64(args.timeout))
        .map_err(|e| (format!("{e}"), e.exit_code()))?;

    let output = global.output();
    output.success(&render(&found), &render_json(&found));
    Ok(())
}

fn render(found: &[Found]) -> String {
    let is_empty = found.is_empty();
    if is_empty {
        // 見つからないのは失敗ではないが、黙っていると探索が壊れているのか
        // 機体がいないのか分からない
        return concat!(
            "no devices found\n",
            "(the device announces itself over mDNS once it has joined WiFi; ",
            "if it is on, pass its IP with --host)"
        )
        .to_string();
    }

    let width = found.iter().map(|f| f.hostname.len()).max().unwrap_or(0);
    found
        .iter()
        .map(|device| {
            // 到達に使えるアドレスだけを出す。ループバックやリンクローカルを
            // 並べても `--host` には渡せず、行が読めなくなるだけ
            let usable = device.reachable_addresses();
            let shown = match usable.first() {
                None => "(no usable address)".to_string(),
                Some(first) => {
                    let extra = usable.len() - 1;
                    if extra > 0 {
                        format!("{first} (+{extra} more)")
                    } else {
                        first.to_string()
                    }
                }
            };
            // ポートはホスト名の側に付ける。アドレスの後ろに置くと IPv6 の
            // コロンと見分けがつかない
            format!(
                "{:<width$}  port {}  {}",
                device.hostname,
                device.port,
                shown,
                width = width
            )
        })
        .collect::<Vec<_>>()
        .join("\n")
}

fn render_json(found: &[Found]) -> String {
    let devices: Vec<String> = found
        .iter()
        .map(|device| {
            // JSON にも届くアドレスだけを載せる (テキスト出力と同じ基準)。
            // 呼び出し側がここから `--host` の値を選ぶので、渡しても届かない
            // ものを混ぜない
            let addresses: Vec<String> = device
                .reachable_addresses()
                .iter()
                .map(|a| quote(&a.to_string()))
                .collect();
            format!(
                "{{\"host\":{},\"addresses\":[{}],\"port\":{}}}",
                quote(&device.hostname),
                addresses.join(","),
                device.port
            )
        })
        .collect();
    format!(
        "{{\"ok\":true,\"devices\":[{}],\"count\":{}}}",
        devices.join(","),
        found.len()
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn device(name: &str, ip: &str) -> Found {
        Found {
            hostname: name.to_string(),
            addresses: vec![ip.parse().unwrap()],
            port: 5555,
        }
    }

    // 見つからないのは失敗ではないので、次に何をすればよいかを書く
    #[test]
    fn an_empty_result_explains_itself() {
        let text = render(&[]);
        assert!(text.contains("no devices found"), "got: {text}");
        assert!(text.contains("--host"), "it should suggest a way forward");
    }

    #[test]
    fn devices_are_listed_with_their_addresses() {
        let text = render(&[
            device("stackchan-a1b2c3.local", "192.168.1.42"),
            device("stackchan-d4e5f6.local", "192.168.1.43"),
        ]);
        assert!(text.contains("stackchan-a1b2c3.local"), "got: {text}");
        assert!(text.contains("192.168.1.42"), "got: {text}");
        assert_eq!(text.lines().count(), 2);
    }

    // ポートをアドレスの後ろに置くと IPv6 のコロンと見分けがつかない
    #[test]
    fn the_port_is_not_glued_to_an_address() {
        let text = render(&[Found {
            hostname: "stackchan-a1b2c3.local".to_string(),
            addresses: vec!["2400:4051:bb61:9e00::1".parse().unwrap()],
            port: 5555,
        }]);
        assert!(text.contains("port 5555"), "got: {text}");
        assert!(
            !text.contains("::1:5555"),
            "an IPv6 address must not run into the port: {text}"
        );
    }

    // ループバックやリンクローカルを並べても `--host` には渡せない
    #[test]
    fn unusable_addresses_are_left_out() {
        let text = render(&[Found {
            hostname: "stackchan-a1b2c3.local".to_string(),
            addresses: vec![
                "127.0.0.1".parse().unwrap(),
                "192.168.1.42".parse().unwrap(),
                "fe80::1".parse().unwrap(),
            ],
            port: 5555,
        }]);
        assert!(text.contains("192.168.1.42"), "got: {text}");
        assert!(!text.contains("127.0.0.1"), "got: {text}");
        assert!(!text.contains("fe80"), "got: {text}");
    }

    // 届くアドレスが 1 つも無いことはありうる。そのときも行は出す
    #[test]
    fn a_device_with_no_usable_address_still_appears() {
        let text = render(&[Found {
            hostname: "stackchan-a1b2c3.local".to_string(),
            addresses: vec!["127.0.0.1".parse().unwrap()],
            port: 5555,
        }]);
        assert!(text.contains("stackchan-a1b2c3.local"), "got: {text}");
        assert!(text.contains("no usable address"), "got: {text}");
    }

    #[test]
    fn json_lists_every_device() {
        let json = render_json(&[device("stackchan-a1b2c3.local", "192.168.1.42")]);
        assert!(json.contains("\"count\":1"), "got: {json}");
        assert!(
            json.contains("\"host\":\"stackchan-a1b2c3.local\""),
            "got: {json}"
        );
        assert!(json.contains("\"port\":5555"), "got: {json}");
    }

    #[test]
    fn an_empty_json_result_is_still_a_success() {
        let json = render_json(&[]);
        assert!(json.contains("\"ok\":true"), "got: {json}");
        assert!(json.contains("\"count\":0"), "got: {json}");
    }
}
