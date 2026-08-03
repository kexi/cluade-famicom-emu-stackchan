// USB-C 直結の Nintendo Switch Pro Controller。
//
// ESP-IDF の usb_host スタックを直接叩き、プロコンの HID インターフェースの
// interrupt IN/OUT エンドポイントを借りて生レポートを読む。
//
// Why not 0x30 フルレポート: 0x30 は 6軸 IMU と各種センサを 60Hz で流す本番
// モードで、有効化するのに 0x01 サブコマンド (SET_INPUT_REPORT_MODE) を
// 「レポート ID + タイマ + Rumble ニュートラル 8 バイト + サブコマンド」という
// 手順で組み立て、ACK (0x21) を待つ必要がある。こちらが欲しいのはボタンと
// スティックだけで、それは 0x3F 簡易 HID レポートに全部載っている — 0x3F は
// ハンドシェイク直後の既定モードなので、追加のサブコマンドを一切送らずに
// 届き始める。実装が短いぶん、状態機械が詰まる場所も少ない。
//
// Why not タスク 1 本: usb_host_lib_handle_events() (ライブラリのデーモン) と
// usb_host_client_handle_events() (クライアント) は別々のブロッキング関数で、
// どちらも「呼ばれ続けること」を前提にしている。1 本のタスクで交互にタイム
// アウト付きポーリングにすると、列挙中にライブラリ側の処理が遅れて enumeration
// が失敗する。ESP-IDF のサンプルが 2 本に分けているのはこのため。

#include <M5Unified.h>
#include <atomic>
#include <cstring>

#include "usb/usb_host.h"

#include "config.h"
#include "usb_pad.h"

// ---------------------------------------------------------------- 公開状態

static std::atomic<uint8_t> g_usbPadBits{0};

uint8_t usbPadBits() { return g_usbPadBits.load(std::memory_order_relaxed); }

// ------------------------------------------------------------ 内部の状態機械

// 進行方向は WaitDevice -> Opening -> Claimed -> Handshaking -> Running。
// どこで失敗しても Cleanup へ落ち、そこから WaitDevice へ戻る (抜き差しで
// 何度でもやり直せる)。
enum class PadState : uint8_t {
    WaitDevice,     // 何も挿さっていない / 列挙待ち
    Opening,        // NEW_DEV を受けた: open して VID/PID を確かめる
    Claimed,        // インターフェースを取った: エンドポイントに転送を並べる
    Handshaking,    // 0x80 0x02 を送って 0x81 0x02 を待っている
    Running,        // 0x3F レポートが流れている
    Cleanup,        // 切断 or 失敗: in-flight の完了を待って後始末する
};

static PadState g_state = PadState::WaitDevice;

static usb_host_client_handle_t g_client = nullptr;
static usb_device_handle_t g_device = nullptr;
// コールバックが立て、tick が拾う。open/claim をコールバック内でやらないのは、
// クライアントのイベントコールバックが usb_host_client_handle_events() の
// コンテキストで走るため — そこで長いブロッキング API を呼ぶと、同じ関数が
// 配ろうとしている後続イベントが詰まる。
static std::atomic<uint8_t> g_pendingAddr{0};
static std::atomic<bool> g_deviceGone{false};

static uint8_t g_interfaceNumber = 0;
static bool g_interfaceClaimed = false;
static usb_transfer_t* g_inTransfer = nullptr;
static usb_transfer_t* g_outTransfer = nullptr;
// in-flight カウンタ。Cleanup が release/free に進んでよいかの唯一の判断材料で、
// 完了コールバック (usbpad タスク上で走る) と tick が同じタスクなので、atomic
// でなく素の int で足りる。
static int g_inFlight = 0;

static uint32_t g_handshakeSentMs = 0;
static int g_handshakeTries = 0;

// プロコンの interrupt エンドポイントは wMaxPacketSize=64。転送バッファも同じ
// サイズで確保する。
static constexpr size_t USB_PAD_MPS = 64;

// Nintendo 独自プロトコルのうち、ここで使う 2 つだけ。
static constexpr uint8_t PROCON_CMD_PREFIX = 0x80;
static constexpr uint8_t PROCON_CMD_HANDSHAKE = 0x02;
// 「USB HID として固定・タイムアウト無効」。これを送らないと、ホストからの
// ポーリングが 1 秒ほど途切れただけでコントローラが Bluetooth へ戻ろうとして
// 入力が止まる。
static constexpr uint8_t PROCON_CMD_NO_TIMEOUT = 0x04;
static constexpr uint8_t PROCON_REPLY_PREFIX = 0x81;
// 簡易 HID 入力レポート。ハンドシェイク直後の既定モードで、ボタン・HAT・
// スティックが載っている。
static constexpr uint8_t PROCON_REPORT_SIMPLE = 0x3F;

