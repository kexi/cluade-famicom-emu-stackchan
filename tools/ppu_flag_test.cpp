// PPU::frameFullyRendered() の単体テスト。
//
// このフラグはフロントエンド (m5stack/src/main.cpp) の repaint ガードが
// 「いま測った emuD は全面描画フレームの代表値か」を判断するために使う。
// 判定を誤ると、DMA 転送中の framebuffer を書き潰して画面が裂けるか、逆に
// 不要な joinBand ストールが恒常的に張り付くかのどちらかになる。
//
// verify_host.cpp との棲み分け: あちらは実 ROM を流して参照/組み込みの
// bit-exact を見る統合検証で、$2001 を「いつ」書くかは制御できない。この
// フラグのバグは「レンダリングを可視行のどのドットで切り替えたか」でしか
// 現れないので、PPU を直接叩いて書き込みタイミングを作る本テストが要る。
//
// CPU は動かさない。PPU を直接回してドット位置を進め、狙ったドットで
// writeReg($2001) するだけ。ROM を読み込むのは、PPU が可視行の dot 260 で
// mapper->scanline() を呼ぶ (MMC3 の近似 IRQ) ため — マッパーが無いと
// ヌル参照で落ちる。ROM の中身自体はこのテストの主張には効かない。
//
// 2 軸で回す:
//
//   ビルド : 参照 (-DNES_EMBEDDED なし) と組み込み (あり)。両者は framebuffer を
//            書くドットが違う (組み込みは dot 256 の一括描画、参照はドット逐次)
//            が、フラグの述語は「区間 1..256 の全体でレンダリングが有効だったか」
//            なので、どちらでも同じ答えになる
//   駆動   : step() 直叩きと stepMany() (組み込みのみ)。実機は stepMany 経路を
//            通り、そこでは区間が 1 回の step() に畳まれて AND の回数が変わる。
//            step() だけで通したテストは実機経路を一度も踏まない

#include "../core/nes.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
        return;
    }
    std::printf("  FAIL %s\n", what.c_str());
    g_failures++;
}

constexpr uint16_t PPUMASK = 0x2001;
constexpr uint8_t RENDER_ON = 0x18;   // background + sprites enabled
constexpr uint8_t RENDER_OFF = 0x00;

std::vector<uint8_t> g_rom;

// PPU の進め方。実機 (組み込みビルド) は stepMany() でドットをまとめて飛ばす
// ので、step() 直叩きだけで通したテストは coalesce 経路を一度も踏まない。
// frameFullyRendered_ は「描画区間 1..256 の全ドットで AND」という形をしていて、
// stepMany はその区間をまとめて 1 回の step() に畳む — つまり AND の回数が
// 両者で違う。同じ答えになることは設計上の主張なので、テストで踏む。
enum class Driver {
    Step,   // 1 ドットずつ step()
    StepMany,   // stepMany() でまとめて (組み込みビルドのみ)
};

const char* driverName(Driver d) { return d == Driver::Step ? "step" : "stepMany"; }

// PPU を単体で回すための最小の器。CPU は一度も step() しないので、状態は
// 「PPU が自力で進んだぶん」だけになり、テストの主張がドット位置だけで書ける。
struct Harness {
    nes::NES machine;
    Driver driver;

    explicit Harness(Driver d) : driver(d) {
        if (!machine.loadRom(g_rom.data(), g_rom.size())) {
            std::printf("  FAIL could not load rom\n");
            g_failures++;
            return;
        }
        machine.powerOn();
    }

    nes::PPU& ppu() { return machine.ppu; }

    int scanline() { return machine.ppu.dbgState().scanline; }
    int dot() { return machine.ppu.dbgState().dot; }

    // 1 単位進める。stepMany モードでは「行末までの残り」をまとめて渡し、
    // coalesce を実際に働かせる。
    //
    // 行末 (dot 340) を上限にするのは、ここが「飛び越えてはいけない」ドットだから
    // (stepMany 側のコメント参照)。行をまたいで大きな値を渡しても stepMany は
    // 正しく刻むが、テスト側が「いまどの行にいるか」を見失う。行単位で止めれば、
    // 下の advanceTo が目標ドットを通り越さないことを保証できる。
    void advanceOne() {
#ifdef NES_EMBEDDED
        if (driver == Driver::StepMany) {
            const int remaining = 340 - dot();
            machine.ppu.stepMany(remaining > 0 ? remaining : 1);
            return;
        }
#endif
        machine.ppu.step();
    }

