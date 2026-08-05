//! デバッグスナップショットの取得 (type 3)。
//!
//! **1 回きり。再送しない。** スナップショットは要求ごとにその時点のフレームを
//! 切り取ったもので、取り直せば別のものになる。届かなかったときに再送しても
//! 「さっきの状態」は返ってこないので、待ち直す意味がない
//! (`serve_web.py` の `fetch_debug` も同じ)。

use std::time::Duration;

use crate::exit::ExitCode;
use crate::proto::debug::{self, DebugPart, Snapshot};
use crate::transport::{Device, PartOutcome, Retry, TransportError};

/// `DEBUG_TIMEOUT_S`
const TIMEOUT: Duration = Duration::from_millis(300);

/// CPU レジスタの並び (`nes_cpu_regs` と同レイアウト)
pub mod cpu {
    pub const PC: usize = 0; // u16 LE
    pub const A: usize = 2;
    pub const X: usize = 3;
    pub const Y: usize = 4;
    pub const SP: usize = 5;
    pub const P: usize = 6;
    // [7] は padding
    pub const FRAME: usize = 8; // u32 LE
}

/// 読みやすく並べ直したスナップショット
#[derive(Debug)]
pub struct Registers {
    pub pc: u16,
    pub a: u8,
    pub x: u8,
    pub y: u8,
    pub sp: u8,
    pub p: u8,
    pub frame: u32,
}

impl Registers {
    fn from_bytes(bytes: &[u8]) -> Option<Self> {
        let is_long_enough = bytes.len() >= 12;
        if !is_long_enough {
            return None;
        }
        Some(Self {
            pc: u16::from_le_bytes([bytes[cpu::PC], bytes[cpu::PC + 1]]),
            a: bytes[cpu::A],
            x: bytes[cpu::X],
            y: bytes[cpu::Y],
            sp: bytes[cpu::SP],
            p: bytes[cpu::P],
            frame: u32::from_le_bytes([
                bytes[cpu::FRAME],
                bytes[cpu::FRAME + 1],
                bytes[cpu::FRAME + 2],
                bytes[cpu::FRAME + 3],
            ]),
        })
    }

    /// ステータスレジスタを `NV-BDIZC` の形にする。立っていないビットは小文字
    pub fn flags(&self) -> String {
        const NAMES: [(u8, char); 8] = [
            (0x80, 'N'),
            (0x40, 'V'),
            (0x20, '-'),
            (0x10, 'B'),
            (0x08, 'D'),
            (0x04, 'I'),
            (0x02, 'Z'),
            (0x01, 'C'),
        ];
        NAMES
            .iter()
            .map(|(bit, name)| {
                let is_set = self.p & bit != 0;
                if is_set {
                    *name
                } else {
                    name.to_ascii_lowercase()
                }
            })
            .collect()
    }
}

#[derive(Debug)]
pub enum DebugError {
    Transport(TransportError),
    /// 応答は来たが、仕様のどの長さとも合わない
    Malformed,
    /// 2 か所に載っている PC が食い違う。組み立てを間違えている証拠
    Inconsistent {
        cpu_pc: u16,
        window_pc: u16,
    },
}

impl DebugError {
    pub fn exit_code(&self) -> ExitCode {
        match self {
            Self::Transport(e) => e.exit_code(),
            Self::Malformed | Self::Inconsistent { .. } => ExitCode::Failure,
        }
    }
}

impl std::fmt::Display for DebugError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Transport(e) => write!(f, "{e}"),
            Self::Malformed => f.write_str("the device sent a snapshot of an unexpected size"),
            Self::Inconsistent { cpu_pc, window_pc } => write!(
                f,
                "the snapshot disagrees with itself: PC is {cpu_pc:04X} in the registers \
                 but {window_pc:04X} next to the code window"
            ),
        }
    }
}

impl From<TransportError> for DebugError {
    fn from(e: TransportError) -> Self {
        Self::Transport(e)
    }
}

pub type Result<T> = std::result::Result<T, DebugError>;

/// スナップショットを取る
pub fn snapshot(device: &mut Device, want_waves: bool) -> Result<(Snapshot, Registers)> {
    let seq = device.next_seq();
    let packet = debug::request_snapshot(seq, want_waves);
    let timeout = device.timeout_or(TIMEOUT);

    // 取り直しても「さっきの状態」は返らないので 1 回きり
    let body: Vec<u8> =
        device.request_parts(&packet, timeout, Retry::Idempotent { attempts: 1 }, || {
            let mut parts: Vec<DebugPart> = Vec::new();
            move |reply: &[u8]| {
                let Some(part) = debug::parse_part(reply, seq) else {
                    return PartOutcome::Ignore;
                };
                let is_new = !parts.iter().any(|p| p.part == part.part);
                if is_new {
                    parts.push(part);
                }
                let Some(body) = debug::assemble(parts.clone()) else {
                    return PartOutcome::NeedMore;
                };
                PartOutcome::Done(body)
            }
        })?;

    let Some(snapshot) = debug::parse_snapshot(&body) else {
        return Err(DebugError::Malformed);
    };
    let Some(registers) = Registers::from_bytes(&snapshot.cpu_regs) else {
        return Err(DebugError::Malformed);
    };

    // firmware が PC を 2 か所に載せるのは、コード窓を別の PC と組み合わせない
    // ようにするため (`core/nes.cpp:382`)。食い違うなら、そもそも組み立てを
    // 間違えている
    let pc_agrees = snapshot.pc == registers.pc;
    if !pc_agrees {
        return Err(DebugError::Inconsistent {
            cpu_pc: registers.pc,
            window_pc: snapshot.pc,
        });
    }

    Ok((snapshot, registers))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn regs(p: u8) -> Registers {
        Registers {
            pc: 0x8000,
            a: 1,
            x: 2,
            y: 3,
            sp: 0xFD,
            p,
            frame: 42,
        }
    }

    #[test]
    fn registers_are_read_in_the_documented_order() {
        // PC(2) A X Y SP P pad frame(4)
        let bytes = [0x34, 0x12, 0xAA, 0xBB, 0xCC, 0xFD, 0x24, 0, 0x0A, 0, 0, 0];
        let r = Registers::from_bytes(&bytes).expect("registers");
        assert_eq!(r.pc, 0x1234);
        assert_eq!(r.a, 0xAA);
        assert_eq!(r.x, 0xBB);
        assert_eq!(r.y, 0xCC);
        assert_eq!(r.sp, 0xFD);
        assert_eq!(r.p, 0x24);
        assert_eq!(r.frame, 10);
    }

    #[test]
    fn a_short_register_block_is_rejected() {
        assert!(Registers::from_bytes(&[0; 11]).is_none());
    }

    // 立っているフラグを大文字、いないものを小文字にする。6502 の
    // デバッガの慣習で、一目で読める
    #[test]
    fn flags_render_as_the_usual_letters() {
        assert_eq!(regs(0x00).flags(), "nv-bdizc");
        assert_eq!(regs(0xFF).flags(), "NV-BDIZC");
        // 0x24 = 未使用ビット + I
        assert_eq!(regs(0x24).flags(), "nv-bdIzc");
    }

    #[test]
    fn a_malformed_snapshot_is_a_failure_not_a_timeout() {
        assert_eq!(DebugError::Malformed.exit_code(), ExitCode::Failure);
        assert_eq!(
            DebugError::Transport(TransportError::Timeout).exit_code(),
            ExitCode::Timeout
        );
    }
}
