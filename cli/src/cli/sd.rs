//! `stackchan sd` — カード上の ROM の一覧・起動・リネーム・削除 (type 5)。
//!
//! 削除とリネームは**再送しない**。firmware は per-seq の結果キャッシュを
//! 持たないので、成功した後に送り直すと `NotFound` が返って成功が失敗に
//! 化ける。1 回送って答えが来なければ、実行されたかどうかは原理的に判らず、
//! 終了コード 4 (結果不明) で報告する。

use clap::Subcommand;

use stackchan::output::{human_bytes, quote};
use stackchan::proto::sd::SdListing;
use stackchan::sd_client;

use super::{CommandResult, GlobalArgs};

#[derive(Subcommand, Debug)]
pub enum SdCommand {
    /// List the ROMs on the card
    Ls {
        /// Show sizes in bytes instead of rounded units
        #[arg(long)]
        bytes: bool,
    },

    /// Show how much space the card has
    Df,

    /// Boot a ROM from the card
    Load {
        /// File name as shown by `sd ls`
        name: String,
    },

    /// Delete a ROM from the card
    ///
    /// Not retried: the device cannot tell a repeat from the original, so a
    /// lost reply is reported as an unknown outcome (exit 4) rather than a
    /// failure. Run `sd ls` to see what actually happened.
    #[command(alias = "delete")]
    Rm {
        /// File name as shown by `sd ls`
        name: String,
    },

    /// Rename a ROM on the card
    ///
    /// Not retried, for the same reason as `rm`.
    #[command(alias = "rename")]
    Mv {
        /// Current name
        from: String,
        /// New name
        to: String,
    },
}

pub fn run(command: &SdCommand, global: &GlobalArgs) -> CommandResult {
    let mut device = global.device()?;
    let output = global.output();

    match command {
        SdCommand::Ls { bytes } => {
            let listing =
                sd_client::list(&mut device).map_err(|e| (format!("{e}"), e.exit_code()))?;
            output.success(&render_listing(&listing, *bytes), &listing_json(&listing));
        }
        SdCommand::Df => {
            let listing =
                sd_client::list(&mut device).map_err(|e| (format!("{e}"), e.exit_code()))?;
            let used = listing.total_bytes.saturating_sub(listing.free_bytes);
            let text = format!(
                "{} free of {} ({} used, {} ROM(s))",
                human_bytes(listing.free_bytes),
                human_bytes(listing.total_bytes),
                human_bytes(used),
                listing.total
            );
            let json = format!(
                "{{\"ok\":true,\"totalBytes\":{},\"freeBytes\":{},\"usedBytes\":{},\"files\":{}}}",
                listing.total_bytes, listing.free_bytes, used, listing.total
            );
            output.success(&text, &json);
        }
        SdCommand::Load { name } => {
            sd_client::load(&mut device, name).map_err(|e| (format!("{e}"), e.exit_code()))?;
            output.success(
                &format!("booted {name}"),
                &format!("{{\"ok\":true,\"loaded\":{}}}", quote(name)),
            );
        }
        SdCommand::Rm { name } => {
            sd_client::delete(&mut device, name).map_err(|e| (format!("{e}"), e.exit_code()))?;
            output.success(
                &format!("deleted {name}"),
                &format!("{{\"ok\":true,\"deleted\":{}}}", quote(name)),
            );
        }
        SdCommand::Mv { from, to } => {
            sd_client::rename(&mut device, from, to)
                .map_err(|e| (format!("{e}"), e.exit_code()))?;
            output.success(
                &format!("renamed {from} to {to}"),
                &format!(
                    "{{\"ok\":true,\"from\":{},\"to\":{}}}",
                    quote(from),
                    quote(to)
                ),
            );
        }
    }
    Ok(())
}

