//! type 5: SD カード操作 (LIST / LOAD / DELETE / RENAME)。
//!
//! リクエストは共通 8 バイトヘッダ + op 固有のペイロード。応答は LOAD /
//! DELETE / RENAME が 8 バイトの ACK 1 発、LIST だけが必要なだけの
//! データグラムに分割される。
//!
//! **同時に処理されるのは 1 件だけ**で、保留中に届いた 2 件目には即座に
//! `Busy` が返る。実際の処理は core 1 のフレーム境界で走る (カードが SPI バスを
//! LCD と共有しているため)。

use super::constants::{
    SD_ACK_SIZE, SD_ENTRIES_PER_PART, SD_HEADER, SD_LIST_HEADER, SD_MAGIC, SD_MAX_FILES,
    SD_MAX_PARTS, SD_OP_DELETE, SD_OP_LIST, SD_OP_LOAD, SD_OP_RENAME,
};
use super::header::{PacketType, check_reply, read_u16_le, read_u32_le, read_u64_le, request};
use super::name;
use super::status::SdStatus;

/// SD の操作。
///
/// **冪等かどうかが再送の可否を決める。** firmware は per-seq の結果キャッシュを
/// 持たないので、成功した DELETE や RENAME をもう一度送ると `NotFound` が返る
/// (`sdRomRename` は移動元の存在を先に見る — `m5stack/src/sd_rom.cpp:576`)。
/// つまり成功が失敗に化ける。型で区別しておかないと、汎用の「タイムアウトしたら
/// 再送」ヘルパに無自覚に食わせてしまう
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SdOp {
    List,
    Load,
    Delete,
    Rename,
}

impl SdOp {
    pub fn wire(self) -> u8 {
        match self {
            Self::List => SD_OP_LIST,
            Self::Load => SD_OP_LOAD,
            Self::Delete => SD_OP_DELETE,
            Self::Rename => SD_OP_RENAME,
        }
    }

    /// 同じリクエストを再送してよいか。
    ///
    /// LIST と LOAD は何度送っても結果が変わらない。DELETE と RENAME は変わる
    pub fn is_idempotent(self) -> bool {
        matches!(self, Self::List | Self::Load)
    }
}

/// 一覧を要求する (ペイロードなし)
pub fn list(seq: u16) -> Vec<u8> {
    header(seq, SdOp::List)
}

/// 名前を指定して起動する
pub fn load(seq: u16, name: &str) -> Result<Vec<u8>, String> {
    with_names(seq, SdOp::Load, &[name])
}

/// 名前を指定して削除する
pub fn delete(seq: u16, name: &str) -> Result<Vec<u8>, String> {
    with_names(seq, SdOp::Delete, &[name])
}

/// リネームする。既存の名前への上書きは firmware が `Exists` で拒む
pub fn rename(seq: u16, from: &str, to: &str) -> Result<Vec<u8>, String> {
    with_names(seq, SdOp::Rename, &[from, to])
}

fn header(seq: u16, op: SdOp) -> Vec<u8> {
    let mut packet = request(PacketType::Sd, seq);
    packet[6] = op.wire();
    packet[7] = 0;
    debug_assert_eq!(packet.len(), SD_HEADER);
    packet
}

fn with_names(seq: u16, op: SdOp, names: &[&str]) -> Result<Vec<u8>, String> {
    let mut packet = header(seq, op);
    for name in names {
        packet.extend_from_slice(&name::field(name, "file name")?);
    }
    Ok(packet)
}

/// LOAD / DELETE / RENAME の ACK
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SdAck {
    pub op: u8,
    pub seq: u16,
    pub status: SdStatus,
}

/// ACK を読む。`seq` と `op` の両方が一致しなければ `None` — 捨てた
/// リクエストへの遅れた応答を、今の答えと取り違えないため
pub fn parse_ack(reply: &[u8], expect_seq: u16, expect_op: SdOp) -> Option<SdAck> {
    check_reply(reply, SD_MAGIC, SD_ACK_SIZE)?;

    let op = reply[3];
    let op_matches = op == expect_op.wire();
    if !op_matches {
        return None;
    }
    let seq = read_u16_le(reply, 4);
    let seq_matches = seq == expect_seq;
    if !seq_matches {
        return None;
    }

    Some(SdAck {
        op,
        seq,
        status: SdStatus::from_wire(reply[6]),
    })
}

