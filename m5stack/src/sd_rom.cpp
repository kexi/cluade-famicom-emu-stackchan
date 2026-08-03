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

static bool endsWithSuffix(const char* name, const char* suffix) {
    const size_t len = strlen(name);
    const size_t suffixLen = strlen(suffix);
    if (len < suffixLen) return false;
    return strcmp(name + len - suffixLen, suffix) == 0;
}

static bool endsWithPart(const char* name) { return endsWithSuffix(name, SD_PART_SUFFIX); }

static bool endsWithBak(const char* name) { return endsWithSuffix(name, SD_BAK_SUFFIX); }

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
    // frame is worth. Safe despite being shared: both callers (sdRomInit() from
    // setup(), and the scan's remount path at a frame boundary) run on core 1
    // and this function neither recurses nor yields, so no two walks overlap.
    //
    // Sized for a full-length ROM name *plus* its suffix, not SD_ROM_NAME_MAX:
    // the entries walked here are "<name>.part" / "<name>.bak", so a name at the
    // 63-byte limit produces a 68-byte entry. Charging them SD_ROM_NAME_MAX made
    // every such entry look "too long" and fall out of the sweep, which left the
    // .bak of the longest names sitting on the card forever — the ROM they back
    // up never coming back after a power cut, which is the one failure the .bak
    // exists to prevent.
    static const size_t SUFFIX_MAX =
        sizeof(SD_PART_SUFFIX) > sizeof(SD_BAK_SUFFIX) ? sizeof(SD_PART_SUFFIX) : sizeof(SD_BAK_SUFFIX);
    static const size_t DOOMED_NAME_MAX = SD_ROM_NAME_MAX + SUFFIX_MAX;
    static char doomed[SD_ROM_MAX_FILES][DOOMED_NAME_MAX];
    // Parallel to `doomed`: true for a ".bak", which is restored rather than
    // deleted. Kept as a flag array instead of a second pass over the directory
    // so the whole sweep still costs one walk.
    static bool isBak[SD_ROM_MAX_FILES];
    int count = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        // Copied before close() rather than after: f.name() hands back a pointer
        // into the File's own storage, which close() releases, so reading it
        // afterwards is a use-after-free that happens to survive on some driver
        // versions and returns garbage on others.
        char name[DOOMED_NAME_MAX];
        const char* raw = f.name();
        const char* slash = strrchr(raw, '/');
        if (slash) raw = slash + 1;
        const bool nameFits = strlen(raw) < DOOMED_NAME_MAX;
        if (nameFits) {
            strncpy(name, raw, DOOMED_NAME_MAX - 1);
            name[DOOMED_NAME_MAX - 1] = '\0';
        } else {
            name[0] = '\0';
        }
        const bool bak = nameFits && endsWithBak(name);
        const bool skip = f.isDirectory() || !nameFits || (!endsWithPart(name) && !bak);
        f.close();
        if (skip) continue;
        if (count >= SD_ROM_MAX_FILES) break;
        strncpy(doomed[count], name, DOOMED_NAME_MAX - 1);
        doomed[count][DOOMED_NAME_MAX - 1] = '\0';
        isBak[count] = bak;
        count++;
    }
    dir.close();

    for (int i = 0; i < count; i++) {
        char path[DOOMED_NAME_MAX + sizeof(SD_ROMS_DIR) + 2];
        sdRomPath(doomed[i], path, sizeof(path));
        if (!isBak[i]) {
            SD.remove(path);
            Serial.printf("SD: swept %s (interrupted write)\n", doomed[i]);
            continue;
        }

        // A .bak that outlived its save means power was lost between moving the
        // old image aside and the new one landing. The old image is the only
        // complete copy on the card, so it goes back under its own name —
        // deleting it here is what would turn a survivable interruption into
        // the data loss the .bak exists to prevent.
        char restored[DOOMED_NAME_MAX];
        strncpy(restored, doomed[i], DOOMED_NAME_MAX - 1);
        restored[DOOMED_NAME_MAX - 1] = '\0';
        restored[strlen(restored) - strlen(SD_BAK_SUFFIX)] = '\0';
        // Now that the buffer admits names longer than SD_ROM_NAME_MAX, the
        // stripped result is checked before it is used as a destination: only a
        // name this firmware would itself hand out is a name it may create. A
        // ".bak" whose body is oversized or unrepresentable was never written by
        // sdRomSave(), so restoring it would put a file on the card that the
        // listing then refuses to show and no caller can open — worse than
        // leaving the .bak in place, where it is at least visibly debris.
        const bool restorable = sdRomNameValid(restored);
        if (!restorable) {
            Serial.printf("SD: leaving %s (restored name not representable)\n", doomed[i]);
            continue;
        }
        char finalPath[DOOMED_NAME_MAX + sizeof(SD_ROMS_DIR) + 2];
        sdRomPath(restored, finalPath, sizeof(finalPath));
        // The new image landing is what makes the .bak redundant, so a .bak
        // sitting next to an existing final name means the save did complete and
        // only the cleanup was lost. Then the .bak is the stale one.
        const bool newImageLanded = SD.exists(finalPath);
        if (newImageLanded) {
            SD.remove(path);
            Serial.printf("SD: swept %s (save already completed)\n", doomed[i]);
            continue;
        }
        const bool restoredOk = SD.rename(path, finalPath);
        Serial.printf("SD: restored %s -> %s (%s)\n", doomed[i], restored, restoredOk ? "ok" : "failed");
    }
}

