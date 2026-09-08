#include "reply_sink.h"

#include "serial_link.h"

void replySend(const ReplySink& sink, const uint8_t* data, size_t len) {
    if (sink.via == ReplyVia::Udp) {
        // lwIP's sendto is thread-safe, which is what lets the frame loop on
        // core 1 answer without handing buffers to the UDP task.
        ::sendto(sink.sock, data, len, 0, (const sockaddr*)&sink.peer, sizeof(sink.peer));
        return;
    }
    if (sink.via == ReplyVia::Serial) {
        serialLinkSend(data, len);
        return;
    }
    // ReplyVia::None: a latched request whose asker was never recorded. Dropping
    // it is correct — there is nowhere to send.
}