/// カード上の 1 本
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SdEntry {
    pub name: String,
    pub size: u32,
}

/// LIST 応答の 1 パート
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SdListPart {
    pub part: u8,
    pub nparts: u8,
    /// 一覧全体の本数 (このパートの本数ではない)
    pub total: u16,
    pub total_bytes: u64,
    pub free_bytes: u64,
    pub entries: Vec<SdEntry>,
}

/// LIST のパートを読む。
///
/// **カウントよりデータグラム長を信じる。** count を信じて読み進めると、
/// 切り詰められたデータグラムが名前を半分だけ読んだエントリを混ぜてくる
/// (`serve_web.py:378` の注記と同じ)
pub fn parse_list_part(reply: &[u8], expect_seq: u16) -> Option<SdListPart> {
    let ack = parse_ack(reply, expect_seq, SdOp::List)?;
    let is_ok = ack.status.is_ok();
    if !is_ok {
        return None;
    }

    let has_list_header = reply.len() >= SD_LIST_HEADER;
    if !has_list_header {
        return None;
    }

    let part = reply[8];
    let nparts = reply[9];
    // nparts は必ず 1 以上。空のカードも count=0 のパートを 1 つ返すので、
    // 0 は壊れたデータグラム。上限も見るのは、壊れた nparts=255 を受けると
    // transport が来ないパートを deadline まで待ち続けるため
    let parts_are_sane = nparts > 0 && (nparts as usize) <= SD_MAX_PARTS && part < nparts;
    if !parts_are_sane {
        return None;
    }

    let total = read_u16_le(reply, 10);
    let count = read_u16_le(reply, 12);
    // count は下の with_capacity に渡るので、確保の前に見る。30 バイトの
    // データグラムに count=65535 を載せるだけで大きな確保を試みてしまう
    let counts_are_sane = (total as usize) <= SD_MAX_FILES
        && (count as usize) <= SD_ENTRIES_PER_PART
        && count <= total;
    if !counts_are_sane {
        return None;
    }

    let total_bytes = read_u64_le(reply, 14);
    let free_bytes = read_u64_le(reply, 22);

    let mut entries = Vec::with_capacity(count as usize);
    let mut at = SD_LIST_HEADER;
    for _ in 0..count {
        let has_entry_header = at + 5 <= reply.len();
        if !has_entry_header {
            return None;
        }
        let size = read_u32_le(reply, at);
        let name_len = reply[at + 4] as usize;
        at += 5;

        let has_name = at + name_len <= reply.len();
        if !has_name {
            return None;
        }
        // 名前は firmware のサニタイザを通った `[A-Za-z0-9._-]` のはず。
        // それ以外は正常な応答ではないので、置換して読んだりせずパートごと
        // 捨てて再送させる — 一覧に出ているのに LOAD も DELETE もできない行が
        // できるほうが困る
        let Ok(name) = std::str::from_utf8(&reply[at..at + name_len]) else {
            return None;
        };
        if !name::is_listable(name) {
            return None;
        }
        at += name_len;

        entries.push(SdEntry {
            name: name.to_string(),
            size,
        });
    }

    // count を読み終えて余りがあるなら、count が実データより小さい。
    // 黙って捨てると一覧が少なく見えるので、パートごと拒否して再送させる
    let consumed_everything = at == reply.len();
    if !consumed_everything {
        return None;
    }

    Some(SdListPart {
        part,
        nparts,
        total,
        total_bytes,
        free_bytes,
        entries,
    })
}

