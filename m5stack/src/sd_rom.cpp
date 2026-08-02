#include "sd_rom.h"

#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>

#include <cstdio>
#include <cstring>

// Whether the card is currently believed usable. A cached flag rather than a
// probe on every call: SD.cardType() talks to the card, and the menu asks this
// once per frame. Every operation that fails on I/O clears it, so the answer
// stays honest without polling.
static bool g_mounted = false;

// ------------------------------------------------------------------ helpers

// Build "/roms/<name>". Every path in this file goes through here, and `name`
// is always a sanitised basename by the time it arrives, so the result cannot
// escape SD_ROMS_DIR. Why not sanitise here instead: the callers need to *know*
// when a name was unrepresentable (to answer BadName) rather than silently
// operate on a mangled one, so validation stays with them and this only joins.
static void sdRomPath(const char* name, char* out, size_t cap) { snprintf(out, cap, "%s/%s", SD_ROMS_DIR, name); }

// A name is usable only if sanitising leaves it unchanged. Anything else means
// the caller handed us something FAT or this firmware would not round-trip, and
// operating on the mangled version would act on a different file than asked.
static bool sdRomNameValid(const char* name) {
    const bool empty = name == nullptr || name[0] == '\0';
    if (empty) return false;
    const bool tooLong = strlen(name) >= (size_t)SD_ROM_NAME_MAX;
    if (tooLong) return false;
    char clean[SD_ROM_NAME_MAX];
    sdRomSanitizeName(name, clean, sizeof(clean));
    return strcmp(clean, name) == 0;
}

static bool endsWithNes(const char* name) {
    const size_t len = strlen(name);
    const bool tooShort = len < 4;
    if (tooShort) return false;
    const char* ext = name + len - 4;
    return ext[0] == '.' && (ext[1] | 0x20) == 'n' && (ext[2] | 0x20) == 'e' && (ext[3] | 0x20) == 's';
}

static bool endsWithPart(const char* name) {
    const size_t len = strlen(name);
    const size_t suffixLen = strlen(SD_PART_SUFFIX);
    if (len < suffixLen) return false;
    return strcmp(name + len - suffixLen, SD_PART_SUFFIX) == 0;
}