    // (scanline, dot) に到達するまで進める。到達不能なら諦めて false。
    //
    // stepMany モードでは目標ドットを飛び越えうるので、残り距離を上限に渡して
    // 刻む。こうすると coalesce は目標の手前までしか働かず、狙ったドットに必ず
    // 着地する — $2001 を「行の描画区間の内側/外側」のどちらで書くかが本テストの
    // 主張そのものなので、着地点がずれると主張が変わってしまう。
    bool advanceTo(int targetScanline, int targetDot) {
        for (int i = 0; i < 262 * 341 * 2; i++) {
            if (scanline() == targetScanline && dot() == targetDot) return true;
#ifdef NES_EMBEDDED
            if (driver == Driver::StepMany && scanline() == targetScanline && dot() < targetDot) {
                machine.ppu.stepMany(targetDot - dot());
                continue;
            }
#endif
            advanceOne();
        }
        return false;
    }

    // 次に frameReady が立つ (= scanline 241 dot 1 を踏む) まで進める。
    // フロントエンドがフラグを読むのと同じ地点。
    bool runToVBlank() {
        machine.ppu.frameReady = false;
        for (int i = 0; i < 262 * 341 * 2; i++) {
            advanceOne();
            if (machine.ppu.frameReady) return true;
        }
        return false;
    }

    void write(uint16_t addr, uint8_t v) { machine.ppu.writeReg(addr, v); }
};

// レンダリングを最初から最後まで有効にしたフレームは fully rendered。
void testFullyRenderedFrame(Driver drv) {
    std::printf("%s [%s]:\n", "full frame with rendering on", driverName(drv));
    Harness h(drv);
    h.write(PPUMASK, RENDER_ON);
    check(h.runToVBlank(), "reached vblank");
    // 1 フレーム目はプリレンダ行が武装する前に始まっている可能性があるので、
    // もう 1 フレーム回してから判定する
    check(h.runToVBlank(), "reached second vblank");
    check(h.ppu().frameFullyRendered(), "frameFullyRendered() is true");
}

// レンダリングを一度も有効にしないフレームは fully rendered ではない。
void testNeverRendered(Driver drv) {
    std::printf("%s [%s]:\n", "full frame with rendering off", driverName(drv));
    Harness h(drv);
    h.write(PPUMASK, RENDER_OFF);
    check(h.runToVBlank(), "reached vblank");
    check(h.runToVBlank(), "reached second vblank");
    check(!h.ppu().frameFullyRendered(), "frameFullyRendered() is false");
}

// 回帰の本体 (1): 可視行を描き切ったあと hblank (dots 257..340) で $2001 を
// 切っても、その行は「描けた行」のまま扱われること。
//
// dot 340 で瞬時の rendering を見ていた旧実装はここで false を記録していた。
// 可視域の末尾で $2001 を落とすゲームでは EWMA が永久にシードされず、ガードが
// 張り付いたままになる。
void testDisableDuringHBlankKeepsLine(Driver drv) {
    std::printf("%s [%s]:\n", "rendering disabled during hblank (dot 300) of the last visible line", driverName(drv));
    Harness h(drv);
    h.write(PPUMASK, RENDER_ON);
    // 1 フレーム分回してプリレンダ行に武装させる
    check(h.runToVBlank(), "reached first vblank");
    check(h.advanceTo(261, 0), "reached pre-render line");
    // 最終可視行 239 を描き切ってから、その行の hblank で切る
    check(h.advanceTo(239, 300), "reached line 239 dot 300");
    h.write(PPUMASK, RENDER_OFF);
    check(h.runToVBlank(), "reached vblank");
    check(h.ppu().frameFullyRendered(), "frameFullyRendered() stays true (line was drawn)");
}

