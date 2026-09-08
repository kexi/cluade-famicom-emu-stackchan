#pragma once

// Where a reply goes: the UDP peer that asked, or the USB serial link.
//
// The protocol handlers used to take (int sock, const sockaddr_in& from) and
// call ::sendto directly. That was fine while UDP was the only transport, but
// the browser cannot send UDP, so serving the page from GitHub Pages — with no
// relay process — needs the same packets to travel over USB. Rather than
// duplicating every handler, the "who asked" argument became this type and the
// six ::sendto call sites became replySend().
//
// Deliberately a plain struct with a tag rather than a virtual interface: two
// of these are stored in globals that core 1 reads (the deferred ROM-save event
// and the SD listing), and a vtable pointer there would mean an object whose
// lifetime has to outlive the frame boundary. A value type copies cleanly.

#include <lwip/sockets.h>

#include <cstddef>
#include <cstdint>

enum class ReplyVia : uint8_t {
    None,   // nothing to answer (a latched request whose asker is gone)
    Udp,
    Serial,
};

struct ReplySink {
    ReplyVia via = ReplyVia::None;
    // Only meaningful when via == Udp. Kept inline rather than behind a pointer
    // so a copy of this struct is self-contained across the core boundary.
    int sock = -1;
    sockaddr_in peer = {};
};

inline ReplySink udpSink(int sock, const sockaddr_in& peer) {
    ReplySink sink;
    sink.via = ReplyVia::Udp;
    sink.sock = sock;
    sink.peer = peer;
    return sink;
}

inline ReplySink serialSink() {
    ReplySink sink;
    sink.via = ReplyVia::Serial;
    return sink;
}

// Send one protocol message to whoever asked for it. Safe to call from either
// core: the UDP path relies on lwIP's thread-safe sendto, and the serial path
// takes a mutex (see serial_link.cpp).
void replySend(const ReplySink& sink, const uint8_t* data, size_t len);
