#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = [
#     "pygame>=2.5",
#     "hidapi",
# ]
# ///
"""Send Switch Pro Controller button state to a NES emulator over UDP.

Reads a Switch Pro Controller (USB or Bluetooth) through pygame's joystick
layer and broadcasts an 8-byte packet to the M5Stack CoreS3 running the
emulator. SDL2's GameController database normalises USB and BT button
numbering, so the same mapping table works for both transports.

Usage:
    uv run tools/procon_udp.py [--host 192.168.x.x] [--port 5555] [--rate 120]
    uv run tools/procon_udp.py --list
"""

import argparse
import os
import socket
import struct
import sys
import time

# Suppress the "Hello from the pygame community" banner before importing pygame.
os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")

import pygame  # noqa: E402  (must follow the env var above)

try:
    import hid  # provided by the "hidapi" package
except ImportError:  # pragma: no cover - only when deps are installed by hand
    hid = None


# ----------------------------------------------------------------- protocol

PROTOCOL_MAGIC = b"NP"
PROTOCOL_VERSION = 1
PACKET_SIZE = 8
DEFAULT_PORT = 5555
DEFAULT_HOST = "255.255.255.255"
DEFAULT_RATE_HZ = 120

# NES controller bit assignments, matching the emulator's shift register order
# (see web/main.js: bit0:A 1:B 2:Select 3:Start 4:Up 5:Down 6:Left 7:Right).
NES_A = 0x01
NES_B = 0x02
NES_SELECT = 0x04
NES_START = 0x08
NES_UP = 0x10
NES_DOWN = 0x20
NES_LEFT = 0x40
NES_RIGHT = 0x80

BIT_NAMES = [
    (NES_A, "A"),
    (NES_B, "B"),
    (NES_SELECT, "Sel"),
    (NES_START, "Sta"),
    (NES_UP, "Up"),
    (NES_DOWN, "Dn"),
    (NES_LEFT, "Lt"),
    (NES_RIGHT, "Rt"),
]


# ------------------------------------------------------------ button mapping

# SDL GameController button indices as reported for the Switch Pro Controller.
# Physical layout: 0=B(bottom) 1=A(right) 2=Y(left) 3=X(top).
# The Pro Controller's A sits where a NES A does, so A->A and B->B; Y is
# additionally mapped to NES B so players can rest a second finger on it for
# rapid fire without re-gripping the pad.
BUTTON_MAP = {
    1: NES_A,  # A (right)
    0: NES_B,  # B (bottom)
    2: NES_B,  # Y (left) -- duplicate binding for rapid fire
    6: NES_SELECT,  # minus
    7: NES_START,  # plus
}

# HOME is deliberately absent from this map, so the SDL backend cannot open the
# menu the way the hidapi one does.
#
# Why not just add an index: the available evidence contradicts itself, and none
# of it was gathered by pressing HOME. The map above is a raw
# `pygame.joystick.Joystick` layout, not SDL_GameController's. SDL 2.28's HIDAPI
# Switch driver would put HOME at 5 (it passes the GameController enum straight
# through as the raw index, with A=0 and minus=4), while the two macOS IOKit rows
# in SDL_GameControllerDB put it at 9 and at 12. The verified entries above
# (minus=6, plus=7) match none of the three.
#
# The pad here reports buttons=20 / hats=0, which is the HIDAPI driver's
# signature — yet that driver's layout disagrees with the very entries this table
# was built from on real hardware. Until someone presses HOME and reads the index
# back, that conflict is unresolved, and the index also shifts between USB and
# Bluetooth (distinct GUIDs) and across controller firmware. Worse, 12 collides
# with the D-pad on the builds that report it as buttons 11-14 just below.
#
# A wrong guess costs more than the missing feature: the misread button is one a
# player holds during normal play, so it would drop them out of a running game at
# random. A HOME that only works over USB is a documented limitation; a D-pad
# press that quits to the menu is a bug nobody would connect to this table. Use
# --backend hid for HOME, or open the picker with a long press on BtnC.
#
# Some SDL builds expose the D-pad as buttons 11-14 instead of a hat.
# Used only when the device reports no hats at all.
DPAD_BUTTON_MAP = {
    11: NES_UP,
    12: NES_DOWN,
    13: NES_LEFT,
    14: NES_RIGHT,
}

