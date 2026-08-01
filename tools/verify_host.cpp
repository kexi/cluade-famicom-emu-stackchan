// ホスト側 bit-exact 検証ハーネス。
//
// コア (core/*.cpp) は Web(WASM) 版と M5Stack 組み込み版で共有され、
// -DNES_EMBEDDED の有無で二形態になる:
//
//   参照ビルド  : CPU 先行なしの lockstep、Pixel = uint32 ARGB
//   組み込み    : CPU 先行 + 遅延 catch-up、Pixel = uint16 RGB565 (byte-swap)
//
// 設計上「観測可能な挙動は lockstep と同一」であるはず (core/nes.h の
// catchUp() のコメント) だが、これは人手のレビューでしか担保されていない。
// このハーネスは同じ ROM を両形態で走らせ、毎フレームの CPU/PPU/APU 状態と
// 画面内容を決定的なテキストに落として機械的に突き合わせる。今後 PPU を
// 最適化してタイミングを壊した場合、desync がフレーム単位で検出できる。
//
// 出力は 1 フレーム 1 行のテキストで、diff にかけることを前提にしている。
// バイナリのダンプにしないのは、差分が出たときに「どのフレームのどの列が
// どう違うか」が目で読めることのほうが、サイズより価値があるため。
//
// ---- 観測点について ----
//
// runFrame() は「PPU が scanline 241 dot 1 に達した」あと、実行中の CPU 命令
// が終わるまで回ってから戻る。組み込みビルドは PPU を catch-up でまとめて
// 進めるので、その行き過ぎ量が参照ビルドと違う。結果として両者は同じ
// *フレーム* で止まるが同じ *dot* では止まらず、PPU の dot / CPU の PC は
// フレーム境界では原理的に一致しない。
//
// これは core/nes.h の catchUp() が明示している設計上の許容 (「IRQ は数十
// CPU サイクル遅れて見える」) であって desync ではない。よって dot と PC は
// 「参考列」として出力はするが一致判定には使わない。判定に使うのは、観測点
// のずれに影響されない蓄積状態 — WRAM / OAM / VRAM / framebuffer の CRC と、
// PPU/APU のうちフレーム単位で確定する値 — に絞る。
//
// ---- 既知の差分 (2026-08 時点) ----
//
// game.nes 600 フレームで、STATE 列のうち framebuffer 以外 (WRAM/OAM/VRAM/
// ctrl/mask/oamAddr/t/fineX/w/frameCount/長さカウンタ/IRQ) は全フレーム一致
// する。一方 framebuffer だけが 600 中 73 フレームで食い違う。
//
// 差分は毎回 scanline 135 の最終タイル (x>=248) に限定され、1 フレームあたり
// 1-5 ピクセル。組み込み側の renderScanline() が 1 ライン分を dot 256 で一括
// 描画するのに対し参照側の renderDot() はライン内で逐次フェッチするため、この
// ゲームが scanline 135 で行うライン途中のレジスタ書き換えが、そのライン最後の
// タイルの取り込みタイミングとしてしか現れないことによる。
//
// この差分は「renderScanline のライン一括描画の既知の副作用」として許容し、
// verify は既知領域に収まっている限り PASS とする (件数は WARN で報告)。
//
// 許容窓の座標 (scanline / x) とその判定は justfile の verify レシピが持つ
// (known_line / known_x と、それを使う awk)。ハーネス側は窓を知らない: 窓は
// ROM 固有の値で、ROM を差し替えれば引き直す対象だが、ハーネスは ROM に依存
// しない道具として保ちたい。判定ロジックを二重に持たないよう、ここでは窓の
// 存在だけを述べ、具体的な座標は justfile の該当箇所を参照すること。
//
// ---- 出力モード ----
//
//   <rom> [frames] [all|skip4] [scenario <file>]
//       1 フレーム 1 行のテキスト。CRC で全フレームを一気に比較する
//   <rom> [frames] [all|skip4] dump <frames> [scenario <file>]
//       <frames> はカンマ区切りのフレーム番号リスト (例 3,76,120)。指定された
//       フレームの framebuffer をパレット index 列 (61440 byte/フレーム) として
//       与えた順ではなく昇順に連結して stdout へ raw 出力する
//
// dump を分けているのは出力サイズとの折り合い。全フレームの index 列を出すと
// 600 フレームで 36MB になる一方、CRC が食い違うのは一部のフレームだけなので、
// justfile 側は「まず trace で CRC 比較 → 食い違ったフレームだけ dump で
// 引き直してピクセル位置を特定する」2 パスで動かす。
//
// dump が複数フレームを一度に受けるのは、1 フレームにつき powerOn からの
// フル再実行を要求すると差分フレーム数 n に対して O(n^2) になるため。600
// フレーム中 73 フレーム差分なら 73 回 x 2 ビルドの再実行になっていた。
// リストで受けて 1 回の実行中に順次吐けば、ビルドごと 1 回で済む。
//
// 出力は「61440 バイト固定長 x 指定フレーム数」の連結。区切りもヘッダも入れて
// いないのは、フレームあたりの長さが固定で自己記述的だから — 呼び出し側は
// 昇順に並んだ i 番目のフレームを offset i*61440 で切り出せる。
//
// ---- 入力スクリプト ----
//
// 既定は無入力 (パッドを一切触らない) で、決定性を最優先する。scenario を
// 渡した場合だけ、指定フレームからパッド状態を切り替える。書式は 1 行 1 イベント:
//
//   <frame> <buttons>
//
// <buttons> は A/B/SELECT/START/UP/DOWN/LEFT/RIGHT の記号を + で繋いだもの、
// または 0 (全解放) か 0x なしの 16 進ビット列。'#' 以降は行コメント。
// イベントはそのフレームの runFrame() の直前に適用され、次のイベントまで
// 保持される — つまり「押しっぱなし」が既定で、離すには解放イベントを書く。
// 記号で書けるようにしたのは、シナリオが人手で書かれ人手で読まれるため。
// tools/scenario-sample.txt が実例。