// Case-insensitive ASCII compare, for sorting a listing the way a user reads
// it. Not strcasecmp: that is locale-aware in principle, and the names here are
// already restricted to ASCII by the sanitiser.
static int nameCompare(const char* a, const char* b) {
    for (;; a++, b++) {
        const int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : (unsigned char)*a;
        const int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : (unsigned char)*b;
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
}

// Mark the card lost after an I/O failure and try to get it back once.
//
// Why exactly one retry: the failure this exists for is a card that was pulled
// and put back, which one remount fixes. A card that is genuinely broken fails
// the remount too, and retrying that in a loop would park the frame loop on the
// SPI bus for as long as the user leaves it broken. Every later call retries
// anyway (through the mount check), so recovery is never permanently lost — it
// just costs one more operation.
static bool recoverAfterIoFailure() {
    Serial.println("SD: I/O failure, attempting remount");
    return sdRomRemount();
}

// ------------------------------------------------------------------- mount

bool sdRomInit() {
    // Pins from M5Unified rather than literals: the CoreS3 and its variants
    // wire the shared bus differently, and M5.getPin() is the board-aware
    // answer that already accounts for the model detected at M5.begin().
    const int sck = M5.getPin(m5::pin_name_t::sd_spi_sclk);
    const int miso = M5.getPin(m5::pin_name_t::sd_spi_miso);
    const int mosi = M5.getPin(m5::pin_name_t::sd_spi_mosi);
    const int cs = M5.getPin(m5::pin_name_t::sd_spi_ss);
    const bool pinsUnknown = sck < 0 || miso < 0 || mosi < 0 || cs < 0;
    if (pinsUnknown) {
        Serial.println("SD: no SD pins on this board");
        g_mounted = false;
        return false;
    }

    // The LCD already ran SPI.begin() on this bus during M5.begin(); calling it
    // again with the same pins is idempotent in the Arduino layer and is what
    // makes the SD library share the bus instead of claiming a second one.
    SPI.begin(sck, miso, mosi, cs);
    g_mounted = SD.begin((uint8_t)cs, SPI, SD_SPI_FREQ);
    if (!g_mounted) {
        Serial.println("SD: mount failed (no card?)");
        return false;
    }

    // Created eagerly so the first save does not have to, and so a fresh card
    // shows the user where to drop files.
    const bool dirMissing = !SD.exists(SD_ROMS_DIR);
    if (dirMissing) SD.mkdir(SD_ROMS_DIR);
    sdRomCleanupParts();

    uint64_t total = 0, freeBytes = 0;
    sdRomSpace(&total, &freeBytes);
    Serial.printf("SD: mounted %s free=%lluMB total=%lluMB\n", SD_ROMS_DIR,
                  (unsigned long long)(freeBytes / (1024 * 1024)), (unsigned long long)(total / (1024 * 1024)));
    return true;
}

bool sdRomMounted() { return g_mounted; }

bool sdRomRemount() {
    SD.end();
    const int cs = M5.getPin(m5::pin_name_t::sd_spi_ss);
    if (cs < 0) {
        g_mounted = false;
        return false;
    }
    g_mounted = SD.begin((uint8_t)cs, SPI, SD_SPI_FREQ);
    Serial.printf("SD: remount %s\n", g_mounted ? "ok" : "failed");
    return g_mounted;
}

void sdRomSpace(uint64_t* totalBytes, uint64_t* freeBytes) {
    if (totalBytes) *totalBytes = 0;
    if (freeBytes) *freeBytes = 0;
    if (!g_mounted) return;
    const uint64_t total = SD.totalBytes();
    const uint64_t used = SD.usedBytes();
    if (totalBytes) *totalBytes = total;
    // usedBytes can exceed totalBytes on a card whose FAT info sector is stale,
    // and an unsigned subtraction there would report a free space of several
    // exabytes — which the save-side space check would then happily accept.
    if (freeBytes) *freeBytes = total > used ? total - used : 0;
}

void sdRomCleanupParts() {
    if (!g_mounted) return;
    File dir = SD.open(SD_ROMS_DIR);
    if (!dir || !dir.isDirectory()) return;

    // Collected before deleting rather than removed while walking: the FAT
    // driver's directory iterator is invalidated by a remove() on the directory
    // it is walking, so deleting in place skips entries.
    //
    // static rather than automatic: 4KB out of the Arduino loop task's 8KB
    // stack, on top of what the SD/FAT driver needs underneath, is more than the
    // frame is worth. Safe because sdRomInit() calls this once from setup() and
    // nothing re-enters it.
    static char doomed[SD_ROM_MAX_FILES][SD_ROM_NAME_MAX];
    int count = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const char* name = f.name();
        const bool skip = f.isDirectory() || !endsWithPart(name) || strlen(name) >= (size_t)SD_ROM_NAME_MAX;
        f.close();
        if (skip) continue;
        if (count >= SD_ROM_MAX_FILES) break;
        strncpy(doomed[count], name, SD_ROM_NAME_MAX - 1);
        doomed[count][SD_ROM_NAME_MAX - 1] = '\0';
        count++;
    }
    dir.close();

    for (int i = 0; i < count; i++) {
        char path[SD_ROM_NAME_MAX + sizeof(SD_ROMS_DIR) + 2];
        sdRomPath(doomed[i], path, sizeof(path));
        SD.remove(path);
        Serial.printf("SD: swept %s (interrupted write)\n", doomed[i]);
    }
}

// -------------------------------------------------------------------- scan