#if USB_PAD_DEBUG
const char* usbPadDebugStatus() {
    switch (g_state) {
    case PadState::WaitDevice: return "WAIT";
    case PadState::Opening:
    case PadState::Claimed: return "ENUM";
    case PadState::Handshaking: return "HS";
    case PadState::Running: return "RUN";
    case PadState::Cleanup: return "END";
    }
    return "?";
}
#endif

// ------------------------------------------------------------ レポート解釈

// 0x3F レポートの HAT (byte3): 0 = 上、時計回りに 1 ずつ、8 = 中立。
static const uint8_t kHatBits[9] = {
    NES_BTN_UP,                     // 0: 上
    (uint8_t)(NES_BTN_UP | NES_BTN_RIGHT),      // 1: 右上
    NES_BTN_RIGHT,                  // 2: 右
    (uint8_t)(NES_BTN_DOWN | NES_BTN_RIGHT),    // 3: 右下
    NES_BTN_DOWN,                   // 4: 下
    (uint8_t)(NES_BTN_DOWN | NES_BTN_LEFT),     // 5: 左下
    NES_BTN_LEFT,                   // 6: 左
    (uint8_t)(NES_BTN_UP | NES_BTN_LEFT),       // 7: 左上
    0,                              // 8: 中立
};

// 中心 0x8000 の uint16 スティック座標を十字キーのビットに写す。grove_input の
// directionBits() と同じ形 (デッドゾーンを超えた軸だけを立て、両立させる)。
// 値域だけが -128..127 でなく ±0x8000 なので、閾値は USB_PAD_STICK_DEADZONE。
static uint8_t stickBits(uint16_t rawX, uint16_t rawY) {
    int x = (int)rawX - 0x8000;
    int y = (int)rawY - 0x8000;
    if (USB_PAD_INVERT_Y) y = -y;
    uint8_t bits = 0;
    if (x > USB_PAD_STICK_DEADZONE) bits |= NES_BTN_RIGHT;
    else if (x < -USB_PAD_STICK_DEADZONE) bits |= NES_BTN_LEFT;
    if (y > USB_PAD_STICK_DEADZONE) bits |= NES_BTN_UP;
    else if (y < -USB_PAD_STICK_DEADZONE) bits |= NES_BTN_DOWN;
    return bits;
}

// 0x3F 簡易 HID レポート (先頭が report ID = 0x3F):
//   byte1: B(0) A(1) Y(2) X(3) L(4) R(5) ZL(6) ZR(7)
//   byte2: -(0) +(1) LStick(2) RStick(3) Home(4) Capture(5)
//   byte3: HAT (0=上、時計回り、8=中立)
//   byte4-5: 左スティック X (uint16 LE)、byte6-7: 左スティック Y
//
// A/B は 2 つずつ割り当てる: プロコンの A/B は右手の下段・右で、ファミコンの
// A/B (右・左) とは物理配置が合わない。X/Y も同じ役に振っておくと、どちらの
// 持ち替えでも「右側のボタンが A、左側が B」で通る。
static uint8_t parseSimpleReport(const uint8_t* data, int len) {
    const bool tooShort = len < 8;
    if (tooShort) return 0;

    const uint8_t b1 = data[1];
    const uint8_t b2 = data[2];
    uint8_t bits = 0;
    if (b1 & 0x01) bits |= NES_BTN_B;   // B
    if (b1 & 0x02) bits |= NES_BTN_A;   // A
    if (b1 & 0x04) bits |= NES_BTN_B;   // Y -> B と同じ
    if (b1 & 0x08) bits |= NES_BTN_A;   // X -> A と同じ
    if (b2 & 0x01) bits |= NES_BTN_SELECT;  // -
    if (b2 & 0x02) bits |= NES_BTN_START;   // +

    const uint8_t hat = data[3];
    if (hat < 9) bits |= kHatBits[hat];

    const uint16_t rawX = (uint16_t)(data[4] | (data[5] << 8));
    const uint16_t rawY = (uint16_t)(data[6] | (data[7] << 8));
    bits |= stickBits(rawX, rawY);
    return bits;
}

// ------------------------------------------------------------ 転送まわり