/// 揃ったパートを 1 つの一覧にまとめる。
///
/// 呼び出し側は**部分的な結果を返してはいけない**。1 パート落ちたら全体を捨てて
/// 再送する (「ROM が少なく見える」より 1 往復遅い方がまし)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SdListing {
    pub entries: Vec<SdEntry>,
    pub total: u16,
    pub total_bytes: u64,
    pub free_bytes: u64,
}

pub fn assemble_listing(mut parts: Vec<SdListPart>) -> Option<SdListing> {
    let is_empty = parts.is_empty();
    if is_empty {
        return None;
    }
    parts.sort_by_key(|p| p.part);

    let first = parts[0].clone();
    let nparts = first.nparts;
    let has_every_part = parts.len() == nparts as usize
        && parts.iter().enumerate().all(|(i, p)| p.part as usize == i);
    if !has_every_part {
        return None;
    }

    // 容量と総数は全パートに載っている (パート 0 を落としても再送で拾えるように
    // するための冗長化)。食い違うなら別々の応答が混ざっているので、まとめては
    // いけない
    let parts_agree = parts.iter().all(|p| {
        p.nparts == nparts
            && p.total == first.total
            && p.total_bytes == first.total_bytes
            && p.free_bytes == first.free_bytes
    });
    if !parts_agree {
        return None;
    }

    let entries: Vec<SdEntry> = parts.iter().flat_map(|p| p.entries.clone()).collect();

    // 集まった本数が宣言された総数と合わないなら、どこかのパートが count を
    // 少なく申告している。「ROM が少なく見える」形で静かに嘘をつくより拒否する
    let count_matches = entries.len() == first.total as usize;
    if !count_matches {
        return None;
    }
    // firmware が一覧に載せる上限を超えることはない
    let is_within_the_device_limit = entries.len() <= SD_MAX_FILES;
    if !is_within_the_device_limit {
        return None;
    }

    Some(SdListing {
        total: first.total,
        total_bytes: first.total_bytes,
        free_bytes: first.free_bytes,
        entries,
    })
}

#[cfg(test)]
mod tests {
    use super::super::constants::SD_NAME_MAX;
    use super::*;

    #[test]
    fn list_is_the_bare_header() {
        let packet = list(0x0102);
        assert_eq!(packet, vec![b'N', b'P', 1, 5, 0x02, 0x01, 0, 0]);
    }

    #[test]
    fn load_appends_a_length_prefixed_name() {
        let packet = load(1, "game.nes").unwrap();
        assert_eq!(&packet[0..8], &[b'N', b'P', 1, 5, 1, 0, SD_OP_LOAD, 0]);
        assert_eq!(packet[8], 8);
        assert_eq!(&packet[9..], b"game.nes");
    }

    #[test]
    fn rename_appends_both_names() {
        let packet = rename(1, "a.nes", "b.nes").unwrap();
        assert_eq!(packet[6], SD_OP_RENAME);
        assert_eq!(packet[8], 5);
        assert_eq!(&packet[9..14], b"a.nes");
        assert_eq!(packet[14], 5);
        assert_eq!(&packet[15..20], b"b.nes");
    }

    // 通らないと判っている名前で往復を無駄にしない
    #[test]
    fn names_are_validated_before_sending() {
        assert!(load(0, "").is_err());
        assert!(load(0, &"x".repeat(SD_NAME_MAX + 1)).is_err());
        assert!(load(0, &"x".repeat(SD_NAME_MAX)).is_ok());
    }

    // UTF-8 の名前はバイト数で数える。文字数で数えると 63 バイト制限を超える
    #[test]
    fn name_length_is_measured_in_bytes() {
        // 3 バイト x 21 文字 = 63 バイト
        let just_fits = "あ".repeat(21);
        assert_eq!(just_fits.len(), 63);
        assert!(load(0, &just_fits).is_ok());

        let too_long = "あ".repeat(22);
        assert!(load(0, &too_long).is_err());
    }

    // `-` 始まりの名前は firmware が許すので実在しうる
    #[test]
    fn hyphen_leading_names_are_accepted() {
        let packet = delete(0, "-x.nes").unwrap();
        assert_eq!(&packet[9..], b"-x.nes");
    }

