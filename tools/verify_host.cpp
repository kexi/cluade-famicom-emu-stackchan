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
// 領域の判定と、なぜハーネス側で潰さないのかは fbDiffInKnownWindow() を参照。
//
// ---- 出力モード ----
//
//   <rom> [frames] [all|skip4]
//       1 フレーム 1 行のテキスト。CRC で全フレームを一気に比較する
//   <rom> [frames] [all|skip4] dump <frame>
//       指定フレームの framebuffer をパレット index 列 (61440 byte) として
//       stdout へ raw 出力する
//
// dump を分けているのは出力サイズとの折り合い。全フレームの index 列を出すと
// 600 フレームで 36MB になる一方、CRC が食い違うのは一部のフレームだけなので、
// justfile 側は「まず trace で CRC 比較 → 食い違ったフレームだけ dump で
// 引き直してピクセル位置を特定する」2 パスで動かす。

#include "../core/nes.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

// ピクセル値 → canonical index。線形探索で足りるのは 54 要素しかないため。
// 未知の値は正規化できないので -1 を返し、呼び出し側が異常終了する。
int canonicalIndexOf(nes::Pixel px) {
    for (size_t k = 0; k < canonicalPixels.size(); k++) {
        if (canonicalPixels[k] == px) return (int)k;
    }
    return -1;
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
    for (int i = 0; i < PALETTE_SIZE; i++) {
        const nes::Pixel px = palettePixel(i);
        if (canonicalIndexOf(px) < 0) canonicalPixels.push_back(px);
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

}   // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom> [frames] [all|skip4] [dump <frame>]\n", argv[0]);
        return 2;
    }
    const char* romPath = argv[1];
    const int frames = argc >= 3 ? atoi(argv[2]) : 600;
    const std::string pattern = argc >= 4 ? argv[3] : "all";

    // dump モード: 指定フレームまで走らせ、その framebuffer を index 列で吐く。
    // frames と pattern はそのまま効くので、trace で差分の出たフレームを同じ
    // 描画パターンのまま引き直せる。
    bool dumpMode = false;
    int dumpFrame = -1;
    if (argc >= 6 && std::string(argv[4]) == "dump") {
        dumpMode = true;
        dumpFrame = atoi(argv[5]);
        if (dumpFrame < 0 || dumpFrame >= frames) {
            fprintf(stderr, "error: dump frame %d is outside 0..%d\n", dumpFrame, frames - 1);
            return 2;
        }
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

    // 入力は無入力固定。決定性を最優先するため、パッドは一切触らない。
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

    for (int frame = 0; frame < frames; frame++) {
        const bool draw = (frame % divisor) == 0;
#ifdef NES_EMBEDDED
        machine.ppu.renderThisFrame = draw;
#endif
        machine.runFrame();

        if (dumpMode) {
            // 目的のフレームに届くまでは走らせるだけ。到達したらその画面を
            // index 列で吐いて終わる。
            if (frame != dumpFrame) {
                machine.apu.sampleCount = 0;
                continue;
            }
            if (!draw) {
                fprintf(stderr, "error: frame %d is not drawn under pattern %s\n", dumpFrame, pattern.c_str());
                return 4;
            }
            if (!normalizeFramebuffer(machine.ppu)) {
                fprintf(stderr, "error: framebuffer holds a pixel value outside the palette (frame %d)\n", frame);
                return 4;
            }
            fwrite(normalizedFb, 1, sizeof(normalizedFb), stdout);
            fflush(stdout);
            return 0;
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