# Left analog stick doubles as the D-pad.
AXIS_LEFT_X = 0
AXIS_LEFT_Y = 1
AXIS_THRESHOLD = 0.5


# ------------------------------------------------------- hidapi backend (USB)

# Direct-HID fallback for when SDL's Switch driver loses its handshake race
# against macOS's gamecontrollerd. Report layout follows dekuNukem's
# Nintendo_Switch_Reverse_Engineering notes.
PROCON_VENDOR_ID = 0x057E
PROCON_PRODUCT_ID = 0x2009

REPORT_ID_STANDARD = 0x30  # full input report, streamed continuously
REPORT_ID_SUBCMD_REPLY = 0x21
USB_CMD_HANDSHAKE = 0x02
USB_CMD_FORCE_FULL_MODE = 0x04
SUBCMD_SET_INPUT_REPORT_MODE = 0x03

# Byte 4 bit7 is the charging-grip flag, not a button; it reads as 1 forever on
# a USB-connected pad and would otherwise look like a stuck press.
SHARED_BYTE_BUTTON_MASK = 0x3F

# Bit positions within the 0x30 report's three button bytes (3=right, 4=shared,
# 5=left). Only the buttons the NES needs are listed.
HID_RIGHT_BUTTONS = {
    0x08: NES_A,  # A
    0x04: NES_B,  # B
    0x01: NES_B,  # Y -- duplicate binding for rapid fire, matches BUTTON_MAP
}
HID_SHARED_BUTTONS = {
    0x01: NES_SELECT,  # minus
    0x02: NES_START,  # plus
}
# HOME は NES のパッドビットに居場所がないので、pad1 ではなく UDP type 2 の
# 「メニューを開く」制御として別送りする。
HID_SHARED_HOME = 0x10
HID_LEFT_BUTTONS = {
    0x02: NES_UP,
    0x01: NES_DOWN,
    0x08: NES_LEFT,
    0x04: NES_RIGHT,
}

# The left stick is 12-bit packed; centre sits near 2048 and full deflection is
# roughly +/-1400, so 500 is a comfortable equivalent of the 0.5 axis threshold.
STICK_CENTER = 2048
STICK_THRESHOLD = 500


def decode_standard_report(report):
    """Turn one 0x30 input report into a (NES button byte, HOME held) pair."""
    is_usable = len(report) >= 12 and report[0] == REPORT_ID_STANDARD
    if not is_usable:
        return None

    right = report[3]
    shared = report[4] & SHARED_BYTE_BUTTON_MASK
    left = report[5]

    pad1 = 0
    for mask, nes_bit in HID_RIGHT_BUTTONS.items():
        if right & mask:
            pad1 |= nes_bit
    for mask, nes_bit in HID_SHARED_BUTTONS.items():
        if shared & mask:
            pad1 |= nes_bit
    for mask, nes_bit in HID_LEFT_BUTTONS.items():
        if left & mask:
            pad1 |= nes_bit

    home_held = bool(shared & HID_SHARED_HOME)

    # Left stick is additive with the D-pad, mirroring the SDL backend.
    stick_x = report[6] | ((report[7] & 0x0F) << 8)
    stick_y = (report[7] >> 4) | (report[8] << 4)
    if stick_x < STICK_CENTER - STICK_THRESHOLD:
        pad1 |= NES_LEFT
    if stick_x > STICK_CENTER + STICK_THRESHOLD:
        pad1 |= NES_RIGHT
    # Stick Y grows upward, the opposite of SDL's axis convention.
    if stick_y > STICK_CENTER + STICK_THRESHOLD:
        pad1 |= NES_UP
    if stick_y < STICK_CENTER - STICK_THRESHOLD:
        pad1 |= NES_DOWN

    return pad1, home_held