static void inTransferCb(usb_transfer_t* transfer);
static void outTransferCb(usb_transfer_t* transfer);

// IN を 1 本投げ直す。失敗したら in-flight を増やさずに Cleanup へ倒す
// (増やしたまま失敗すると、来ない完了を Cleanup が待ち続ける)。
static bool submitIn() {
    g_inTransfer->num_bytes = USB_PAD_MPS;
    const esp_err_t err = usb_host_transfer_submit(g_inTransfer);
    if (err != ESP_OK) return false;
    g_inFlight++;
    return true;
}

// 2 バイトのコマンドを OUT に流す。プロコンの USB コマンドは全部この形
// (0x80 + サブコマンド) なので、可変長にする理由がない。
static bool sendCommand(uint8_t cmd) {
    const bool busy = g_outTransfer == nullptr;
    if (busy) return false;
    g_outTransfer->data_buffer[0] = PROCON_CMD_PREFIX;
    g_outTransfer->data_buffer[1] = cmd;
    g_outTransfer->num_bytes = 2;
    const esp_err_t err = usb_host_transfer_submit(g_outTransfer);
    if (err != ESP_OK) return false;
    g_inFlight++;
    return true;
}

static void inTransferCb(usb_transfer_t* transfer) {
    g_inFlight--;
    // エラーで戻ったら resubmit しない。切断 (NO_DEVICE) も STALL も、後始末は
    // Cleanup の仕事 — ここで投げ直すと転送が終わらず、Cleanup が
    // usb_host_interface_release() に進めない。DEV_GONE が来ない失敗 (STALL や
    // ERROR) もあるので、ここから Cleanup へ倒しておかないとストリームが
    // 黙って止まったまま復帰しなくなる。
    const bool failed = transfer->status != USB_TRANSFER_STATUS_COMPLETED;
    if (failed) {
        g_usbPadBits.store(0, std::memory_order_relaxed);
        g_state = PadState::Cleanup;
        return;
    }
    // Cleanup に入ったあとに届いた完了は、状態機械の外の残響。読み捨てる。
    const bool stale = g_state != PadState::Handshaking && g_state != PadState::Running;
    if (stale) return;

    const uint8_t* data = transfer->data_buffer;
    const int len = transfer->actual_num_bytes;
    const bool hasReport = len >= 1;
    if (hasReport) {
        if (data[0] == PROCON_REPORT_SIMPLE) {
            // 再接続時はコントローラ側がハンドシェイク済みのまま覚えていて、
            // 0x81 応答を返さずにいきなりレポートを流し始めることがある。届いた
            // 時点で通信は成立しているので、待たずに Running へ上げる。
            g_state = PadState::Running;
            g_usbPadBits.store(parseSimpleReport(data, len), std::memory_order_relaxed);
        } else if (len >= 2 && data[0] == PROCON_REPLY_PREFIX && data[1] == PROCON_CMD_HANDSHAKE) {
            // ハンドシェイク成立。続けてタイムアウト無効を送ってから Running へ。
            sendCommand(PROCON_CMD_NO_TIMEOUT);
            g_state = PadState::Running;
        }
        // それ以外 (0x81 0x01 の状態応答など) は無視して読み続ける。
    }

    // 投げ直しに失敗したらそこで読みが途絶える。IN が 1 本も飛んでいない状態で
    // Running に留まると、抜き差ししない限り二度と入力が戻らないので Cleanup へ。
    const bool resubmitted = submitIn();
    if (!resubmitted) {
        g_usbPadBits.store(0, std::memory_order_relaxed);
        g_state = PadState::Cleanup;
    }
}

static void outTransferCb(usb_transfer_t* transfer) {
    g_inFlight--;
    (void)transfer;
    // 送りっぱなしでよい: 応答は IN 側に来るので、ここで見るべき結果がない。
    // 失敗しても Handshaking のリトライタイマが拾う。
}

// ------------------------------------------------------------ 列挙と後始末

static void clientEventCb(const usb_host_client_event_msg_t* msg, void* /*arg*/) {
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        // アドレスをラッチするだけ。open も claim も tick 側でやる (この
        // コールバックは usb_host_client_handle_events() の中で走っている)。
        g_pendingAddr.store(msg->new_dev.address, std::memory_order_relaxed);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        // 真っ先にボタンを離す。後始末より優先するのは、抜いた瞬間に押していた
        // 方向やボタンが押しっぱなしで残るのが一番まずい壊れ方だから — マリオが
        // 走り続けて穴に落ちる。ハンドルの解放は tick が順番を守って行う。
        g_usbPadBits.store(0, std::memory_order_relaxed);
        g_deviceGone.store(true, std::memory_order_relaxed);
        break;
    }
}

