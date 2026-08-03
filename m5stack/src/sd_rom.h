#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

// SD card ROM storage for the CoreS3.
//
// The card shares its SPI bus with the LCD (SCK/MISO/MOSI are the same pins,
// only the chip select differs), so every function here assumes the caller has
// already joined any outstanding band DMA and holds no open display
// transaction. That is a caller contract rather than something enforced here:
// the only way to check would be to reach into main.cpp's band state, and the
// two places that call these functions (setup() and loop()) are precisely the
// ones that own it. The UDP task on core 0 must never call into this file.

// Shared error code, reported identically by the menu, the type 4 ACK and the
// type 5 replies. One enum rather than per-path codes so the PC side can keep a
// single message table — the same reason UDP_ROM_STATUS_* is shared today.
enum class SdStatus : uint8_t {
    Ok = 0,
    NotMounted = 1,   // no card, unreadable filesystem, or a failed remount
    NotFound = 2,   // the named file does not exist
    NoSpace = 3,   // free space would not hold the image
    TooBig = 4,   // larger than ROM_MAX_SIZE, i.e. past what staging holds
    BadRom = 5,   // not iNES, or an unsupported mapper
    BadName = 6,   // empty / oversized / unrepresentable after sanitising
    Busy = 7,   // the staging buffer is owned by another transfer
    IoError = 8,   // read/write failed on a mounted card
    Exists = 9,   // rename target is already taken
};

struct SdRomEntry {
    char name[SD_ROM_NAME_MAX];
    uint32_t size;
};

// Mount the card. Returns false when there is none — which is not an error the
// caller has to handle beyond skipping SD features, so boot continues either
// way. Also creates SD_ROMS_DIR and sweeps stale ".part" files.
bool sdRomInit();

// Whether the last operation left the card usable. Cheap: a cached flag, not a
// probe, so the menu can ask it every frame.
bool sdRomMounted();

// Re-run SD.end() / SD.begin(). Used to recover from a card that was pulled and
// pushed back; also the one retry every failing operation makes before giving
// up with NotMounted.
bool sdRomRemount();

// Card capacity. Both outputs are set to 0 when nothing is mounted, so a caller
// can report "0 / 0" without a separate mounted test.
void sdRomSpace(uint64_t* totalBytes, uint64_t* freeBytes);

// List SD_ROMS_DIR's *.nes into `out`, sorted by name (case-insensitive).
// Returns the number written, or 0 when unmounted. Entries whose names do not
// fit SD_ROM_NAME_MAX, or that survive sanitising as something different, are
// skipped with a serial warning rather than listed under a name the caller
// could not then open.
int sdRomScan(SdRomEntry* out, int max);

// Read `name` into `buf`. `cap` is the buffer size; a file larger than that (or
// than ROM_MAX_SIZE) is rejected with TooBig without reading a byte.
SdStatus sdRomLoad(const char* name, uint8_t* buf, size_t cap, size_t* outSize);

// Write `size` bytes as `name`, overwriting any existing file of that name.
// Writes to "<name>.part" and renames on success, so a power cut during the
// write cannot leave a truncated image under a name the menu would offer.
SdStatus sdRomSave(const char* name, const uint8_t* buf, size_t size);

SdStatus sdRomDelete(const char* name);

// Rename within SD_ROMS_DIR. Refuses to overwrite: `Exists` is returned instead
// so the confirmation lives in the UI that has a user to ask.
SdStatus sdRomRename(const char* from, const char* to);

// Reduce an arbitrary name to something FAT and this firmware both accept:
// every byte outside [A-Za-z0-9._-] becomes '_', a ".nes" suffix is forced, and
// an empty result becomes "rom.nes". Only the basename survives, which is what
// makes path traversal structurally impossible — see the note in sd_rom.cpp.
//
// The result is at most `cap - 1` bytes either way: a name already ending in
// ".nes" is copied whole up to that limit, and one that needs the suffix has its
// body cut short first so the append still fits.
void sdRomSanitizeName(const char* in, char* out, size_t cap);

// Delete leftover "<name>.part" files. Called from sdRomInit(); exposed so a
// later caller can sweep again without remounting.
void sdRomCleanupParts();

// Human-readable form of a status, for serial logs and the menu's error line.
const char* sdStatusText(SdStatus status);
