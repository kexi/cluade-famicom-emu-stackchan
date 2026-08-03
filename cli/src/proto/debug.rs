//! type 3: デバッグスナップショット。
//!
//! ```text
//! [0..1] 'N','P'
//! [2]    version
//! [3]    3
//! [4..5] seq u16 LE   — 応答にエコーされる (実際に読まれる数少ない type)
//! [6]    flags (bit0 = APU 波形も要求)
//! [7]    0
//! ```
//!
//! 応答は `'N','D' | version | part | nparts | seq エコー u16 | payload` で、
//! ~2.1KB が MTU を超えるため自前で分割される。IP フラグメンテーションに頼ると
//! 断片が 1 つ落ちただけで全体を失うため。
//!
//! 読み取りに副作用は無い。`$2000-$401F` は 0 として返る (`$2002` を読むと
//! vblank がクリアされる等、観測が対象を変えてしまうため)。

use super::constants::{
    DEBUG_CHUNK, DEBUG_FLAG_WAVES, DEBUG_HEADER, DEBUG_MAGIC, DEBUG_MAX_PARTS, DEBUG_SNAPSHOT_SIZE,
    DEBUG_SNAPSHOT_WITH_WAVES_SIZE,
};
use super::header::{PacketType, check_reply, read_u16_le, request};

/// スナップショット本体のレイアウト (`m5stack/README.md` の表)
pub mod layout {
    /// CPU レジスタ (Web 版 `nes_cpu_regs` と同レイアウト)
    pub const CPU_REGS: (usize, usize) = (0, 12);
    /// `apuRegShadow` — `$4000-$4017` の最後の書き込み値
    pub const APU_REGS: (usize, usize) = (12, 24);
    /// PC (冗長。コード窓とセットで意味を持つ)
    pub const PC: (usize, usize) = (36, 2);
    /// PC 近傍のコード窓 (逆アセンブラが辿る最大長)
    pub const CODE_WINDOW: (usize, usize) = (38, 48);
    /// ワーク RAM
    pub const WRAM: (usize, usize) = (86, 2048);
    /// APU 波形。6 行 x 280 サンプルの uint8 (P1, P2, TRI, NOI, DMC, MIX)
    pub const WAVES: (usize, usize) = (2134, 1680);

    /// 波形の行数と 1 行のサンプル数
    pub const WAVE_ROWS: usize = 6;
    pub const WAVE_SAMPLES: usize = 280;
    pub const WAVE_LABELS: [&str; WAVE_ROWS] = ["P1", "P2", "TRI", "NOI", "DMC", "MIX"];
}

/// スナップショットを要求する
pub fn request_snapshot(seq: u16, want_waves: bool) -> Vec<u8> {
    let mut packet = request(PacketType::Debug, seq);
    packet[6] = if want_waves { DEBUG_FLAG_WAVES } else { 0 };
    packet[7] = 0;
    packet
}

/// 応答の 1 パート
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DebugPart {
    pub part: u8,
    pub nparts: u8,
    pub payload: Vec<u8>,
}

/// パートを読む。`seq` が一致しなければ `None` — 捨てた要求への遅れた応答を
/// 今のスナップショットに混ぜないため
pub fn parse_part(reply: &[u8], expect_seq: u16) -> Option<DebugPart> {
    check_reply(reply, DEBUG_MAGIC, DEBUG_HEADER)?;

    let part = reply[3];
    let nparts = reply[4];
    // 上限を見ておかないと、壊れた nparts=255 を受けて transport が来ない
    // パートを待ち続ける
    let parts_are_sane = nparts > 0 && nparts <= DEBUG_MAX_PARTS && part < nparts;
    if !parts_are_sane {
        return None;
    }

    let payload_fits = reply.len() - DEBUG_HEADER <= DEBUG_CHUNK;
    if !payload_fits {
        return None;
    }

    let seq_matches = read_u16_le(reply, 5) == expect_seq;
    if !seq_matches {
        return None;
    }

    Some(DebugPart {
        part,
        nparts,
        payload: reply[DEBUG_HEADER..].to_vec(),
    })
}

/// 揃ったパートを連結する。欠けていれば `None` (部分的なスナップショットは
/// レジスタの途中で切れた無意味なバイト列にしかならない)
pub fn assemble(mut parts: Vec<DebugPart>) -> Option<Vec<u8>> {
    let is_empty = parts.is_empty();
    if is_empty {
        return None;
    }
    parts.sort_by_key(|p| p.part);

    let nparts = parts[0].nparts;
    let has_every_part = parts.len() == nparts as usize
        && parts.iter().enumerate().all(|(i, p)| {
            let is_in_order = p.part as usize == i;
            let agrees_on_nparts = p.nparts == nparts;
            is_in_order && agrees_on_nparts
        });
    if !has_every_part {
        return None;
    }

    Some(parts.into_iter().flat_map(|p| p.payload).collect())
}