class HidProController:
    """Reads a USB Pro Controller directly, bypassing SDL entirely."""

    def __init__(self):
        self.device = hid.device()
        self.device.open(PROCON_VENDOR_ID, PROCON_PRODUCT_ID)
        self.device.set_nonblocking(1)
        self.packet_counter = 0
        self.last_pad1 = 0
        self.last_home = False
        self.name = "Nintendo Switch Pro Controller (hidapi/USB)"

    def _send_subcommand(self, subcommand, argument):
        # Rumble payload is mandatory filler; the controller ignores a subcommand
        # sent without it. The low nibble counter must advance per packet.
        #
        # 整形を止めているのは、中央の 8 バイトが左右ランブルの 4 バイト × 2 だから。
        # ruff format に任せると 1 バイト 1 行に展開され、この対称性が読めなくなる
        # fmt: off
        packet = bytes(
            [
                0x01,
                self.packet_counter & 0x0F,
                0x00, 0x01, 0x40, 0x40,  # 左ランブル (ニュートラル)
                0x00, 0x01, 0x40, 0x40,  # 右ランブル (ニュートラル)
                subcommand,
                argument,
            ]
        )
        # fmt: on
        self.packet_counter += 1
        self.device.write(packet + b"\x00" * (64 - len(packet)))

    def start(self):
        """Run the USB handshake and switch the pad into standard report mode.

        Without this the controller stays silent: it sends no input reports at
        all until the host completes the handshake.
        """
        self.device.write(bytes([0x80, USB_CMD_HANDSHAKE]))
        time.sleep(0.15)
        self.device.read(64)
        self.device.write(bytes([0x80, USB_CMD_FORCE_FULL_MODE]))
        time.sleep(0.15)
        self._send_subcommand(SUBCMD_SET_INPUT_REPORT_MODE, REPORT_ID_STANDARD)
        time.sleep(0.3)

        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            report = self.device.read(64)
            is_input_report = report and report[0] == REPORT_ID_STANDARD
            if is_input_report:
                return True
            time.sleep(0.002)
        return False

    def read_buttons(self):
        """Return the newest button state, reusing the last one when idle.

        The pad streams ~85 Hz while we poll at 120 Hz, so most ticks legitimately
        have no fresh report to consume.
        """
        while True:
            report = self.device.read(64)
            if not report:
                return self.last_pad1, self.last_home
            decoded = decode_standard_report(report)
            if decoded is not None:
                self.last_pad1, self.last_home = decoded

    def close(self):
        self.device.close()


# ------------------------------------------------------------------- packet


def build_packet(seq, pad1, pad2=0):
    """Pack one 8-byte controller frame.

    Layout: magic 'NP' | version | reserved | seq (uint16 LE) | pad1 | pad2
    """
    return struct.pack(
        "<2sBBHBB",
        PROTOCOL_MAGIC,
        PROTOCOL_VERSION,
        0,  # reserved
        seq & 0xFFFF,
        pad1 & 0xFF,
        pad2 & 0xFF,
    )


# UDP type 2 (control) の「メニューを開く」ビット。config.h の UDP_CTRL_MENU と
# 対で、ゲーム中のみ意味を持つ (メニュー表示中は実機側で読み捨てられる)。
TYPE_CTRL = 2
CTRL_MENU = 0x04


def build_menu_packet(seq):
    """Pack a type 2 control frame asking the device to open the ROM picker."""
    return struct.pack(
        "<2sBBHBB",
        PROTOCOL_MAGIC,
        PROTOCOL_VERSION,
        TYPE_CTRL,
        seq & 0xFFFF,
        CTRL_MENU,
        0,
    )


