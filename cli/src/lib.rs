//! M5Stack CoreS3 のファミコンエミュレータを操作するためのライブラリ。
//!
//! 実機は HTTP を持たず、UDP :5555 の独自プロトコルだけを喋る。このクレートは
//! そのプロトコル (`proto`) と、再送・タイムアウトの戦略 (`transport`) を提供し、
//! バイナリ (`main.rs`) はその利用者に徹する。
//!
//! プロトコルの権威は `m5stack/src/config.h` で、ここの定数はその写し。対応する
//! C++ 側の定数名を各定義のドキュメントに書いてあるので、食い違いを疑ったときは
//! そちらを引くこと。

pub mod debug_client;
pub mod discover;
pub mod exit;
pub mod output;
pub mod proto;
pub mod rom_client;
pub mod sd_client;
pub mod transport;