int sdRomScan(SdRomEntry* out, int max) {
    if (!g_mounted || max <= 0) return 0;

    File dir = SD.open(SD_ROMS_DIR);
    const bool dirUnusable = !dir || !dir.isDirectory();
    if (dirUnusable) {
        if (dir) dir.close();
        // A missing directory on a mounted card is not an I/O failure — the
        // user may simply have never created it — so recreate rather than
        // unmounting the card over it.
        SD.mkdir(SD_ROMS_DIR);
        return 0;
    }

    int count = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const char* name = f.name();
        // f.name() returns the basename on the ESP32 SD driver for entries
        // walked from an open directory, but a full path on some versions.
        // Take everything after the last '/' either way, so the listing is
        // always what sdRomPath() will later join back on.
        const char* slash = strrchr(name, '/');
        if (slash) name = slash + 1;

        const bool notRom = f.isDirectory() || !endsWithNes(name);
        const uint32_t size = notRom ? 0 : (uint32_t)f.size();
        f.close();
        if (notRom) continue;

        const bool unusableName = !sdRomNameValid(name);
        if (unusableName) {
            Serial.printf("SD: skipping '%s' (name not representable)\n", name);
            continue;
        }
        if (count >= max) {
            Serial.printf("SD: listing truncated at %d entries\n", max);
            break;
        }
        strncpy(out[count].name, name, SD_ROM_NAME_MAX - 1);
        out[count].name[SD_ROM_NAME_MAX - 1] = '\0';
        out[count].size = size;
        count++;
    }
    dir.close();

    // Insertion sort: the list is at most SD_ROM_MAX_FILES and already close to
    // sorted on most cards, so this beats dragging in qsort's code size for a
    // one-shot call that happens on a menu entry.
    for (int i = 1; i < count; i++) {
        SdRomEntry key = out[i];
        int j = i - 1;
        while (j >= 0 && nameCompare(out[j].name, key.name) > 0) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return count;
}

// -------------------------------------------------------------------- load

SdStatus sdRomLoad(const char* name, uint8_t* buf, size_t cap, size_t* outSize) {
    if (outSize) *outSize = 0;
    if (!sdRomNameValid(name)) return SdStatus::BadName;
    if (!g_mounted && !sdRomRemount()) return SdStatus::NotMounted;

    char path[SD_ROM_NAME_MAX + sizeof(SD_ROMS_DIR) + 2];
    sdRomPath(name, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) {
        // Distinguish "no such file" from "the card went away": exists() on a
        // dead card fails too, so a failed open with a failing exists() is
        // treated as I/O and gets the one remount.
        const bool cardAlive = SD.exists(SD_ROMS_DIR);
        if (cardAlive) return SdStatus::NotFound;
        return recoverAfterIoFailure() ? SdStatus::NotFound : SdStatus::NotMounted;
    }

    const size_t size = (size_t)f.size();
    const size_t limit = cap < ROM_MAX_SIZE ? cap : ROM_MAX_SIZE;
    const bool tooBig = size > limit;
    // 16 bytes is the iNES header; anything shorter cannot even be inspected,
    // let alone loaded, and reading it would leave the caller checking a header
    // built from uninitialised staging memory.
    const bool tooSmall = size < 16;
    if (tooBig || tooSmall) {
        f.close();
        return tooBig ? SdStatus::TooBig : SdStatus::BadRom;
    }

    size_t done = 0;
    while (done < size) {
        const size_t want = (size - done) < SD_IO_CHUNK ? (size - done) : SD_IO_CHUNK;
        const int got = f.read(buf + done, want);
        const bool readFailed = got <= 0;
        if (readFailed) {
            f.close();
            Serial.printf("SD: read failed at %u/%u of %s\n", (unsigned)done, (unsigned)size, name);
            return recoverAfterIoFailure() ? SdStatus::IoError : SdStatus::NotMounted;
        }
        done += (size_t)got;
    }
    f.close();
    if (outSize) *outSize = size;
    return SdStatus::Ok;
}

// -------------------------------------------------------------------- save