// 開いたデバイスの active config を舐めて、HID インターフェースと interrupt
// IN/OUT エンドポイントを拾う。見つからなければ false。
static bool findHidEndpoints(uint8_t* inEp, uint8_t* outEp) {
    const usb_config_desc_t* cfg = nullptr;
    const esp_err_t err = usb_host_get_active_config_descriptor(g_device, &cfg);
    if (err != ESP_OK || cfg == nullptr) return false;

    bool inInterface = false;
    bool haveIn = false, haveOut = false;
    const uint8_t* p = (const uint8_t*)cfg;
    const uint8_t* end = p + cfg->wTotalLength;
    while (p + 2 <= end) {
        const uint8_t bLength = p[0];
        const bool malformed = bLength < 2 || p + bLength > end;
        if (malformed) break;
        const uint8_t bDescriptorType = p[1];

        if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE && bLength >= USB_INTF_DESC_SIZE) {
            const usb_intf_desc_t* intf = (const usb_intf_desc_t*)p;
            // 揃った時点で打ち切る。エンドポイントはインターフェースに属する
            // ので、境界を越えて拾い続けると別機能のものを掴んでしまう。
            const bool complete = inInterface && haveIn && haveOut;
            if (complete) break;
            // 揃わないまま次のインターフェースに入ったら、そこまでの収穫は
            // 捨てて仕切り直す (IN だけあって OUT が無い HID の残骸を、次の
            // インターフェースの OUT と混ぜないため)。
            inInterface = intf->bInterfaceClass == USB_CLASS_HID && intf->bAlternateSetting == 0;
            haveIn = false;
            haveOut = false;
            if (inInterface) g_interfaceNumber = intf->bInterfaceNumber;
        } else if (inInterface && bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                   bLength >= USB_EP_DESC_SIZE) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)p;
            const bool isInterrupt = USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_INTR;
            if (isInterrupt) {
                const bool isIn = USB_EP_DESC_GET_EP_DIR(ep) != 0;
                if (isIn && !haveIn) {
                    *inEp = ep->bEndpointAddress;
                    haveIn = true;
                } else if (!isIn && !haveOut) {
                    *outEp = ep->bEndpointAddress;
                    haveOut = true;
                }
            }
        }
        p += bLength;
    }
    return haveIn && haveOut;
}

// 掴んだものを掴んだ順の逆にほどく。順序を守らないと ESP_ERR_INVALID_STATE で
// 弾かれ、ハンドルが宙に浮いたまま二度と再接続できなくなる:
//   in-flight の完了を待つ -> interface_release -> transfer_free -> device_close
static void releaseDevice() {
    if (g_interfaceClaimed) {
        usb_host_interface_release(g_client, g_device, g_interfaceNumber);
        g_interfaceClaimed = false;
    }
    if (g_inTransfer) {
        usb_host_transfer_free(g_inTransfer);
        g_inTransfer = nullptr;
    }
    if (g_outTransfer) {
        usb_host_transfer_free(g_outTransfer);
        g_outTransfer = nullptr;
    }
    if (g_device) {
        usb_host_device_close(g_client, g_device);
        g_device = nullptr;
    }
    g_usbPadBits.store(0, std::memory_order_relaxed);
    g_handshakeTries = 0;
    g_inFlight = 0;
}

// 何も掴んでいない間。NEW_DEV のラッチを見るのが本線だが、それだけだと
// 「列挙は済んでいるのにこちらが取りこぼした」状態から抜けられない —
// ハンドシェイク切れや STALL で Cleanup した直後がまさにそれで、デバイスは
// 挿さったままなので NEW_DEV はもう来ない。Grove の再プローブと同じ発想で、
// 1 秒おきに列挙済みデバイスの一覧を引いて拾い直す。
static void tickWaitDevice() {
    const bool latched = g_pendingAddr.load(std::memory_order_relaxed) != 0;
    if (latched) {
        g_state = PadState::Opening;
        return;
    }

    static uint32_t lastScanMs = 0;
    const uint32_t now = millis();
    const bool tooSoon = now - lastScanMs < USB_PAD_RESCAN_MS;
    if (tooSoon) return;
    lastScanMs = now;

    uint8_t addrs[USB_PAD_MAX_DEVICES];
    int found = 0;
    const esp_err_t err = usb_host_device_addr_list_fill(USB_PAD_MAX_DEVICES, addrs, &found);
    if (err != ESP_OK || found < 1) return;
    // 先頭 1 台だけ見る。対象外なら tickOpening() が close して戻すので、
    // 次の走査でも同じ相手を試し続けることになるが、ハブ非対応の前提では
    // 「挿さっているのは 1 台」が普通で、総当たりの複雑さに見合わない。
    g_pendingAddr.store(addrs[0], std::memory_order_relaxed);
    g_state = PadState::Opening;
}