// 回帰の本体 (2): 逆向き。可視行の描画区間 (dots 1..256) の途中で切ったら、
// その行は描き切れていないので fully rendered ではないこと。
void testDisableMidDrawSpanClearsLine(Driver drv) {
    std::printf("%s [%s]:\n", "rendering disabled mid draw span (dot 100) of a visible line", driverName(drv));
    Harness h(drv);
    h.write(PPUMASK, RENDER_ON);
    check(h.runToVBlank(), "reached first vblank");
    check(h.advanceTo(261, 0), "reached pre-render line");
    check(h.advanceTo(100, 100), "reached line 100 dot 100");
    h.write(PPUMASK, RENDER_OFF);
    // 同じ行のうちに戻す。区間の一部が off だったので、行は不成立のまま
    check(h.advanceTo(100, 200), "reached line 100 dot 200");
    h.write(PPUMASK, RENDER_ON);
    check(h.runToVBlank(), "reached vblank");
    check(!h.ppu().frameFullyRendered(), "frameFullyRendered() is false (line was split)");
}

// 可視域が終わったあとの vblank 中に切っても、そのフレームの判定は動かない。
// フロントエンドはフラグを vblank 中に読むので、読む前に値が変わらないことが要る。
void testDisableDuringVBlankDoesNotAffectFrame(Driver drv) {
    std::printf("%s [%s]:\n", "rendering disabled during vblank", driverName(drv));
    Harness h(drv);
    h.write(PPUMASK, RENDER_ON);
    check(h.runToVBlank(), "reached first vblank");
    check(h.advanceTo(261, 0), "reached pre-render line");
    check(h.runToVBlank(), "reached vblank");
    const bool before = h.ppu().frameFullyRendered();
    check(before, "frameFullyRendered() is true at vblank");
    h.write(PPUMASK, RENDER_OFF);
    check(h.advanceTo(250, 10), "advanced further into vblank");
    check(h.ppu().frameFullyRendered() == before, "flag unchanged during vblank");
}

// リセット直後は false。$2001 はゼロなので、まだ何も描けていない。
void testResetState(Driver drv) {
    std::printf("%s [%s]:\n", "state after reset", driverName(drv));
    Harness h(drv);
    check(!h.ppu().frameFullyRendered(), "frameFullyRendered() is false after reset");
}

int run(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: ppu_flag_test <rom.nes>\n");
        return 2;
    }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::printf("ppu_flag_test: cannot open %s\n", argv[1]);
        return 2;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    g_rom.resize((size_t)n);
    const size_t got = std::fread(g_rom.data(), 1, (size_t)n, f);
    std::fclose(f);
    if (got != (size_t)n) {
        std::printf("ppu_flag_test: short read on %s\n", argv[1]);
        return 2;
    }

#ifdef NES_EMBEDDED
    std::printf("=== ppu_flag_test (embedded build) ===\n");
#else
    std::printf("=== ppu_flag_test (reference build) ===\n");
#endif
    // 参照ビルドに stepMany() は無い (NES_EMBEDDED 限定) ので step のみ。
    // 組み込みビルドでは同じケースを両方の駆動で回し、coalesce の有無で
    // frameFullyRendered() の答えが変わらないことを見る。
    std::vector<Driver> drivers{Driver::Step};
#ifdef NES_EMBEDDED
    drivers.push_back(Driver::StepMany);
#endif
    for (const Driver drv : drivers) {
        testResetState(drv);
        testFullyRenderedFrame(drv);
        testNeverRendered(drv);
        testDisableDuringHBlankKeepsLine(drv);
        testDisableMidDrawSpanClearsLine(drv);
        testDisableDuringVBlankDoesNotAffectFrame(drv);
    }

    if (g_failures == 0) {
        std::printf("ppu_flag_test: PASS\n");
        return 0;
    }
    std::printf("ppu_flag_test: FAIL (%d)\n", g_failures);
    return 1;
}

}   // namespace

// verify_host.cpp と同じ体裁: 中身は run() に置き、main は投げないようにする
// (vector の確保などが throw しうるため)
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 5;
    }
}