#include "../core/nes.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------- CRC32
//
// 外部依存を持たないテーブル方式の CRC32 (IEEE 802.3, 反転多項式 0xEDB88320)。
// zlib をリンクしないのは、このハーネスがコアと同じ「素の clang++ だけで
// 通る」制約を共有しているため。ハッシュの暗号学的強度は要らず、必要なのは
// 「同じ入力なら両ビルドで同じ値」だけなので、CRC32 で十分。
uint32_t crcTable[256];

void buildCrcTable() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crcTable[i] = c;
    }
}

uint32_t crc32(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------- framebuffer の正規化
//
// 参照ビルドは Pixel=uint32 ARGB、組み込みは Pixel=uint16 の byte-swap 済み
// RGB565 で、同じ絵でもビット列が違う。そのままハッシュしても比較できない
// ので、ピクセル値 → NES パレット index (0-63) に逆変換してから CRC を取る。
//
// 逆変換表はコア側の palLut と同じ式で組み立てる。コアの静的テーブルを直接
// 参照しない (できない) のは、あれが ppu.cpp のファイルスコープに閉じている
// ため。式を二重に持つことになるが、ここが狂えば下の自己検査が落ちる。
//
// パレットには元から重複がある: 0xFF000000 が 10 箇所、0xFFFFFEFF が 2 箇所。
// これは NES パレット自体の性質で、RGB565 に落としても増えない (どちらの形式
// でも相異なる値は 54 個)。よって重複は「最初に現れた index」に畳んで正規化
// する。両ビルドが同じ規則で畳むので、比較の妥当性は失われない。
constexpr int PALETTE_SIZE = 64;

nes::Pixel palettePixel(int index) {
    const uint32_t c = nes::NES_PALETTE[index];
#ifdef NES_EMBEDDED
    const uint8_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    const uint16_t rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return (nes::Pixel)__builtin_bswap16(rgb565);
#else
    return (nes::Pixel)c;
#endif
}

std::vector<nes::Pixel> canonicalPixels;   // canonical index -> pixel value
// 逆引き。要素は 54 個しかないので線形探索でも「正しい」が、
// normalizeFramebuffer() が 1 フレームあたり 61440 回ここを引き、それを 3 系列
// x 600 フレーム繰り返すため、平均 27 回の比較が積もると検証全体の実行時間に
// 効いてくる。定数時間の引きに替える。
std::unordered_map<nes::Pixel, int> canonicalIndex;

// ピクセル値 → canonical index。未知の値は正規化できないので -1 を返し、
// 呼び出し側が異常終了する。
int canonicalIndexOf(nes::Pixel px) {
    const auto it = canonicalIndex.find(px);
    return it == canonicalIndex.end() ? -1 : it->second;
}

// 逆マップを構築し、正規化が成立することを起動時に検査する。
//
// 検査するのは「相異なるピクセル値の個数が、期待どおり ARGB と RGB565 で
// 一致すること」。RGB565 は 5/6/5 bit に丸めるので、原理的には ARGB では
// 区別できていた 2 色が同じ値に潰れうる。そうなると組み込み側の画面だけ
// 情報が落ち、両者の CRC 比較は「差が出ないこと」を保証しなくなる。
// 黙って通すと検証が骨抜きになるため、その場合は即エラー終了する。
bool buildPaletteInverse(std::string& err) {
    canonicalPixels.clear();
    canonicalIndex.clear();
    for (int i = 0; i < PALETTE_SIZE; i++) {
        const nes::Pixel px = palettePixel(i);
        // 重複は「最初に現れた index」に畳む。emplace は既存キーを上書きしない
        // ので、この畳み方がそのまま表現できる。
        if (canonicalIndexOf(px) < 0) {
            canonicalIndex.emplace(px, (int)canonicalPixels.size());
            canonicalPixels.push_back(px);
        }
    }

    // NES パレット 64 色のうち相異なるのは 54 色 (0xFF000000 x10, 0xFFFFFEFF x2
    // が重複)。RGB565 でもこの数が保たれることが、丸めによる衝突が起きていない
    // ことの必要十分な確認になる。
    constexpr size_t EXPECTED_DISTINCT = 54;
    if (canonicalPixels.size() != EXPECTED_DISTINCT) {
        char buf[160];
        snprintf(buf, sizeof(buf), "palette collision: %zu distinct pixel values, expected %zu", canonicalPixels.size(),
                 EXPECTED_DISTINCT);
        err = buf;
        return false;
    }
    return true;
}

constexpr int FB_PIXELS = 256 * 240;

// framebuffer を canonical index 列へ正規化する。trace の CRC も dump の raw
// 出力も同じここを通るので、両モードが指す「画面の内容」は定義上ひとつになる。
uint8_t normalizedFb[FB_PIXELS];

bool normalizeFramebuffer(const nes::PPU& ppu) {
    for (int i = 0; i < FB_PIXELS; i++) {
        const int idx = canonicalIndexOf(ppu.framebuffer[i]);
        if (idx < 0) return false;
        normalizedFb[i] = (uint8_t)idx;
    }
    return true;
}

// ---------------------------------------------------------------- 実行
uint8_t cpuStatusByte(const nes::CPU& cpu) {
    return (uint8_t)((cpu.fN << 7) | (cpu.fV << 6) | 0x20 | (cpu.fD << 3) | (cpu.fI << 2) | (cpu.fZ << 1) |
                     (uint8_t)cpu.fC);
}

// ------------------------------------------------------- 入力スクリプト
//
// 「フレーム f からパッド状態 bits」を並べたもの。runFrame() の直前に、その
// フレームに一致するイベントがあれば適用する。保持式 (次のイベントまで有効)
// にしているのは、NES のパッドがラッチではなくレベルであるのと同じ理由 —
// 「押している間」を表すのに毎フレーム行を書かせるのは非現実的。
struct PadEvent {
    int frame;
    uint8_t bits;
};

// 非負の 10 進整数だけを受ける。atoi を直に使わないのは、非数値を黙って 0 に
// 変えてしまうため — シナリオの "abc 150" のような書き間違いが「frame 0」として
// 通ると、意図と違う入力で検証が回ったうえに PASS まで見えてしまう。数値を
// 受け取る箇所はすべてここを通し、拒否の方針を 1 つに揃える。
bool parseNonNegativeInt(const std::string& tok, int& out) {
    if (tok.empty() || tok.find_first_not_of("0123456789") != std::string::npos) return false;
    // 桁数で溢れを弾く。10 桁を超えると int の範囲を出うるが、フレーム番号に
    // その大きさが要る場面はないので、無条件に拒否で足りる。
    if (tok.size() > 9) return false;
    out = atoi(tok.c_str());
    return true;
}

// config.h の NES_BTN_* と同じ並び。ここで再定義しているのは、config.h が
// m5stack のビルド設定 (Arduino ヘッダ前提) であってホストから引けないため。
// コア側の Controller::setButtons はビット位置を規定していないので、ずれたら
// シナリオの意味が変わる。web/main.js の PAD ビットとも同じ並び。
struct ButtonName {
    const char* name;
    uint8_t bit;
};
constexpr ButtonName BUTTON_NAMES[] = {{"A", 0x01},    {"B", 0x02},    {"SELECT", 0x04}, {"START", 0x08}, {"UP", 0x10},
                                       {"DOWN", 0x20}, {"LEFT", 0x40}, {"RIGHT", 0x80},  {"NONE", 0x00}};

bool parseButtons(const std::string& spec, uint8_t& bits, std::string& err) {
    bits = 0;
    // 16 進のビット列。記号を覚えていなくても書けるし、UDP プロトコルの
    // ダンプをそのまま貼れる。
    //
    // 記号名より先に判定するが、"A" と "B" は 16 進数字でもありボタン名でも
    // ある。ここで hex を優先すると、シナリオに書いた A が 0x0A (= SELECT|DOWN)
    // として黙って通り、書いた人の意図と違う入力で「検証が通った」ことになる。
    // 名前が付いているトークンは常に名前として読む方を優先し、hex 解釈は
    // ボタン名でないものに限る。
    bool isButtonName = false;
    for (const ButtonName& b : BUTTON_NAMES) {
        if (spec == b.name) {
            isButtonName = true;
            break;
        }
    }
    const bool hexDigitsOnly =
        !spec.empty() && spec.size() <= 2 && spec.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    if (hexDigitsOnly && !isButtonName) {
        bits = (uint8_t)strtoul(spec.c_str(), nullptr, 16);
        return true;
    }
    size_t at = 0;
    while (at <= spec.size()) {
        const size_t plus = spec.find('+', at);
        const std::string tok = spec.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
        bool known = false;
        for (const ButtonName& b : BUTTON_NAMES) {
            if (tok == b.name) {
                bits |= b.bit;
                known = true;
                break;
            }
        }
        if (!known) {
            err = "unknown button '" + tok + "'";
            return false;
        }
        if (plus == std::string::npos) break;
        at = plus + 1;
    }
    return true;
}

bool loadScenario(const char* path, int frames, std::vector<PadEvent>& out, std::string& err) {
    FILE* f = fopen(path, "r");
    if (!f) {
        err = std::string("cannot read scenario: ") + path;
        return false;
    }
    char line[256];
    int lineNo = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        lineNo++;
        std::string s(line);
        const size_t hash = s.find('#');
        if (hash != std::string::npos) s.resize(hash);
        // 空白で 2 トークンに割る。sscanf ではなく手で割るのは、ボタン指定が
        // 記号 + 記号の可変長で、書式指定に落とすと空白の扱いが曖昧になるため。
        const size_t b0 = s.find_first_not_of(" \t\r\n");
        if (b0 == std::string::npos) continue;
        const size_t b1 = s.find_first_of(" \t\r\n", b0);
        if (b1 == std::string::npos) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s:%d: expected '<frame> <buttons>'", path, lineNo);
            err = buf;
            ok = false;
            break;
        }
        const size_t b2 = s.find_first_not_of(" \t\r\n", b1);
        const size_t b3 = s.find_last_not_of(" \t\r\n");
        if (b2 == std::string::npos) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s:%d: missing button field", path, lineNo);
            err = buf;
            ok = false;
            break;
        }
        PadEvent ev{};
        const std::string frameTok = s.substr(b0, b1 - b0);
        if (!parseNonNegativeInt(frameTok, ev.frame)) {
            char buf[192];
            snprintf(buf, sizeof(buf), "%s:%d: frame field is not a number ('%s')", path, lineNo, frameTok.c_str());
            err = buf;
            ok = false;
            break;
        }
        std::string berr;
        if (!parseButtons(s.substr(b2, b3 - b2 + 1), ev.bits, berr)) {
            char buf[192];
            snprintf(buf, sizeof(buf), "%s:%d: %s", path, lineNo, berr.c_str());
            err = buf;
            ok = false;
            break;
        }
        // 走らせるフレーム数の外を指すイベントは黙って無視されると、シナリオが
        // 効いていないのに PASS したように見える。書き間違いとして弾く。
        if (ev.frame < 0 || ev.frame >= frames) {
            char buf[160];
            snprintf(buf, sizeof(buf), "%s:%d: frame %d is outside 0..%d", path, lineNo, ev.frame, frames - 1);
            err = buf;
            ok = false;
            break;
        }
        // 昇順であることを要求する。並べ替えて受け付けることもできるが、
        // シナリオは時系列として読まれるものなので、順序が狂っているのは
        // 書き間違いとみなすほうが安全。
        if (!out.empty() && ev.frame < out.back().frame) {
            char buf[160];
            snprintf(buf, sizeof(buf), "%s:%d: frame %d goes backwards (previous was %d)", path, lineNo, ev.frame,
                     out.back().frame);
            err = buf;
            ok = false;
            break;
        }
        out.push_back(ev);
    }
    fclose(f);
    return ok;
}