// open して VID/PID を確かめるところまで。対象外なら close して WaitDevice へ
// 戻る (それはハブでもストレージでもよく、こちらの知ったことではない)。
static void tickOpening() {
    const uint8_t addr = g_pendingAddr.exchange(0, std::memory_order_relaxed);
    const bool nothingPending = addr == 0;
    if (nothingPending) {
        g_state = PadState::WaitDevice;
        return;
    }

    const esp_err_t opened = usb_host_device_open(g_client, addr, &g_device);
    if (opened != ESP_OK) {
        g_device = nullptr;
        g_state = PadState::WaitDevice;
        return;
    }

    const usb_device_desc_t* desc = nullptr;
    const esp_err_t gotDesc = usb_host_get_device_descriptor(g_device, &desc);
    if (gotDesc != ESP_OK || desc == nullptr) {
        usb_host_device_close(g_client, g_device);
        g_device = nullptr;
        g_state = PadState::WaitDevice;
        return;
    }

    // 他のコントローラに広げるならここが分岐点: VID/PID の表を引いて、
    // 「どのレポートパーサを使うか」を決める種別を持たせればよい。今は
    // プロコン 1 種なので定数比較のまま置く。
    const bool isProCon = desc->idVendor == USB_PAD_VID && desc->idProduct == USB_PAD_PID;
    if (!isProCon) {
        usb_host_device_close(g_client, g_device);
        g_device = nullptr;
        g_state = PadState::WaitDevice;
        return;
    }

    g_state = PadState::Claimed;
}

// インターフェースを取り、転送を 2 本確保して IN を投げ、ハンドシェイクを開始
// する。ここまで来れば以降はコールバック駆動。
static void tickClaimed() {
    uint8_t inEp = 0, outEp = 0;
    const bool found = findHidEndpoints(&inEp, &outEp);
    if (!found) {
        g_state = PadState::Cleanup;
        return;
    }

    const esp_err_t claimed = usb_host_interface_claim(g_client, g_device, g_interfaceNumber, 0);
    if (claimed != ESP_OK) {
        g_state = PadState::Cleanup;
        return;
    }
    g_interfaceClaimed = true;

    const esp_err_t allocIn = usb_host_transfer_alloc(USB_PAD_MPS, 0, &g_inTransfer);
    const esp_err_t allocOut = usb_host_transfer_alloc(USB_PAD_MPS, 0, &g_outTransfer);
    if (allocIn != ESP_OK || allocOut != ESP_OK) {
        g_state = PadState::Cleanup;
        return;
    }

    g_inTransfer->device_handle = g_device;
    g_inTransfer->bEndpointAddress = inEp;
    g_inTransfer->callback = inTransferCb;
    g_inTransfer->context = nullptr;
    g_inTransfer->timeout_ms = 0;

    g_outTransfer->device_handle = g_device;
    g_outTransfer->bEndpointAddress = outEp;
    g_outTransfer->callback = outTransferCb;
    g_outTransfer->context = nullptr;
    g_outTransfer->timeout_ms = 0;

    // IN を先に並べてから OUT を送る: 逆にすると、応答が届く窓が開く前に
    // コマンドが出てしまい、最初の 0x81 を取りこぼす。
    if (!submitIn()) {
        g_state = PadState::Cleanup;
        return;
    }
    g_handshakeTries = 1;
    g_handshakeSentMs = millis();
    sendCommand(PROCON_CMD_HANDSHAKE);
    g_state = PadState::Handshaking;
}