def make_socket(host):
    """Create the UDP socket, enabling broadcast when the target needs it."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    is_broadcast = host.endswith(".255") or host == "255.255.255.255"
    if is_broadcast:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    return sock


class PacketSender:
    """Owns the socket and the rolling sequence number."""

    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = make_socket(host)
        self.seq = 0

    def send(self, pad1, pad2=0):
        packet = build_packet(self.seq, pad1, pad2)
        self.seq = (self.seq + 1) & 0xFFFF
        self.sock.sendto(packet, (self.host, self.port))
        return packet

    def send_menu(self):
        packet = build_menu_packet(self.seq)
        self.seq = (self.seq + 1) & 0xFFFF
        self.sock.sendto(packet, (self.host, self.port))
        return packet

    def close(self):
        self.sock.close()


def format_bits(pad1):
    """Render a button byte as a compact one-line label for logging."""
    pressed = [name for bit, name in BIT_NAMES if pad1 & bit]
    if not pressed:
        return "----"
    return " ".join(pressed)


# -------------------------------------------------------------- joystick IO


def read_buttons(joystick):
    """Collapse the joystick's current state into a NES button byte."""
    pad1 = 0

    for index, nes_bit in BUTTON_MAP.items():
        has_button = index < joystick.get_numbuttons()
        if has_button and joystick.get_button(index):
            pad1 |= nes_bit

    # D-pad: prefer the hat, fall back to buttons on SDL builds without one.
    has_hat = joystick.get_numhats() > 0
    if has_hat:
        hat_x, hat_y = joystick.get_hat(0)
        if hat_y > 0:
            pad1 |= NES_UP
        if hat_y < 0:
            pad1 |= NES_DOWN
        if hat_x < 0:
            pad1 |= NES_LEFT
        if hat_x > 0:
            pad1 |= NES_RIGHT
    else:
        for index, nes_bit in DPAD_BUTTON_MAP.items():
            has_button = index < joystick.get_numbuttons()
            if has_button and joystick.get_button(index):
                pad1 |= nes_bit

    # Left stick is additive with the D-pad, never exclusive.
    has_axes = joystick.get_numaxes() > max(AXIS_LEFT_X, AXIS_LEFT_Y)
    if has_axes:
        axis_x = joystick.get_axis(AXIS_LEFT_X)
        axis_y = joystick.get_axis(AXIS_LEFT_Y)
        if axis_y < -AXIS_THRESHOLD:
            pad1 |= NES_UP
        if axis_y > AXIS_THRESHOLD:
            pad1 |= NES_DOWN
        if axis_x < -AXIS_THRESHOLD:
            pad1 |= NES_LEFT
        if axis_x > AXIS_THRESHOLD:
            pad1 |= NES_RIGHT

    return pad1


def init_joystick_subsystem():
    """Bring up joystick + event handling without opening a real window."""
    # pygame.event.pump() raises "video system not initialized" without a video
    # subsystem, so the dummy driver stands in for a real window: it keeps SDL's
    # event queue (and therefore hotplug events) alive while staying headless.
    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    pygame.display.init()
    pygame.joystick.init()
    pygame.event.pump()


# SDL's Switch HIDAPI driver performs a multi-step handshake when opening a Pro
# Controller and can lose the race against macOS's gamecontrollerd, failing with
# messages like "Couldn't enable vibration" or "Couldn't load stick calibration".
# A failed open also detaches the device, so the retry has to restart the whole
# joystick subsystem rather than just re-requesting the same index.
OPEN_RETRY_COUNT = 3
OPEN_RETRY_DELAY = 0.4

HANDSHAKE_HINT = (
    "SDL が Pro コントローラの初期化に失敗しました。\n"
    "macOS の gamecontrollerd がデバイスを掴んでいる場合に起こります。対処:\n"
    "  - Steam / Discord など、コントローラを掴むアプリを終了する\n"
    "  - USB ケーブルを挿し直す (または Bluetooth を切って USB 接続にする)\n"
    "  - コントローラの HOME ボタンを押して復帰させる\n"
    "  - それでも直らない場合は Mac を再起動する"
)