    // 名前の途中の NUL は firmware から見える名前を切る。`game.nes\0other` を
    // 削除しようとすると `game.nes` が消える — 利用者が指定したのとは別のファイル
    #[test]
    fn names_with_an_embedded_nul_are_rejected() {
        assert!(delete(0, "game.nes\0other").is_err());
        assert!(load(0, "a\0b").is_err());
        assert!(rename(0, "a.nes", "b\0c").is_err());
        assert!(rename(0, "a\0b", "c.nes").is_err());
    }

    #[test]
    fn idempotence_matches_the_retry_policy() {
        assert!(SdOp::List.is_idempotent());
        assert!(SdOp::Load.is_idempotent());
        // ここが true になると、再送で成功が失敗に化ける
        assert!(!SdOp::Delete.is_idempotent());
        assert!(!SdOp::Rename.is_idempotent());
    }

    fn ack_bytes(op: u8, seq: u16, status: u8) -> Vec<u8> {
        let s = seq.to_le_bytes();
        vec![b'N', b'S', 1, op, s[0], s[1], status, 0]
    }

    #[test]
    fn parse_ack_reads_the_status() {
        let reply = ack_bytes(SD_OP_DELETE, 0x1234, 2);
        let ack = parse_ack(&reply, 0x1234, SdOp::Delete).unwrap();
        assert_eq!(ack.status, SdStatus::NotFound);
        assert_eq!(ack.seq, 0x1234);
    }

    // 捨てたリクエストへの遅れた応答を、今の答えと取り違えない
    #[test]
    fn parse_ack_rejects_a_stale_seq_or_op() {
        let reply = ack_bytes(SD_OP_DELETE, 7, 0);
        assert!(parse_ack(&reply, 8, SdOp::Delete).is_none(), "wrong seq");
        assert!(parse_ack(&reply, 7, SdOp::Load).is_none(), "wrong op");
        assert!(parse_ack(&reply, 7, SdOp::Delete).is_some());
    }

    #[test]
    fn parse_ack_rejects_malformed_datagrams() {
        let good = ack_bytes(SD_OP_LOAD, 1, 0);
        assert!(parse_ack(&good[..7], 1, SdOp::Load).is_none(), "too short");

        let mut bad_magic = good.clone();
        bad_magic[1] = b'R';
        assert!(parse_ack(&bad_magic, 1, SdOp::Load).is_none());

        let mut bad_version = good.clone();
        bad_version[2] = 2;
        assert!(parse_ack(&bad_version, 1, SdOp::Load).is_none());
    }

    /// LIST パートを組み立てる。テスト用。
    ///
    /// `total` は**一覧全体の本数**で、このパートが運ぶ本数 (count) とは別物。
    /// 分割された応答では両者が食い違うのが普通なので、別々に受け取る
    fn list_part_with_total(
        seq: u16,
        part: u8,
        nparts: u8,
        total: u16,
        entries: &[(&str, u32)],
    ) -> Vec<u8> {
        let s = seq.to_le_bytes();
        let mut reply = vec![b'N', b'S', 1, SD_OP_LIST, s[0], s[1], 0, 0];
        reply.push(part);
        reply.push(nparts);
        reply.extend_from_slice(&total.to_le_bytes());
        reply.extend_from_slice(&(entries.len() as u16).to_le_bytes()); // count
        reply.extend_from_slice(&8_000_000_000u64.to_le_bytes()); // totalBytes
        reply.extend_from_slice(&1_200_000_000u64.to_le_bytes()); // freeBytes
        for (name, size) in entries {
            reply.extend_from_slice(&size.to_le_bytes());
            reply.push(name.len() as u8);
            reply.extend_from_slice(name.as_bytes());
        }
        reply
    }

    /// 1 パートで収まる一覧 (total == count)
    fn list_part(seq: u16, part: u8, nparts: u8, entries: &[(&str, u32)]) -> Vec<u8> {
        list_part_with_total(seq, part, nparts, entries.len() as u16, entries)
    }