fn render_listing(listing: &SdListing, exact: bool) -> String {
    let entries = sd_client::sorted(listing.entries.clone());
    let is_empty = entries.is_empty();
    if is_empty {
        return format!(
            "no ROMs on the card ({} free of {})",
            human_bytes(listing.free_bytes),
            human_bytes(listing.total_bytes)
        );
    }

    // 名前の幅を揃えて読みやすくする
    let width = entries.iter().map(|e| e.name.len()).max().unwrap_or(0);
    let mut lines: Vec<String> = entries
        .iter()
        .map(|entry| {
            let size = if exact {
                format!("{}", entry.size)
            } else {
                human_bytes(entry.size as u64)
            };
            format!("{:<width$}  {}", entry.name, size, width = width)
        })
        .collect();

    lines.push(format!(
        "{} ROM(s), {} free of {}",
        entries.len(),
        human_bytes(listing.free_bytes),
        human_bytes(listing.total_bytes)
    ));
    lines.join("\n")
}

fn listing_json(listing: &SdListing) -> String {
    let files: Vec<String> = sd_client::sorted(listing.entries.clone())
        .iter()
        .map(|e| format!("{{\"name\":{},\"size\":{}}}", quote(&e.name), e.size))
        .collect();
    format!(
        "{{\"ok\":true,\"files\":[{}],\"total\":{},\"totalBytes\":{},\"freeBytes\":{}}}",
        files.join(","),
        listing.total,
        listing.total_bytes,
        listing.free_bytes
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use stackchan::proto::sd::SdEntry;

    fn listing(entries: Vec<(&str, u32)>) -> SdListing {
        SdListing {
            total: entries.len() as u16,
            total_bytes: 8_000_000_000,
            free_bytes: 1_200_000_000,
            entries: entries
                .into_iter()
                .map(|(name, size)| SdEntry {
                    name: name.to_string(),
                    size,
                })
                .collect(),
        }
    }

    #[test]
    fn an_empty_card_says_so_rather_than_showing_nothing() {
        let text = render_listing(&listing(vec![]), false);
        assert!(text.contains("no ROMs"), "got: {text}");
        // 容量は空でも出す
        assert!(text.contains("free of"), "got: {text}");
    }

    #[test]
    fn listings_are_sorted_and_carry_the_capacity() {
        let text = render_listing(&listing(vec![("z.nes", 100), ("a.nes", 200)]), false);
        let a_at = text.find("a.nes").expect("a.nes");
        let z_at = text.find("z.nes").expect("z.nes");
        assert!(a_at < z_at, "entries should be sorted: {text}");
        assert!(text.contains("2 ROM(s)"), "got: {text}");
    }

    #[test]
    fn exact_sizes_are_available_for_scripts() {
        let text = render_listing(&listing(vec![("a.nes", 40976)]), true);
        assert!(text.contains("40976"), "got: {text}");

        let rounded = render_listing(&listing(vec![("a.nes", 40976)]), false);
        assert!(rounded.contains("41.0 KB"), "got: {rounded}");
    }

    /// 出力そのものを固定する。「JSON として正しい」の保証は
    /// バイナリ経由のテスト (tests/mock_device.rs) が行い、エスケープは
    /// `output::quote` の単体テストが受け持つ
    #[test]
    fn listing_json_has_the_expected_shape_and_order() {
        let json = listing_json(&listing(vec![("z.nes", 1), ("a.nes", 2)]));
        assert_eq!(
            json,
            concat!(
                r#"{"ok":true,"files":["#,
                r#"{"name":"a.nes","size":2},"#,
                r#"{"name":"z.nes","size":1}"#,
                r#"],"total":2,"totalBytes":8000000000,"freeBytes":1200000000}"#,
            )
        );
    }

    // 名前はこちらが決めたものではないので、JSON に載せる前にエスケープする
    #[test]
    fn names_are_escaped_in_json() {
        let json = listing_json(&listing(vec![("a\"b.nes", 1)]));
        assert!(json.contains("\"a\\\"b.nes\""), "got: {json}");
    }
}