bool readFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return false;
    }
    out.resize((size_t)size);
    const size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size();
}

// 本体。main から切り出してあるのは、std::vector / std::string を使う以上
// bad_alloc が抜けうるためで、main から例外が出ると terminate になって
// 「何が起きたか」が残らない。下の main が受けてメッセージにする。
int run(int argc, char** argv) {
    const char* USAGE = "usage: %s <rom> [frames] [all|skip4] [dump <f0,f1,...>] [scenario <file>]\n";
    if (argc < 2) {
        fprintf(stderr, USAGE, argv[0]);
        return 2;
    }
    const char* romPath = argv[1];
    // frames も他の数値引数と同じ拒否方針に揃える。atoi 直だと "abc" が 0 に
    // なってループが一度も回らず、何も比較していないのに正常終了して見える。
    // 0 も同じ理由で弾く (フレームを 1 つも走らせない検証に意味がない)。
    int frames = 600;
    if (argc >= 3) {
        if (!parseNonNegativeInt(argv[2], frames) || frames <= 0) {
            fprintf(stderr, "error: frames must be a positive number (got '%s')\n", argv[2]);
            return 2;
        }
    }
    const std::string pattern = argc >= 4 ? argv[3] : "all";

    // dump / scenario は argv[4] 以降にキーワード + 値の対で並ぶ。位置引数では
    // なくキーワードにしているのは、どちらも省略可能で、両方指定する順番に
    // 意味を持たせたくないため。
    //
    // 値の欠けたキーワードは黙って無視しない: 以前は `... all dump` が trace
    // モードに落ちて「差分なし」を報告していた。呼び出し側のタイプミスが
    // 検証の PASS に化けるので、エラー終了させる。
    bool dumpMode = false;
    std::vector<int> dumpFrames;
    const char* scenarioPath = nullptr;
    for (int i = 4; i < argc; i++) {
        const std::string key = argv[i];
        const bool needsValue = key == "dump" || key == "scenario";
        if (!needsValue) {
            fprintf(stderr, "error: unexpected argument '%s'\n", key.c_str());
            fprintf(stderr, USAGE, argv[0]);
            return 2;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "error: '%s' needs a value\n", key.c_str());
            fprintf(stderr, USAGE, argv[0]);
            return 2;
        }
        const std::string value = argv[++i];
        if (key == "scenario") {
            scenarioPath = argv[i];
            continue;
        }
        dumpMode = true;
        size_t at = 0;
        while (at <= value.size()) {
            const size_t comma = value.find(',', at);
            const std::string tok = value.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            int fr = 0;
            if (!parseNonNegativeInt(tok, fr)) {
                fprintf(stderr, "error: dump frame list holds a non-number ('%s')\n", tok.c_str());
                return 2;
            }
            if (fr >= frames) {
                fprintf(stderr, "error: dump frame %d is outside 0..%d\n", fr, frames - 1);
                return 2;
            }
            dumpFrames.push_back(fr);
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        // 実行は 1 パスなので、フレームは昇順でしか吐けない。呼び出し側が
        // どんな順で並べても出力が昇順になるよう、ここで揃えて重複も潰す。
        // 出力の順序規則は冒頭のコメントに明記してある。
        std::sort(dumpFrames.begin(), dumpFrames.end());
        dumpFrames.erase(std::unique(dumpFrames.begin(), dumpFrames.end()), dumpFrames.end());
    }
    if (dumpMode && dumpFrames.empty()) {
        fprintf(stderr, "error: 'dump' needs at least one frame number\n");
        return 2;
    }

    const bool patternKnown = pattern == "all" || pattern == "skip4";
    if (!patternKnown) {
        fprintf(stderr, "error: unknown draw pattern '%s' (expected all|skip4)\n", pattern.c_str());
        return 2;
    }
    // 参照ビルドには renderThisFrame が無く常に描画する。skip4 を黙って all と
    // して走らせると「一致した」という誤った結論になるため、受け付けない。
#ifndef NES_EMBEDDED
    if (pattern == "skip4") {
        fprintf(stderr, "error: skip4 requires the embedded build (-DNES_EMBEDDED)\n");
        return 2;
    }
#endif

    buildCrcTable();
    std::string err;
    if (!buildPaletteInverse(err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 3;
    }

    // ROM を読む前に読む: シナリオの書式エラーは、長い実行を始める前に出したい。
    std::vector<PadEvent> scenario;
    if (scenarioPath && !loadScenario(scenarioPath, frames, scenario, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 3;
    }

    std::vector<uint8_t> rom;
    if (!readFile(romPath, rom)) {
        fprintf(stderr, "error: cannot read ROM: %s\n", romPath);
        return 3;
    }

    // NES はスタックに置くには大きい (framebuffer + OAM + APU バッファ)。
    static nes::NES machine;
    if (!machine.loadRom(rom.data(), rom.size())) {
        fprintf(stderr, "error: unsupported ROM: %s\n", romPath);
        return 3;
    }
    // 44100Hz 固定。サンプルレートは APU の分周器を決めるので、系列間で
    // 揃っていないと sampleCount が食い違う。
    machine.apu.setSampleRate(44100.0);
    machine.powerOn();

    // 既定は無入力。決定性を最優先するため、シナリオが無ければパッドは一切
    // 触らない。シナリオがある場合もフレーム番号で駆動するので、実行は同じく
    // 決定的 — 3 系列すべてに同じシナリオを渡す限り比較は成立する。
    const int divisor = pattern == "skip4" ? 4 : 1;

    // dump は raw バイナリを吐くので、ヘッダは trace のときだけ。
    if (!dumpMode) {
        printf("# rom=%s frames=%d pattern=%s build=%s\n", romPath, frames, pattern.c_str(),
#ifdef NES_EMBEDDED
               "embedded"
#else
               "reference"
#endif
        );
        // 一致判定に使う列 (STATE) と、観測点のずれで動くため参考に留める列
        // (INFO) を行内で分ける。INFO を別扱いにする理由は冒頭の「観測点に
        // ついて」を参照。
        //
        // v (loopy スクロール) と DMC の残バイト数・フレームシーケンサ位置を
        // INFO に置くのは、これらがフレーム内で連続的に進むカウンタで、値が
        // そのまま「フレーム内のどこで止まったか」を表しているため。ここが
        // 違うのは状態の食い違いではなく観測点の違いそのもので、比較しても
        // 意味がない。
        printf("# STATE columns: frame wram oam vram ppu(ctrl mask oamaddr t finex w fc) "
               "apu(p1 p2 tri noi firq dirq) fb\n");
        printf("# INFO columns (not compared): A X Y P SP PC status rdbuf openbus v scanline dot odd dmc step cyc "
               "smp\n");
    }

    size_t nextEvent = 0;
    size_t nextDump = 0;
    for (int frame = 0; frame < frames; frame++) {
        const bool draw = (frame % divisor) == 0;
#ifdef NES_EMBEDDED
        machine.ppu.renderThisFrame = draw;
#endif
        // パッドは runFrame() の前に据える。同じフレーム番号に複数行あっても
        // 最後のものが効くので、書き足しでシナリオを直すときの挙動が素直。
        while (nextEvent < scenario.size() && scenario[nextEvent].frame == frame) {
            machine.pad[0].setButtons(scenario[nextEvent].bits);
            nextEvent++;
        }
        machine.runFrame();

        if (dumpMode) {
            // 目的のフレームに届くまでは走らせるだけ。dumpFrames は昇順なので
            // 1 回の実行で全部を順に吐ける (冒頭「出力モード」の O(n^2) の話)。
            const bool wanted = nextDump < dumpFrames.size() && dumpFrames[nextDump] == frame;
            if (!wanted) {
                machine.apu.sampleCount = 0;
                continue;
            }
            nextDump++;
            if (!draw) {
                fprintf(stderr, "error: frame %d is not drawn under pattern %s\n", frame, pattern.c_str());
                return 4;
            }
            if (!normalizeFramebuffer(machine.ppu)) {
                fprintf(stderr, "error: framebuffer holds a pixel value outside the palette (frame %d)\n", frame);
                return 4;
            }
            fwrite(normalizedFb, 1, sizeof(normalizedFb), stdout);
            machine.apu.sampleCount = 0;
            // 残りが無くなったらそこで終わる。最後の対象フレームより後ろを
            // 走らせても出力は増えないので、時間だけ捨てることになる。
            if (nextDump == dumpFrames.size()) {
                fflush(stdout);
                return 0;
            }
            continue;
        }

        // 音声はサンプル数だけ状態として観測し、中身は捨てる。APU を駆動する
        // のは runFrame() の側なので、ここでの取得は不要 — ただし sampleCount
        // を毎フレーム 0 に戻すのはフロントエンドの役目で、放置すると 2048 で
        // 頭打ちになって列としての意味を失う。実機フロントエンドと同じく
        // 読み出して捨てる。
        const nes::APU::DbgState apu = machine.apu.dbgState();
        machine.apu.sampleCount = 0;

        const nes::PPU::DbgState ppu = machine.ppu.dbgState();
        const nes::CPU& cpu = machine.cpu;

        printf("%05d", frame);
        printf(" wram=%08X", crc32(machine.ram, sizeof(machine.ram)));
        printf(" oam=%08X", crc32(machine.ppu.dbgOam(), 256));
        printf(" vram=%08X", crc32(machine.ppu.dbgVram(), 0x800));
        printf(" ppu=%02X,%02X,%02X,%04X,%X,%d,%u", ppu.ctrl, ppu.mask, ppu.oamAddr, ppu.t, ppu.fineX, ppu.w ? 1 : 0,
               ppu.frameCount);
        printf(" apu=%d,%d,%d,%d,%d,%d", apu.p1len, apu.p2len, apu.triLen, apu.noiseLen, apu.frameIrq ? 1 : 0,
               apu.dmcIrq ? 1 : 0);

        // framebuffer は描画したフレームだけ出す。描画をスキップしたフレームの
        // 内容は「前回描いた絵が残っている」だけで、系列間で比較する意味がない。
        if (draw) {
            if (!normalizeFramebuffer(machine.ppu)) {
                printf("\n");
                fflush(stdout);
                fprintf(stderr, "error: framebuffer holds a pixel value outside the palette (frame %d)\n", frame);
                return 4;
            }
            printf(" fb=%08X", crc32(normalizedFb, sizeof(normalizedFb)));
        } else {
            printf(" fb=-");
        }

        // 以降は参考列。観測点 (フレーム内のどの dot で止まったか) に依存して
        // 動くため一致判定には使わないが、差分が出たときに「どのくらい行き
        // 過ぎたか」を読むのに要るので出力はする。
        printf("  # A=%02X X=%02X Y=%02X P=%02X SP=%02X PC=%04X status=%02X rdbuf=%02X openbus=%02X v=%04X sl=%d "
               "dot=%d odd=%d dmc=%d step=%d cyc=%d smp=%d",
               cpu.a, cpu.x, cpu.y, cpuStatusByte(cpu), cpu.sp, cpu.pc, ppu.status, ppu.readBuffer, ppu.openBus, ppu.v,
               ppu.scanline, ppu.dot, ppu.oddFrame ? 1 : 0, apu.dmcBytes, apu.frameStep, apu.frameCycles,
               apu.sampleCount);
        printf("\n");
    }

    fflush(stdout);
    return 0;
}

}   // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 5;
    }
}