    #[test]
    fn parse_list_part_reads_entries_and_capacity() {
        let reply = list_part(1, 0, 1, &[("game.nes", 40976), ("smb.nes", 40976)]);
        let parsed = parse_list_part(&reply, 1).unwrap();
        assert_eq!(parsed.entries.len(), 2);
        assert_eq!(parsed.entries[0].name, "game.nes");
        assert_eq!(parsed.entries[0].size, 40976);
        assert_eq!(parsed.total_bytes, 8_000_000_000);
        assert_eq!(parsed.free_bytes, 1_200_000_000);
    }

    // 空のカードは count=0 のパートを 1 つ返す。「マウント済みで空」と
    // 「応答なし」を区別できなければならない
    #[test]
    fn an_empty_card_is_a_valid_listing() {
        let reply = list_part(1, 0, 1, &[]);
        let parsed = parse_list_part(&reply, 1).unwrap();
        assert!(parsed.entries.is_empty());
        assert_eq!(parsed.nparts, 1);

        let listing = assemble_listing(vec![parsed]).unwrap();
        assert!(listing.entries.is_empty());
    }

    // カウントを信じてデータグラム長を無視すると、切り詰められた応答が
    // 名前を半分だけ読んだエントリを混ぜてくる
    #[test]
    fn a_truncated_datagram_is_rejected_not_half_read() {
        let full = list_part(1, 0, 1, &[("game.nes", 40976)]);
        for cut in 1..=8 {
            let truncated = &full[..full.len() - cut];
            assert!(
                parse_list_part(truncated, 1).is_none(),
                "a datagram cut by {cut} bytes must be rejected"
            );
        }
    }

    #[test]
    fn nonsensical_part_indices_are_rejected() {
        let mut reply = list_part(1, 0, 1, &[]);
        reply[9] = 0; // nparts = 0
        assert!(parse_list_part(&reply, 1).is_none());

        let mut reply = list_part(1, 0, 1, &[]);
        reply[8] = 3; // part >= nparts
        assert!(parse_list_part(&reply, 1).is_none());
    }

    // 非 Ok のステータスが載っていたら、後続は無い
    #[test]
    fn a_failed_list_carries_no_entries() {
        let mut reply = list_part(1, 0, 1, &[]);
        reply[6] = 1; // NotMounted
        assert!(parse_list_part(&reply, 1).is_none());
    }

    #[test]
    fn assemble_joins_parts_in_order() {
        // 2 パートで合計 2 本。total は全体の本数なのでどちらも 2
        let p0 = parse_list_part(&list_part_with_total(1, 0, 2, 2, &[("a.nes", 1)]), 1).unwrap();
        let p1 = parse_list_part(&list_part_with_total(1, 1, 2, 2, &[("b.nes", 2)]), 1).unwrap();

        // 順不同で渡しても並べ直す
        let listing = assemble_listing(vec![p1, p0]).unwrap();
        let names: Vec<&str> = listing.entries.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(names, vec!["a.nes", "b.nes"]);
        assert_eq!(listing.total, 2);
    }

    // 部分的な結果を返すと「ROM が少なく見える」形で静かに嘘をつく
    #[test]
    fn assemble_refuses_an_incomplete_set() {
        let p0 = parse_list_part(&list_part_with_total(1, 0, 2, 2, &[("a.nes", 1)]), 1).unwrap();
        assert!(assemble_listing(vec![p0]).is_none(), "part 1 is missing");
        assert!(assemble_listing(vec![]).is_none());
    }

    #[test]
    fn assemble_refuses_duplicated_parts() {
        let p0 = parse_list_part(&list_part_with_total(1, 0, 2, 2, &[("a.nes", 1)]), 1).unwrap();
        assert!(assemble_listing(vec![p0.clone(), p0]).is_none());
    }

    // count が実データより小さいと末尾が黙って捨てられ、一覧が少なく見える
    #[test]
    fn a_part_that_under_reports_its_count_is_rejected() {
        let mut reply = list_part(1, 0, 1, &[("a.nes", 1), ("b.nes", 2)]);
        reply[12] = 1; // count を 2 -> 1 に偽る
        reply[13] = 0;
        assert!(parse_list_part(&reply, 1).is_none());
    }

