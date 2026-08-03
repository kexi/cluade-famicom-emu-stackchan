//! UDP プロトコルのエンコードとデコード。
//!
//! ネットワークを一切持たない純粋関数だけを置く。ソケットの扱いや再送は
//! `transport` の仕事で、ここはバイト列との相互変換に徹する — そうすると
//! `m5stack/README.md` の仕様表と 1 対 1 で照合できるテストが書ける。
//!
//! 仕様の権威は `m5stack/src/config.h` と `m5stack/README.md:201-347`。

pub mod constants;
pub mod ctrl;
pub mod debug;
pub mod header;
pub mod name;
pub mod pad;
pub mod pins;
pub mod rom;
pub mod sd;
pub mod status;

pub use header::PacketType;
pub use status::{RomStatus, SdStatus};
