// libFuzzer harness for the MTProto TL deserializer (generated scheme read path).
//
// Exercises the exact "length/count fields on the read path" surface named in the
// task: vector counts, string/bytes length prefixes, nested + recursive boxed
// types, on server-reachable top-level types. Bytes -> mtpPrime buffer -> boxed
// read(from,end). Selector byte spreads coverage across several entry types
// (recursive ones like Page/RichText/WebPage included for stack-depth bugs).
//
// Assertion failures (Expects/Assert) route through base::assertion::fail (null
// deref + abort) -> ASan reports them as crashes, which is what we want.

#include "scheme.h"

#include <cstdint>
#include <cstddef>
#include <vector>

// tdesktop leaves base::assertion::log undefined outside the app; provide it.
namespace base::assertion {
void log(const char *message, const char *file, int line) {
    // no-op: fail() still null-derefs/aborts after this, ASan catches it.
    (void)message; (void)file; (void)line;
}
} // namespace base::assertion

namespace {

template <typename T>
void tryRead(const mtpPrime *from, const mtpPrime *end) {
    T value;
    // boxed read: consumes the constructor id from the buffer, then dispatches.
    value.read(from, end);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    const uint8_t selector = data[0];
    ++data; --size;

    // Copy the remaining bytes into an aligned, heap mtpPrime buffer so reads
    // are well-aligned and ASan red-zones bracket it (past-end reads detected).
    const size_t primes = size / sizeof(mtpPrime);
    if (primes == 0) return 0;
    std::vector<mtpPrime> buffer(primes);
    memcpy(buffer.data(), data, primes * sizeof(mtpPrime));
    const auto *from = buffer.data();
    const auto *end = from + primes;

    // Broadened server-push surface: the top-level types a (malicious) server can
    // send the client, weighted toward nested/recursive ones (Instant View blocks,
    // rich text, media, reply markup, difference) where count/length interpretation
    // bugs and stack-depth issues are most likely.
    switch (selector % 32) {
    case 0:  tryRead<MTPUpdates>(from, end); break;
    case 1:  tryRead<MTPmessages_Messages>(from, end); break;
    case 2:  tryRead<MTPUser>(from, end); break;
    case 3:  tryRead<MTPPage>(from, end); break;          // recursive (IV)
    case 4:  tryRead<MTPRichText>(from, end); break;      // recursive
    case 5:  tryRead<MTPWebPage>(from, end); break;
    case 6:  tryRead<MTPhelp_ConfigSimple>(from, end); break;
    case 7:  tryRead<MTPDataJSON>(from, end); break;
    case 8:  tryRead<MTPPageBlock>(from, end); break;     // recursive (IV blocks)
    case 9:  tryRead<MTPMessage>(from, end); break;
    case 10: tryRead<MTPMessageMedia>(from, end); break;
    case 11: tryRead<MTPReplyMarkup>(from, end); break;   // nested button rows
    case 12: tryRead<MTPKeyboardButton>(from, end); break;
    case 13: tryRead<MTPDocument>(from, end); break;
    case 14: tryRead<MTPPhoto>(from, end); break;
    case 15: tryRead<MTPMessageEntity>(from, end); break;
    case 16: tryRead<MTPTextWithEntities>(from, end); break;
    case 17: tryRead<MTPWebPageAttribute>(from, end); break;
    case 18: tryRead<MTPmessages_BotResults>(from, end); break;
    case 19: tryRead<MTPupdates_Difference>(from, end); break;
    case 20: tryRead<MTPmessages_Dialogs>(from, end); break;
    case 21: tryRead<MTPmessages_StickerSet>(from, end); break;
    case 22: tryRead<MTPStickerSet>(from, end); break;
    case 23: tryRead<MTPWebDocument>(from, end); break;
    case 24: tryRead<MTPPeerSettings>(from, end); break;
    case 25: tryRead<MTPWallPaper>(from, end); break;
    case 26: tryRead<MTPBotInlineResult>(from, end); break;
    case 27: tryRead<MTPStoryItem>(from, end); break;
    case 28: tryRead<MTPExportedChatInvite>(from, end); break;
    case 29: tryRead<MTPDialog>(from, end); break;
    case 30: tryRead<MTPChat>(from, end); break;
    case 31: tryRead<MTPStickerSetCovered>(from, end); break;
    }
    return 0;
}