def open_joystick(index=0):
    """Open one joystick, retrying past SDL's transient macOS open failures."""
    last_error = None
    for attempt in range(OPEN_RETRY_COUNT):
        try:
            joystick = pygame.joystick.Joystick(index)
            joystick.init()
            return joystick
        except pygame.error as error:
            last_error = error
            is_last_attempt = attempt == OPEN_RETRY_COUNT - 1
            if is_last_attempt:
                break
            pygame.joystick.quit()
            time.sleep(OPEN_RETRY_DELAY)
            pygame.joystick.init()
    raise last_error


def open_first_joystick():
    """Return an initialised joystick, or None when nothing is connected."""
    count = pygame.joystick.get_count()
    if count == 0:
        return None
    return open_joystick(0)


def list_joysticks():
    init_joystick_subsystem()
    count = pygame.joystick.get_count()
    if count == 0:
        print("ジョイスティックが見つかりません。")
        print("Switch Pro コントローラを USB ケーブルで接続するか、")
        print("Bluetooth でペアリングしてから再実行してください。")
        return 1

    print(f"検出されたジョイスティック: {count} 台")
    for index in range(count):
        try:
            joystick = open_joystick(index)
        except pygame.error as error:
            print(f"  [{index}] オープンに失敗しました: {error}")
            print(HANDSHAKE_HINT)
            continue
        print(f"  [{index}] {joystick.get_name()}")
        print(
            f"       buttons={joystick.get_numbuttons()} "
            f"hats={joystick.get_numhats()} axes={joystick.get_numaxes()} "
            f"guid={joystick.get_guid()}"
        )
    return 0


def list_hid_devices():
    """Show Pro Controllers visible to hidapi, which SDL may fail to open."""
    is_hid_importable = hid is not None
    if not is_hid_importable:
        return

    devices = hid.enumerate(PROCON_VENDOR_ID, PROCON_PRODUCT_ID)
    print()
    if not devices:
        print("hidapi: Pro コントローラ (USB) は見つかりませんでした。")
        return

    print(f"hidapi から見えるデバイス: {len(devices)} 件")
    for device in devices:
        print(
            f"  {device.get('product_string')} "
            f"VID=0x{device['vendor_id']:04X} PID=0x{device['product_id']:04X}"
        )
    print("  (SDL が失敗しても --backend hid で動作します)")


# ------------------------------------------------------------- test pattern


def run_test_pattern(sender, verbose=True):
    """Send a fixed sequence of button states with no joystick attached.

    Exercises the whole send path so the wire format can be verified against a
    receiver without hardware.
    """
    pattern = [
        ("none", 0x00),
        ("A", NES_A),
        ("B", NES_B),
        ("Start", NES_START),
        ("Select", NES_SELECT),
        ("Up", NES_UP),
        ("Down", NES_DOWN),
        ("Left", NES_LEFT),
        ("Right", NES_RIGHT),
        ("A+Right", NES_A | NES_RIGHT),
        ("all", 0xFF),
    ]

    print(f"テストパターン送信: {sender.host}:{sender.port}")
    for label, pad1 in pattern:
        packet = sender.send(pad1)
        if verbose:
            print(f"  {label:<8} pad1=0x{pad1:02X}  {packet.hex(' ')}")
        time.sleep(0.05)
    print(f"{len(pattern)} パケット送信完了。")
    return 0


# -------------------------------------------------------------------- main


def run_hid_loop(sender, rate_hz, controller):
    """Timing loop for the hidapi backend, matching the SDL loop's cadence."""
    interval = 1.0 / rate_hz
    last_pad1 = None
    last_home = False
    next_tick = time.monotonic()

    while True:
        try:
            pad1, home = controller.read_buttons()
        except OSError as error:
            # Unplugging the pad surfaces as a read error on macOS.
            print(f"コントローラの読み取りに失敗しました: {error}", file=sys.stderr)
            has_stale_state = last_pad1 not in (None, 0)
            if has_stale_state:
                sender.send(0)
            return 1

        # HOME はパッドビットではなく「メニューを開く」制御として押下エッジで
        # 1 回だけ送る (押しっぱなしでも連射しない)。
        is_home_pressed = home and not last_home
        last_home = home
        if is_home_pressed:
            sender.send_menu()
            print("HOME -> menu")

        is_changed = pad1 != last_pad1
        if is_changed:
            sender.send(pad1)
            print(f"pad1=0x{pad1:02X}  {format_bits(pad1)}")
            last_pad1 = pad1
            next_tick = time.monotonic() + interval
            continue

        now = time.monotonic()
        is_tick_due = now >= next_tick
        if is_tick_due:
            sender.send(pad1)
            next_tick += interval
            if next_tick < now:
                next_tick = now + interval
            continue

        time.sleep(min(0.001, max(0.0, next_tick - now)))