/// 分解したスナップショット
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Snapshot {
    pub cpu_regs: Vec<u8>,
    pub apu_regs: Vec<u8>,
    pub pc: u16,
    pub code_window: Vec<u8>,
    pub wram: Vec<u8>,
    /// 波形を要求していれば 6 行 x 280 サンプル
    pub waves: Option<Vec<Vec<u8>>>,
}

/// 連結済みの本体を分解する。長さが仕様のどちらとも**完全一致**しなければ `None`。
///
/// 下限判定にすると、2135..3813 バイトを「波形なし」として受理して末尾を捨てる
/// ことになる。想定外の長さは仕様の食い違いを意味するので、黙って一部だけ読むより
/// 拒否するほうがよい
pub fn parse_snapshot(body: &[u8]) -> Option<Snapshot> {
    let has_waves = body.len() == DEBUG_SNAPSHOT_WITH_WAVES_SIZE;
    let is_a_documented_length = has_waves || body.len() == DEBUG_SNAPSHOT_SIZE;
    if !is_a_documented_length {
        return None;
    }

    let slice = |(at, len): (usize, usize)| body[at..at + len].to_vec();

    let waves = if has_waves {
        let (at, _) = layout::WAVES;
        let rows = (0..layout::WAVE_ROWS)
            .map(|row| {
                let start = at + row * layout::WAVE_SAMPLES;
                body[start..start + layout::WAVE_SAMPLES].to_vec()
            })
            .collect();
        Some(rows)
    } else {
        None
    };

    Some(Snapshot {
        cpu_regs: slice(layout::CPU_REGS),
        apu_regs: slice(layout::APU_REGS),
        pc: read_u16_le(body, layout::PC.0),
        code_window: slice(layout::CODE_WINDOW),
        wram: slice(layout::WRAM),
        waves,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn request_matches_the_documented_layout() {
        let packet = request_snapshot(0x1234, false);
        assert_eq!(packet, vec![b'N', b'P', 1, 3, 0x34, 0x12, 0, 0]);

        let with_waves = request_snapshot(1, true);
        assert_eq!(with_waves[6], DEBUG_FLAG_WAVES);
    }

    fn part_bytes(seq: u16, part: u8, nparts: u8, payload: &[u8]) -> Vec<u8> {
        let s = seq.to_le_bytes();
        let mut reply = vec![b'N', b'D', 1, part, nparts, s[0], s[1]];
        reply.extend_from_slice(payload);
        reply
    }

    #[test]
    fn parse_part_reads_the_payload() {
        let reply = part_bytes(7, 0, 2, b"abc");
        let parsed = parse_part(&reply, 7).unwrap();
        assert_eq!(parsed.part, 0);
        assert_eq!(parsed.nparts, 2);
        assert_eq!(parsed.payload, b"abc");
    }

    // 捨てた要求への遅れた応答を今のスナップショットに混ぜない
    #[test]
    fn parse_part_rejects_a_stale_seq() {
        let reply = part_bytes(7, 0, 1, b"abc");
        assert!(parse_part(&reply, 8).is_none());
    }

    #[test]
    fn parse_part_rejects_malformed_datagrams() {
        let good = part_bytes(1, 0, 1, b"x");
        assert!(parse_part(&good[..6], 1).is_none(), "too short");

        let mut bad_magic = good.clone();
        bad_magic[1] = b'S';
        assert!(parse_part(&bad_magic, 1).is_none());

        let zero_parts = part_bytes(1, 0, 0, b"x");
        assert!(parse_part(&zero_parts, 1).is_none());

        let out_of_range = part_bytes(1, 3, 2, b"x");
        assert!(parse_part(&out_of_range, 1).is_none());
    }

    #[test]
    fn assemble_joins_parts_in_order() {
        let p0 = parse_part(&part_bytes(1, 0, 2, b"hello "), 1).unwrap();
        let p1 = parse_part(&part_bytes(1, 1, 2, b"world"), 1).unwrap();
        let body = assemble(vec![p1, p0]).unwrap();
        assert_eq!(body, b"hello world");
    }

    #[test]
    fn assemble_refuses_an_incomplete_set() {
        let p0 = parse_part(&part_bytes(1, 0, 2, b"half"), 1).unwrap();
        assert!(assemble(vec![p0]).is_none());
        assert!(assemble(vec![]).is_none());
    }

    #[test]
    fn snapshot_layout_has_no_gaps_or_overlaps() {
        let regions = [
            layout::CPU_REGS,
            layout::APU_REGS,
            layout::PC,
            layout::CODE_WINDOW,
            layout::WRAM,
        ];
        let mut at = 0;
        for (start, len) in regions {
            assert_eq!(
                start, at,
                "region at {start} does not follow the previous one"
            );
            at += len;
        }
        assert_eq!(at, DEBUG_SNAPSHOT_SIZE, "regions must fill the snapshot");

        // 波形は本体の直後
        assert_eq!(layout::WAVES.0, DEBUG_SNAPSHOT_SIZE);
        assert_eq!(
            layout::WAVES.0 + layout::WAVES.1,
            DEBUG_SNAPSHOT_WITH_WAVES_SIZE
        );
        assert_eq!(layout::WAVE_ROWS * layout::WAVE_SAMPLES, layout::WAVES.1);
    }

    #[test]
    fn parse_snapshot_splits_the_documented_regions() {
        let mut body = vec![0u8; DEBUG_SNAPSHOT_SIZE];
        body[layout::PC.0] = 0x34;
        body[layout::PC.0 + 1] = 0x12;
        body[layout::WRAM.0] = 0xAB;

        let snapshot = parse_snapshot(&body).unwrap();
        assert_eq!(snapshot.cpu_regs.len(), 12);
        assert_eq!(snapshot.apu_regs.len(), 24);
        assert_eq!(snapshot.pc, 0x1234);
        assert_eq!(snapshot.code_window.len(), 48);
        assert_eq!(snapshot.wram.len(), 2048);
        assert_eq!(snapshot.wram[0], 0xAB);
        assert!(snapshot.waves.is_none());
    }

    #[test]
    fn parse_snapshot_reads_waves_when_present() {
        let mut body = vec![0u8; DEBUG_SNAPSHOT_WITH_WAVES_SIZE];
        // 各行の先頭に行番号を置いて、行の切り出しがずれていないか見る
        for row in 0..layout::WAVE_ROWS {
            body[layout::WAVES.0 + row * layout::WAVE_SAMPLES] = row as u8 + 1;
        }

        let waves = parse_snapshot(&body).unwrap().waves.unwrap();
        assert_eq!(waves.len(), layout::WAVE_ROWS);
        for (row, samples) in waves.iter().enumerate() {
            assert_eq!(samples.len(), layout::WAVE_SAMPLES);
            assert_eq!(samples[0], row as u8 + 1, "wave row {row} is misaligned");
        }
    }

    #[test]
    fn parse_snapshot_rejects_a_short_body() {
        let short = vec![0u8; DEBUG_SNAPSHOT_SIZE - 1];
        assert!(parse_snapshot(&short).is_none());
    }

    // 仕様の 2 つの長さ「ちょうど」以外は受け取らない。下限判定にすると
    // 2135..3813 を波形なしとして受理し、末尾を黙って捨てることになる
    #[test]
    fn parse_snapshot_accepts_only_the_documented_lengths() {
        assert!(parse_snapshot(&vec![0u8; DEBUG_SNAPSHOT_SIZE]).is_some());
        assert!(parse_snapshot(&vec![0u8; DEBUG_SNAPSHOT_WITH_WAVES_SIZE]).is_some());

        for len in [
            DEBUG_SNAPSHOT_SIZE + 1,
            DEBUG_SNAPSHOT_WITH_WAVES_SIZE - 1,
            DEBUG_SNAPSHOT_WITH_WAVES_SIZE + 1,
        ] {
            assert!(
                parse_snapshot(&vec![0u8; len]).is_none(),
                "{len} bytes is not a documented snapshot length"
            );
        }
    }

    // 壊れた nparts を受けると、transport が来ないパートを待ち続ける
    #[test]
    fn parse_part_rejects_more_parts_than_the_device_sends() {
        let too_many = part_bytes(1, 0, DEBUG_MAX_PARTS + 1, b"x");
        assert!(parse_part(&too_many, 1).is_none());

        let at_the_limit = part_bytes(1, 0, DEBUG_MAX_PARTS, b"x");
        assert!(parse_part(&at_the_limit, 1).is_some());
    }

    #[test]
    fn parse_part_rejects_an_oversized_payload() {
        let too_long = part_bytes(1, 0, 1, &vec![0u8; DEBUG_CHUNK + 1]);
        assert!(parse_part(&too_long, 1).is_none());

        let at_the_limit = part_bytes(1, 0, 1, &vec![0u8; DEBUG_CHUNK]);
        assert!(parse_part(&at_the_limit, 1).is_some());
    }
}