    // 名前は firmware のサニタイザを通った ASCII のはず。置換して読むと、
    // その名前で DELETE を送っても同じファイルを指せない
    #[test]
    fn a_part_with_a_non_utf8_name_is_rejected() {
        let mut reply = list_part(1, 0, 1, &[("ab.nes", 1)]);
        let name_at = SD_LIST_HEADER + 5;
        reply[name_at] = 0xFF;
        assert!(parse_list_part(&reply, 1).is_none());
    }

    // サニタイザが作りえない名前も同様。一覧に出るのに操作できない行を作らない
    #[test]
    fn a_part_with_an_unsanitised_name_is_rejected() {
        let mut reply = list_part(1, 0, 1, &[("ab.nes", 1)]);
        let name_at = SD_LIST_HEADER + 5;
        reply[name_at] = b'/'; // サニタイザなら `_` になっている
        assert!(parse_list_part(&reply, 1).is_none());
    }

    // 壊れた nparts を受けると、transport が来ないパートを待ち続ける
    #[test]
    fn part_counts_beyond_the_device_maximum_are_rejected() {
        // firmware の式で決まる上限 (perPart=19, 64 本で 4 パート)
        assert_eq!(SD_ENTRIES_PER_PART, 19);
        assert_eq!(SD_MAX_PARTS, 4);

        let at_the_limit = list_part_with_total(1, 0, SD_MAX_PARTS as u8, 1, &[("a.nes", 1)]);
        assert!(parse_list_part(&at_the_limit, 1).is_some());

        let too_many = list_part_with_total(1, 0, SD_MAX_PARTS as u8 + 1, 1, &[("a.nes", 1)]);
        assert!(parse_list_part(&too_many, 1).is_none());
    }

    // count は with_capacity に渡るので、確保の前に検査する。30 バイトの
    // データグラムに count=65535 を載せるだけで大きな確保を試みてしまう
    #[test]
    fn an_absurd_count_is_rejected_before_allocating() {
        let mut reply = list_part(1, 0, 1, &[]);
        reply[12] = 0xFF;
        reply[13] = 0xFF;
        assert!(parse_list_part(&reply, 1).is_none());
    }

    #[test]
    fn a_total_beyond_the_device_maximum_is_rejected() {
        let too_many = list_part_with_total(1, 0, 1, SD_MAX_FILES as u16 + 1, &[("a.nes", 1)]);
        assert!(parse_list_part(&too_many, 1).is_none());
    }

    // count は total を超えられない (このパートが全体より多くを運ぶことはない)
    #[test]
    fn a_count_larger_than_total_is_rejected() {
        let inconsistent = list_part_with_total(1, 0, 1, 1, &[("a.nes", 1), ("b.nes", 2)]);
        assert!(parse_list_part(&inconsistent, 1).is_none());
    }

    // 容量や総数が食い違うパートは、別々の応答が混ざった証拠
    #[test]
    fn assemble_refuses_parts_that_disagree_on_metadata() {
        let p0 = parse_list_part(&list_part(1, 0, 2, &[("a.nes", 1)]), 1).unwrap();

        let mut disagrees = p0.clone();
        disagrees.part = 1;
        disagrees.free_bytes += 1;
        assert!(assemble_listing(vec![p0.clone(), disagrees]).is_none());

        let mut other_total = p0.clone();
        other_total.part = 1;
        other_total.total += 5;
        assert!(assemble_listing(vec![p0, other_total]).is_none());
    }

    // 集まった本数と宣言された総数が合わないなら、どこかが count を偽っている
    #[test]
    fn assemble_refuses_when_the_entry_count_disagrees_with_total() {
        let mut p0 = parse_list_part(&list_part(1, 0, 1, &[("a.nes", 1)]), 1).unwrap();
        p0.total = 9;
        assert!(assemble_listing(vec![p0]).is_none());
    }
}