SdStatus sdRomSave(const char* name, const uint8_t* buf, size_t size) {
    if (!sdRomNameValid(name)) return SdStatus::BadName;
    if (size == 0 || size > ROM_MAX_SIZE) return SdStatus::TooBig;
    if (!g_mounted && !sdRomRemount()) return SdStatus::NotMounted;

    // Checked before a single byte is written, so the failure mode for a full
    // card is a clean rejection rather than a half-written .part that then has
    // to be swept.
    uint64_t total = 0, freeBytes = 0;
    sdRomSpace(&total, &freeBytes);
    const bool wontFit = freeBytes < (uint64_t)size + SD_SAVE_MARGIN_BYTES;
    if (wontFit) {
        Serial.printf("SD: no space for %s (%u bytes, %llu free)\n", name, (unsigned)size,
                      (unsigned long long)freeBytes);
        return SdStatus::NoSpace;
    }

    char finalPath[SD_ROM_NAME_MAX + sizeof(SD_ROMS_DIR) + 2];
    sdRomPath(name, finalPath, sizeof(finalPath));
    char partPath[sizeof(finalPath) + sizeof(SD_PART_SUFFIX)];
    snprintf(partPath, sizeof(partPath), "%s%s", finalPath, SD_PART_SUFFIX);

    // A leftover .part from a previous attempt would otherwise be appended to.
    SD.remove(partPath);
    File f = SD.open(partPath, FILE_WRITE);
    if (!f) {
        Serial.printf("SD: cannot create %s\n", partPath);
        return recoverAfterIoFailure() ? SdStatus::IoError : SdStatus::NotMounted;
    }

    size_t done = 0;
    bool writeFailed = false;
    while (done < size) {
        const size_t want = (size - done) < SD_IO_CHUNK ? (size - done) : SD_IO_CHUNK;
        const size_t wrote = f.write(buf + done, want);
        // A short write is the card filling up or failing mid-transfer; either
        // way the .part is incomplete and must not survive.
        if (wrote != want) {
            writeFailed = true;
            break;
        }
        done += wrote;
    }
    f.close();

    if (writeFailed) {
        SD.remove(partPath);
        Serial.printf("SD: write failed at %u/%u of %s\n", (unsigned)done, (unsigned)size, name);
        // The space check passed, so a short write here is either a card that
        // went away or one whose reported free space lied. Reported as NoSpace
        // only when the card is still there — otherwise the user would be told
        // to free space on a card that is no longer present.
        if (!recoverAfterIoFailure()) return SdStatus::NotMounted;
        return SdStatus::IoError;
    }

    // The overwrite happens here, at the last possible moment: until the rename
    // the old image is still intact, so a failure anywhere above leaves the
    // user with the ROM they already had.
    SD.remove(finalPath);
    const bool renamed = SD.rename(partPath, finalPath);
    if (!renamed) {
        SD.remove(partPath);
        Serial.printf("SD: rename %s -> %s failed\n", partPath, finalPath);
        return recoverAfterIoFailure() ? SdStatus::IoError : SdStatus::NotMounted;
    }
    Serial.printf("SD: saved %s (%u bytes)\n", name, (unsigned)size);
    return SdStatus::Ok;
}

// ---------------------------------------------------------- delete / rename

SdStatus sdRomDelete(const char* name) {
    if (!sdRomNameValid(name)) return SdStatus::BadName;
    if (!g_mounted && !sdRomRemount()) return SdStatus::NotMounted;

    char path[SD_ROM_NAME_MAX + sizeof(SD_ROMS_DIR) + 2];
    sdRomPath(name, path, sizeof(path));
    if (!SD.exists(path)) {
        const bool cardAlive = SD.exists(SD_ROMS_DIR);
        if (cardAlive) return SdStatus::NotFound;
        return recoverAfterIoFailure() ? SdStatus::NotFound : SdStatus::NotMounted;
    }
    const bool removed = SD.remove(path);
    if (!removed) {
        Serial.printf("SD: remove %s failed\n", name);
        return recoverAfterIoFailure() ? SdStatus::IoError : SdStatus::NotMounted;
    }
    Serial.printf("SD: deleted %s\n", name);
    return SdStatus::Ok;
}