// -------------------------------------------------------------------- scan

// Shortest gap between two remount attempts made on the scan path. A failed
// mount on an empty slot costs the SD library's probe timeout, and the scan runs
// on core 1's frame boundary, so an unthrottled retry would stutter the frame
// every time a listing is asked for with no card in.
static const uint32_t SCAN_REMOUNT_RETRY_MS = 3000;

// millis() of the last attempt, or 0 for "never tried". 0 also means the very
// first scan after boot retries immediately rather than waiting out the window.
static uint32_t g_lastScanRemountMs = 0;

// Get the card back before a listing, at most once per retry window.
//
// Why the scan needs this at all: sdRomLoad()/sdRomSave() already remount on
// demand, so without it a card inserted after boot (or pulled and put back)
// stays invisible in every listing while loading a ROM by name from that same
// card would work — the listing is the only way the user finds the name, so the
// asymmetry reads as a dead card.
static bool ensureMountedForScan() {
    if (g_mounted) return true;

    // millis() wrapping (49.7 days) can only make one retry come early, which is
    // harmless — the window exists to bound cost, not for correctness.
    const uint32_t now = millis();
    const bool everTried = g_lastScanRemountMs != 0;
    const bool tooSoon = everTried && (now - g_lastScanRemountMs) < SCAN_REMOUNT_RETRY_MS;
    if (tooSoon) return false;

    g_lastScanRemountMs = now;
    if (!sdRomRemount()) return false;

    // A card that just came back is in the same state as one found at boot: it
    // may be brand new (no /roms) and it may carry the debris of a save that was
    // interrupted by the removal itself, which is exactly what sdRomInit() does
    // on the boot path. Doing it here keeps a re-inserted card from listing a
    // half-written .part as if it were a ROM.
    const bool dirMissing = !SD.exists(SD_ROMS_DIR);
    if (dirMissing) SD.mkdir(SD_ROMS_DIR);
    sdRomCleanupParts();
    return true;
}