def open_hid_backend():
    """Open the Pro Controller through hidapi, or return None if unavailable."""
    is_hid_importable = hid is not None
    if not is_hid_importable:
        print(
            "hidapi が import できません (uv run なら自動で入ります)。", file=sys.stderr
        )
        return None

    try:
        controller = HidProController()
    except Exception as error:
        print(f"hidapi でのオープンに失敗しました: {error}", file=sys.stderr)
        return None

    is_streaming = controller.start()
    if not is_streaming:
        print("hidapi: 入力レポートが届きません。", file=sys.stderr)
        controller.close()
        return None

    return controller


def acquire_sdl_joystick():
    """Try the SDL path. Returns (joystick, error) with at most one set."""
    init_joystick_subsystem()
    try:
        return open_first_joystick(), None
    except pygame.error as error:
        return None, error


# A partially initialised SDL device (a leftover of the failed-open retries) can
# report a D-pad direction that is not actually held, which would send the
# emulator a stuck input. Sampling the resting state catches that before we
# commit to the SDL backend.
SDL_SANITY_SAMPLES = 20


def is_sdl_state_trustworthy(joystick):
    """True when the pad reads as idle, i.e. no direction is stuck on."""
    for _ in range(SDL_SANITY_SAMPLES):
        pygame.event.pump()
        pad1 = read_buttons(joystick)
        has_phantom_input = pad1 != 0
        if has_phantom_input:
            return False
        time.sleep(0.01)
    return True