SdStatus sdRomRename(const char* from, const char* to) {
    if (!sdRomNameValid(from) || !sdRomNameValid(to)) return SdStatus::BadName;
    if (!g_mounted && !sdRomRemount()) return SdStatus::NotMounted;

    char fromPath[SD_ROM_NAME_MAX + sizeof(SD_ROMS_DIR) + 2];
    char toPath[sizeof(fromPath)];
    sdRomPath(from, fromPath, sizeof(fromPath));
    sdRomPath(to, toPath, sizeof(toPath));

    if (!SD.exists(fromPath)) {
        const bool cardAlive = SD.exists(SD_ROMS_DIR);
        if (cardAlive) return SdStatus::NotFound;
        return recoverAfterIoFailure() ? SdStatus::NotFound : SdStatus::NotMounted;
    }
    // Never silently overwrite. The caller that has a user in front of it asks
    // for confirmation and re-issues as a delete plus a rename; a firmware that
    // clobbered here would give it no way to offer that choice.
    const bool sameName = strcmp(fromPath, toPath) == 0;
    if (!sameName && SD.exists(toPath)) return SdStatus::Exists;

    const bool renamed = SD.rename(fromPath, toPath);
    if (!renamed) {
        Serial.printf("SD: rename %s -> %s failed\n", from, to);
        return recoverAfterIoFailure() ? SdStatus::IoError : SdStatus::NotMounted;
    }
    Serial.printf("SD: renamed %s -> %s\n", from, to);
    return SdStatus::Ok;
}

// ------------------------------------------------------------------- names

void sdRomSanitizeName(const char* in, char* out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!in) in = "";

    // Only the basename survives. This is the single line that makes traversal
    // impossible: whatever the sender wrote, only the part after the last
    // separator reaches the filesystem, and every remaining byte is then
    // restricted to a character class that contains neither '/' nor a bare '.'
    // sequence that could climb.
    const char* slash = strrchr(in, '/');
    if (slash) in = slash + 1;
    const char* backslash = strrchr(in, '\\');
    if (backslash) in = backslash + 1;

    // Reserve room for a ".nes" that may have to be appended below, so forcing
    // the extension can never be the thing that truncates — but skip the
    // reservation for a name that already carries the suffix and fits whole.
    // '.', 'n', 'e' and 's' all survive the byte filter unchanged, so such a
    // name still ends in ".nes" after copying and needs no room reserved.
    // Charging it the reservation anyway is what truncated a 59-to-63 byte name
    // into one that then failed sdRomNameValid()'s round-trip and was skipped by
    // the listing, despite being a name the card and the header both allow. A
    // .nes name too long to fit still gets the reservation, because truncating
    // it mid-body leaves a suffix that has to be re-appended.
    const bool suffixFits = endsWithNes(in) && strlen(in) < cap;
    const size_t reserve = suffixFits ? 0 : 4;
    // +1 because the loop below stops at n + 1 == bodyCap, leaving room for the
    // terminator: a bodyCap of cap therefore admits cap - 1 bytes, which is the
    // whole name a caller passing SD_ROM_NAME_MAX expects to be able to store.
    const size_t bodyCap = cap > reserve ? cap - reserve : 1;
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < bodyCap; p++) {
        const char c = *p;
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                             c == '_' || c == '-';
        out[n++] = allowed ? c : '_';
    }
    out[n] = '\0';

    // "." and ".." would survive the character filter (both are made only of
    // allowed bytes) and mean something to the filesystem, so they are rejected
    // by name rather than by class.
    const bool dotEntry = strcmp(out, ".") == 0 || strcmp(out, "..") == 0;
    if (n == 0 || dotEntry) {
        snprintf(out, cap, "rom.nes");
        return;
    }
    if (endsWithNes(out)) {
        // Case is normalised so two names differing only in extension case do
        // not both list on a FAT volume that considers them the same file.
        char* ext = out + n - 4;
        ext[1] = 'n';
        ext[2] = 'e';
        ext[3] = 's';
        return;
    }
    snprintf(out + n, cap - n, ".nes");
}

const char* sdStatusText(SdStatus status) {
    switch (status) {
    case SdStatus::Ok: return "ok";
    case SdStatus::NotMounted: return "no SD card";
    case SdStatus::NotFound: return "not found";
    case SdStatus::NoSpace: return "no space";
    case SdStatus::TooBig: return "too big";
    case SdStatus::BadRom: return "bad ROM";
    case SdStatus::BadName: return "bad name";
    case SdStatus::Busy: return "busy";
    case SdStatus::IoError: return "I/O error";
    case SdStatus::Exists: return "already exists";
    }
    return "unknown";
}