int sdRomScan(SdRomEntry* out, int max) {
    if (max <= 0) return 0;
    if (!ensureMountedForScan()) return 0;

    File dir = SD.open(SD_ROMS_DIR);
    const bool dirUnusable = !dir || !dir.isDirectory();
    if (dirUnusable) {
        if (dir) dir.close();
        // Two very different causes look identical here, so they are told apart
        // before deciding: the user may never have created the directory (fine,
        // make it), or the card may have been pulled (not fine — every later
        // call would keep reporting an empty listing on a card that is gone).
        // mkdir succeeding is the proof the card is still writable; when it
        // fails the card is dropped so sdRomMounted() turns false and the
        // existing remount path in sdRomLoad()/sdRomSave() — and the
        // NotMounted reply the LIST caller sends on !sdRomMounted() — take over.
        //
        // Why not call recoverAfterIoFailure() here: it remounts immediately,
        // which on a card that is physically absent parks this frame on the SPI
        // bus for the mount timeout. Clearing the flag lets the next actual
        // operation pay that cost, at a point where the caller is prepared for
        // it.
        const bool cardWritable = SD.mkdir(SD_ROMS_DIR) || SD.exists(SD_ROMS_DIR);
        if (!cardWritable) {
            Serial.println("SD: /roms unusable and cannot be created, card lost");
            g_mounted = false;
        }
        return 0;
    }

    int count = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        // Copied out before close() rather than used after it: f.name() points
        // into the File object's own storage, which close() releases, so the
        // later sdRomNameValid()/strncpy would be reading freed memory — it
        // happens to still hold the name on some driver versions and hands back
        // garbage on others, which is exactly the bug that listed corrupt names.
        //
        // f.name() returns the basename on the ESP32 SD driver for entries
        // walked from an open directory, but a full path on some versions.
        // Take everything after the last '/' either way, so the listing is
        // always what sdRomPath() will later join back on.
        char name[SD_ROM_NAME_MAX];
        const char* raw = f.name();
        const char* slash = strrchr(raw, '/');
        if (slash) raw = slash + 1;
        // A name too long to copy cannot be listed anyway: sdRomNameValid()
        // rejects it on length, so it is emptied here and falls out below with
        // the same warning as any other unrepresentable name.
        const bool nameFits = strlen(raw) < (size_t)SD_ROM_NAME_MAX;
        if (nameFits) {
            strncpy(name, raw, SD_ROM_NAME_MAX - 1);
            name[SD_ROM_NAME_MAX - 1] = '\0';
        } else {
            name[0] = '\0';
        }

        const bool notRom = f.isDirectory() || !nameFits || !endsWithNes(name);
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

    // sdRomInit() creates this at boot, but a save can also run on a card that
    // was inserted afterwards and came back through the remount above — that
    // path never passes through the boot-time mkdir, so on a fresh card the
    // .part open below would fail into a bare IoError with the real cause (no
    // directory) invisible to the user.
    const bool dirMissing = !SD.exists(SD_ROMS_DIR);
    if (dirMissing && !SD.mkdir(SD_ROMS_DIR)) {
        Serial.printf("SD: cannot create %s\n", SD_ROMS_DIR);
        return recoverAfterIoFailure() ? SdStatus::IoError : SdStatus::NotMounted;
    }

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
    //
    // Why not remove(final) then rename(.part -> final): that pair has a window
    // where neither file exists, and a rename that fails inside it destroys the
    // ROM the user already had while not delivering the new one — the one
    // outcome an atomic-ish save must never produce. Moving the old image aside
    // instead keeps a complete copy on the card at every instant, so the failure
    // path can put it back.
    char bakPath[sizeof(finalPath) + sizeof(SD_BAK_SUFFIX)];
    snprintf(bakPath, sizeof(bakPath), "%s%s", finalPath, SD_BAK_SUFFIX);
    const bool overwriting = SD.exists(finalPath);
    if (overwriting) {
        // A .bak left by an earlier interrupted save would block the rename.
        SD.remove(bakPath);
        const bool stashed = SD.rename(finalPath, bakPath);
        if (!stashed) {
            SD.remove(partPath);
            Serial.printf("SD: cannot move %s aside\n", finalPath);
            return recoverAfterIoFailure() ? SdStatus::IoError : SdStatus::NotMounted;
        }
    }
    const bool renamed = SD.rename(partPath, finalPath);
    if (!renamed) {
        SD.remove(partPath);
        // Put the old image back under its own name, so a failed overwrite is a
        // no-op rather than a loss. If this restore fails too the image is still
        // on the card as .bak, which cleanupParts() no longer sweeps blindly.
        if (overwriting) SD.rename(bakPath, finalPath);
        Serial.printf("SD: rename %s -> %s failed\n", partPath, finalPath);
        return recoverAfterIoFailure() ? SdStatus::IoError : SdStatus::NotMounted;
    }
    // Only now is the new image safely in place under its final name.
    if (overwriting) SD.remove(bakPath);
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