// 応答が来ないぶんだけ 0x80 0x02 を再送する。コールドスタートのプロコンは
// 挿してすぐには返事をしないので、1 回で諦めない。
static void tickHandshaking() {
    const uint32_t waited = millis() - g_handshakeSentMs;
    const bool stillWaiting = waited < USB_PAD_HANDSHAKE_RETRY_MS;
    if (stillWaiting) return;

    const bool exhausted = g_handshakeTries >= USB_PAD_HANDSHAKE_RETRIES;
    if (exhausted) {
        // 応答しない個体・別物だった、のどちらか。抜き差しからやり直せるよう
        // ハンドルを全部返す。
        g_state = PadState::Cleanup;
        return;
    }
    g_handshakeTries++;
    g_handshakeSentMs = millis();
    sendCommand(PROCON_CMD_HANDSHAKE);
}

// in-flight がゼロになるまで待ってから解放する。切断時の転送は
// USB_TRANSFER_STATUS_NO_DEVICE で必ず戻ってくるので、待てば必ず抜ける。
static void tickCleanup() {
    const bool transfersPending = g_inFlight > 0;
    if (transfersPending) return;
    releaseDevice();
    g_state = PadState::WaitDevice;
}

// ------------------------------------------------------------------- タスク

// USB ホストライブラリのデーモン。列挙・切断の実務はこの中で進む。
static void usbLibTask(void*) {
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        // Why not USB_HOST_LIB_EVENT_FLAGS_ALL_FREE を見て uninstall する:
        // このファームは電源が切れるまで USB ホストを畳まない。畳んだところで
        // シリアルが戻るわけでもなく (PHY の付け替えには再起動が要る)、
        // uninstall の途中で抜き差しが起きる経路を増やすだけ。
    }
}

// クライアントのイベント配送 + 状態機械の tick。
static void usbPadTask(void*) {
    for (;;) {
        // 1ms でタイムアウトさせるのは、イベントが無くても tick を回すため
        // (ハンドシェイクの再送タイマと Cleanup の待ちはイベント駆動ではない)。
        usb_host_client_handle_events(g_client, pdMS_TO_TICKS(1));

        // 切断はどの状態から来ても Cleanup が引き受ける。判定を tick の頭に
        // 置くのは、Opening/Claimed の途中で抜かれた場合も同じ 1 本の道に
        // 合流させるため。
        const bool gone = g_deviceGone.exchange(false, std::memory_order_relaxed);
        if (gone) g_state = PadState::Cleanup;

        switch (g_state) {
        case PadState::WaitDevice: tickWaitDevice(); break;
        case PadState::Opening: tickOpening(); break;
        case PadState::Claimed: tickClaimed(); break;
        case PadState::Handshaking: tickHandshaking(); break;
        case PadState::Running: break;   // 以降はコールバックが回す
        case PadState::Cleanup: tickCleanup(); break;
        }
    }
}

// --------------------------------------------------------------------- 初期化

void usbPadInit() {
    // HWCDC の送信待ちを無効化する。この直後 usb_host_install() が PHY を
    // ホスト側へ持っていくと、CDC は永久に排出されないバッファを抱えるので、
    // タイムアウトが残っていると以降の Serial.printf が丸ごとブロックして
    // フレームループが止まる。ログは出ないが、止まりはしない状態にしておく。
    Serial.setTxTimeoutMs(0);

    usb_host_config_t hostCfg = {};
    // 内部 PHY をライブラリに設定させる (外部 PHY は載っていない)。
    hostCfg.skip_phy_setup = false;
    // Why not 0: intr_flags=0 だと空きの割り当てレベルによっては install が
    // ESP_ERR_NOT_FOUND で落ちることがある。LEVEL1 は最も低い優先度の割り込みで、
    // 明示しておくと確保に失敗しない。
    hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    const esp_err_t installed = usb_host_install(&hostCfg);
    if (installed != ESP_OK) return;

    usb_host_client_config_t clientCfg = {};
    // 非同期 (コールバック) 構成。同期構成はライブラリが「今のところ false に
    // せよ」としている。
    clientCfg.is_synchronous = false;
    clientCfg.max_num_event_msg = 5;
    clientCfg.async.client_event_callback = clientEventCb;
    clientCfg.async.callback_arg = nullptr;
    const esp_err_t registered = usb_host_client_register(&clientCfg, &g_client);
    if (registered != ESP_OK) return;

    // core 0 に固定。Grove と同じ理由で、入力のポーリングをエミュレーションの
    // core 1 に載せない。
    xTaskCreatePinnedToCore(usbLibTask, "usbh", USB_LIB_TASK_STACK, nullptr, USB_LIB_TASK_PRIO, nullptr, 0);
    xTaskCreatePinnedToCore(usbPadTask, "usbpad", USB_PAD_TASK_STACK, nullptr, USB_PAD_TASK_PRIO, nullptr, 0);
}