def run_loop(sender, rate_hz, backend="auto"):
    """Poll the controller and send on a fixed tick plus on every change."""
    joystick = None
    hid_controller = None

    wants_sdl = backend in ("auto", "sdl")
    if wants_sdl:
        joystick, sdl_error = acquire_sdl_joystick()
        has_sdl_failure = sdl_error is not None
        if has_sdl_failure:
            print(f"SDL でのオープンに失敗しました: {sdl_error}", file=sys.stderr)
            is_sdl_only = backend == "sdl"
            if is_sdl_only:
                print("", file=sys.stderr)
                print(HANDSHAKE_HINT, file=sys.stderr)
                return 1
            print("hidapi にフォールバックします...", file=sys.stderr)

        is_suspect = (
            joystick is not None
            and backend == "auto"
            and not is_sdl_state_trustworthy(joystick)
        )
        if is_suspect:
            print(
                "SDL が押されていない入力を報告しています。"
                "hidapi にフォールバックします...",
                file=sys.stderr,
            )
            joystick = None

    wants_hid_now = backend == "hid" or (backend == "auto" and joystick is None)
    if wants_hid_now:
        hid_controller = open_hid_backend()

    has_no_backend = joystick is None and hid_controller is None
    if has_no_backend:
        print("エラー: コントローラを開けませんでした。", file=sys.stderr)
        print("", file=sys.stderr)
        print(
            "Switch Pro コントローラを USB ケーブルで Mac に接続してください。",
            file=sys.stderr,
        )
        print("接続済みのデバイスは --list で確認できます。", file=sys.stderr)
        print("", file=sys.stderr)
        print(HANDSHAKE_HINT, file=sys.stderr)
        return 1

    is_hid_mode = hid_controller is not None
    if is_hid_mode:
        print(f"デバイス : {hid_controller.name}")
        print("バックエンド: hidapi (0x30 standard report)")
    else:
        print(f"デバイス : {joystick.get_name()}")
        print(
            f"           buttons={joystick.get_numbuttons()} "
            f"hats={joystick.get_numhats()} axes={joystick.get_numaxes()}"
        )
        print("バックエンド: SDL (pygame)")
        print(
            "注意     : HOME でのメニュー呼び出しは USB (--backend hid) のみ対応です。"
        )
    print(f"宛先     : {sender.host}:{sender.port}")
    print(f"レート   : {rate_hz} Hz")
    print("Ctrl-C で終了します。")

    if is_hid_mode:
        try:
            return run_hid_loop(sender, rate_hz, hid_controller)
        finally:
            hid_controller.close()

    interval = 1.0 / rate_hz
    last_pad1 = None
    next_tick = time.monotonic()

    while True:
        for event in pygame.event.get():
            is_added = event.type == pygame.JOYDEVICEADDED
            is_removed = event.type == pygame.JOYDEVICEREMOVED
            if is_removed:
                print("コントローラが切断されました。再接続を待っています...")
                joystick = None
            if is_added and joystick is None:
                try:
                    joystick = open_first_joystick()
                except pygame.error as error:
                    # Stay in the reconnect loop; the next add event retries.
                    print(f"再接続に失敗しました: {error}")
                    joystick = None
                if joystick is not None:
                    print(f"再接続しました: {joystick.get_name()}")
                    last_pad1 = None

        is_disconnected = joystick is None
        if is_disconnected:
            # Hold the emulator's inputs clear while nothing is attached.
            has_stale_state = last_pad1 not in (None, 0)
            if has_stale_state:
                sender.send(0)
                last_pad1 = 0
            time.sleep(0.1)
            next_tick = time.monotonic()
            continue

        pad1 = read_buttons(joystick)

        is_changed = pad1 != last_pad1
        if is_changed:
            sender.send(pad1)
            print(f"pad1=0x{pad1:02X}  {format_bits(pad1)}")
            last_pad1 = pad1
            next_tick = time.monotonic() + interval
            continue

        now = time.monotonic()
        is_tick_due = now >= next_tick
        if is_tick_due:
            sender.send(pad1)
            # Advance from the deadline, not from now, to avoid drift.
            next_tick += interval
            if next_tick < now:
                next_tick = now + interval
            continue

        time.sleep(min(0.001, max(0.0, next_tick - now)))


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=(
            "Switch Pro コントローラの入力を UDP で NES エミュレータへ送信します。"
        )
    )
    parser.add_argument(
        "--host",
        default=DEFAULT_HOST,
        help=f"送信先アドレス (default: {DEFAULT_HOST} = ブロードキャスト)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"送信先ポート (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--rate",
        type=int,
        default=DEFAULT_RATE_HZ,
        help=f"定期送信レート Hz (default: {DEFAULT_RATE_HZ})",
    )
    parser.add_argument(
        "--backend",
        choices=("auto", "sdl", "hid"),
        default="auto",
        help=(
            "入力バックエンド "
            "(default: auto = SDL 失敗時に hidapi へ自動フォールバック)"
        ),
    )
    parser.add_argument(
        "--list", action="store_true", help="ジョイスティックを列挙して終了する"
    )
    parser.add_argument(
        "--test-pattern",
        action="store_true",
        help="ジョイスティックなしで固定パターンを送信する (疎通確認用)",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    if args.list:
        status = list_joysticks()
        list_hid_devices()
        return status

    is_valid_rate = args.rate > 0
    if not is_valid_rate:
        print("エラー: --rate は 1 以上を指定してください。", file=sys.stderr)
        return 2

    sender = PacketSender(args.host, args.port)
    try:
        if args.test_pattern:
            return run_test_pattern(sender)
        return run_loop(sender, args.rate, args.backend)
    except KeyboardInterrupt:
        print("\n終了します。")
        return 0
    finally:
        sender.close()


if __name__ == "__main__":
    sys.exit(main())
