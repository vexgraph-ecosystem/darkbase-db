#include "wal/wal.h"
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#include "annotation/definition.h"
#include "annotation/overview.h"
#include "darkbase/type.h"
#include "exception/throw.h"
#include "io/file.h"
#include "nio/mem.h"
#include "util/crc32.h"

;;DEFINITION
/** The write-ahead log. A mutation is recorded here BEFORE the store touches its
 * data, and the log is synced BEFORE the mutation is acknowledged; on recovery the
 * records replay in order so work whose data pages never landed can be redone. That
 * ordering — record, sync, then mutate — is the whole of crash safety, and it is the
 * caller's discipline to keep, which is why `append` deliberately does not sync.
 *
 * An incomplete trailing record is DROPPED: a torn tail is exactly what a crash
 * mid-append looks like, and obeying it would apply half an intent. A complete
 * record whose checksum fails is CORRUPTION and is reported, never skipped — a hole
 * in a log destroys the order the log exists to guarantee.
 *
 * Bytes are written in memory order through explicit little-endian encoders rather
 * than by copying a struct, so the on-disk layout is the format definition itself
 * and cannot drift with struct padding. The host floor is little-endian, so no
 * byte-order detection or swapping exists anywhere in this file.
 *
 * IO is the engine's `File`; the checksum is the engine's `Crc32`. Neither is
 * reimplemented here, and nothing here allocates: append writes the record straight
 * from the caller's span, and replay streams payloads through a buffer the CALLER
 * supplies, refusing a record too large for it instead of truncating it. */
;;OVERVIEW
/** ========================================================================
 * CLASS: Wal (wal/wal.c)
 * STRUCT FIELDS (Mirroring wal/wal.h)
 *   void *file            engine File* owner (void* keeps this header IO-free)
 *   char path[1024]       the log's path, borrowed from the caller
 *   uint64_t baseSequence highest sequence that a checkpoint has retired
 *   uint64_t sequence     highest sequence written or recovered
 *   uint32_t count        records appended since the last checkpoint
 *   uint32_t recovered    records delivered by the last successful replay
 *   int32_t lastCode      last WalCode refusal
 *   bool tailTorn         recovery found an incomplete trailing record
 *   bool open             owner holds a live File
 * LAYOUT (byte-native, little-endian, memory order)
 *   header 24B: magic u32, version u16, reserved u16, baseSequence u64,
 *               reserved u32, crc u32 over the first 20
 *   record:     magic u32, op u32, sequence u64, length u32, payload[length],
 *               crc u32 over the 20 prefix bytes plus the payload
 * FUNCTION REGISTRY (exports)
 *   Constructors: Wal_0(void), Wal_1(path)         (create-or-open + recover)
 *   Lifecycle:    Wal_free, Wal_isOpen, Wal_path, Wal_code
 *   Appends:      Wal_append(self, op, payload, length, outSequence)
 *   Durability:   Wal_sync(self), Wal_checkpoint(self)
 *   Recovery:     Wal_replay(self, scratch, scratchCap, visit, context)
 *   Queries:      Wal_sequence, Wal_baseSequence, Wal_count, Wal_recovered,
 *                 Wal_tailTorn, Wal_codeName(code)
 *   Projections:  Wal_toString, Wal_toStringStruct
 * PRIVATE HELPERS
 *   putU32/putU64/getU32/getU64   explicit memory-order encoders (the format)
 *   encodeHeader/writeHeader      header bytes + checksum
 *   readExact/writeExact          short-read/short-write loops over File
 *   scanHeader(self)              verify and adopt the file header
 *   scanRecords(self, scratch, cap, visit, ctx)   the one recovery walk
 *   syncFile(self)                flush plus the host's fsync
 * CONTRACT: null/closed owners and illegal arguments are refused through THROW with
 * the owner untouched. A refusal never leaves a half-written record: the record is
 * validated before the first byte is written. `checkpoint` requires the caller to
 * have made the data durable first; the sequence never restarts after one.
 * ======================================================================== */

#define WAL_READ_CHUNK 4096u

static void putU32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t) value;
    out[1] = (uint8_t) (value >> 8);
    out[2] = (uint8_t) (value >> 16);
    out[3] = (uint8_t) (value >> 24);
}

static void putU64(uint8_t *out, uint64_t value) {
    for (unsigned i = 0u; i < 8u; i++)
        out[i] = (uint8_t) (value >> (i * 8u));
}

static uint32_t getU32(const uint8_t *in) {
    return (uint32_t) in[0] | ((uint32_t) in[1] << 8) | ((uint32_t) in[2] << 16) |
           ((uint32_t) in[3] << 24);
}

static uint64_t getU64(const uint8_t *in) {
    uint64_t value = 0u;
    for (unsigned i = 0u; i < 8u; i++)
        value |= (uint64_t) in[i] << (i * 8u);
    return value;
}

static bool readExact(File *file, void *dest, size_t length) {
    uint8_t *p = (uint8_t*) dest;
    size_t done = 0u;
    while (done < length) {
        int64_t n = File_read(file, p + done, (int64_t) (length - done));
        if (n <= 0)
            return false;
        done += (size_t) n;
    }
    return true;
}

static bool writeExact(File *file, const void *src, size_t length) {
    const uint8_t *p = (const uint8_t*) src;
    size_t done = 0u;
    while (done < length) {
        int64_t n = File_write(file, p + done, (int64_t) (length - done));
        if (n <= 0)
            return false;
        done += (size_t) n;
    }
    return true;
}

static bool syncFile(Wal *self) {
    File *file = (File*) (*self).file;
    if (!File_flush(file))
        return false;
#if defined(_WIN32)
    return _commit(_fileno(File_handle(file))) == 0;
#else
    return fsync(fileno(File_handle(file))) == 0;
#endif
}

static void encodeHeader(const Wal *self, uint8_t out[WAL_HEADER_BYTES]) {
    putU32(&out[0], WAL_MAGIC);
    out[4] = (uint8_t) WAL_VERSION;
    out[5] = 0u;
    out[6] = 0u;
    out[7] = 0u;
    putU64(&out[8], (*self).baseSequence);
    putU32(&out[16], 0u);
    putU32(&out[20], Crc32_of(out, 20u));
}

static bool writeHeader(Wal *self) {
    uint8_t header[WAL_HEADER_BYTES];
    encodeHeader(self, header);
    if (!File_seek((File*) (*self).file, 0))
        return false;
    return writeExact((File*) (*self).file, header, WAL_HEADER_BYTES);
}

static WalCode scanHeader(Wal *self) {
    uint8_t header[WAL_HEADER_BYTES];
    if (!File_seek((File*) (*self).file, 0))
        return WAL_IO;
    int64_t size = File_size((File*) (*self).file);
    if (size == 0) {
        // A brand-new log: write the header and start from sequence zero.
        (*self).baseSequence = 0u;
        (*self).sequence = 0u;
        (*self).count = 0u;
        if (!writeHeader(self))
            return WAL_IO;
        return WAL_OK;
    }
    if (size < (int64_t) WAL_HEADER_BYTES || !readExact((File*) (*self).file, header, WAL_HEADER_BYTES))
        return WAL_CORRUPT;
    if (getU32(&header[0]) != WAL_MAGIC || header[4] != (uint8_t) WAL_VERSION)
        return WAL_CORRUPT;
    if (getU32(&header[20]) != Crc32_of(header, 20u))
        return WAL_CORRUPT;
    (*self).baseSequence = getU64(&header[8]);
    (*self).sequence = (*self).baseSequence;
    (*self).count = 0u;
    return WAL_OK;
}

/**
 * The single recovery walk. With a null visitor it only validates and counts; with a
 * visitor it delivers each payload through `scratch`, which must hold the largest
 * record. Every complete record is checksum-verified and its sequence must follow
 * the previous one exactly, because a gap means the order cannot be trusted.
 */
static WalCode scanRecords(Wal *self, uint8_t *scratch, size_t scratchCap,
                           WalVisitFn visit, void *context, bool adopt) {
    File *file = (File*) (*self).file;
    uint64_t expected = (*self).baseSequence;
    uint32_t seen = 0u;
    uint32_t delivered = 0u;
    if (!File_seek(file, (int64_t) WAL_HEADER_BYTES))
        return WAL_IO;
    for (;;) {
        uint8_t prefix[WAL_RECORD_FIXED];
        uint8_t trailer[4];
        int64_t prefixBytes = File_read(file, prefix, (int64_t) WAL_RECORD_FIXED);
        if (prefixBytes <= 0)
            break;                       // clean end at a record boundary
        if (prefixBytes < (int64_t) WAL_RECORD_FIXED) {
            (*self).tailTorn = true;     // a crash tore this record's prefix
            break;
        }
        if (getU32(&prefix[0]) != WAL_MAGIC) {
            (*self).tailTorn = true;     // only the final prefix can be this broken
            break;
        }
        uint32_t op = getU32(&prefix[4]);
        uint64_t sequence = getU64(&prefix[8]);
        uint32_t length = getU32(&prefix[16]);
        if (op < WAL_OP_INSERT || op > WAL_OP_CHECKPOINT || length > WAL_PAYLOAD_MAX)
            return WAL_CORRUPT;
        if (sequence != expected + 1u)
            return WAL_CORRUPT;          // a gap or a repeat is not a valid log
        uint32_t crc = Crc32_update(CRC32_SEED, prefix, WAL_RECORD_FIXED);
        uint32_t remaining = length;
        size_t offset = 0u;
        while (remaining > 0u) {
            uint8_t chunk[WAL_READ_CHUNK];
            uint32_t want = remaining < WAL_READ_CHUNK ? remaining : WAL_READ_CHUNK;
            if (!readExact(file, chunk, want)) {
                (*self).tailTorn = true;
                break;
            }
            crc = Crc32_update(crc, chunk, want);
            if (scratch != nullptr && offset + want <= scratchCap) {
                memcpy(scratch + offset, chunk, want);
                offset += want;
            }
            remaining -= want;
        }
        if ((*self).tailTorn)
            break;
        if (!readExact(file, trailer, 4u)) {
            (*self).tailTorn = true;
            break;
        }
        if (Crc32_finalize(crc) != getU32(trailer))
            return WAL_CORRUPT;          // complete record, bad checksum: never skip
        if (visit != nullptr) {
            if (length > scratchCap)
                return WAL_INVALID;      // refused, not truncated
            WalRecord record = { op, sequence, scratch, length };
            ++delivered;
            if (!visit(context, &record))
                break;
        }
        expected = sequence;
        ++seen;
        // Only opening and recovery move the APPEND position. A replay is a
        // read-only walk: if it moved the position, a replay refused for a small
        // buffer would make the next append reuse a sequence.
        if (adopt) {
            (*self).sequence = sequence;
            (*self).count = seen;
        }
    }
    if (visit != nullptr)
        (*self).recovered = delivered;   // how many the visitor actually received
    return (*self).tailTorn ? WAL_TRUNCATED : WAL_OK;
}

static Wal *allocWal(const char *path) {
    Wal *self = (Wal*) Memory_alloc(TYPE_DB_WAL_SINGLETON, sizeof(Wal));
    if (self == nullptr) {
        THROW("Wal: allocation refused");
        return nullptr;
    }
    memset(self, 0, sizeof(*self));
    size_t length = strlen(path);
    if (length == 0u || length >= WAL_PATH_MAX) {
        THROW("Wal: path is empty or too long");
        Memory_free(self);
        return nullptr;
    }
    memcpy((*self).path, path, length + 1u);
    return self;
}

Wal *Wal_0(void) {
    Wal *self = (Wal*) Memory_alloc(TYPE_DB_WAL_SINGLETON, sizeof(Wal));
    if (self == nullptr) {
        THROW("Wal_0: allocation refused");
        return nullptr;
    }
    memset(self, 0, sizeof(*self));
    return self;
}

Wal *Wal_1(const char *path) {
    if (path == nullptr) {
        THROW("Wal_1: null path");
        return nullptr;
    }
    Wal *self = allocWal(path);
    if (self == nullptr)
        return nullptr;
    File *file = File_open((*self).path, FILE_MODE_READ | FILE_MODE_WRITE | FILE_MODE_CREATE);
    if (file == nullptr) {
        THROW("Wal_1: the log could not be opened");
        (*self).lastCode = WAL_IO;
        (*self).file = nullptr;
        return self;
    }
    (*self).file = file;
    (*self).open = true;
    WalCode code = scanHeader(self);
    if (code == WAL_OK)
        code = scanRecords(self, nullptr, 0u, nullptr, nullptr, true);
    (*self).lastCode = code;
    if (code == WAL_CORRUPT) {
        // The tail scan cannot corrupt anything, but a bad header cannot be used.
        THROW("Wal_1: the log header or a record is corrupt");
    }
    return self;
}

void Wal_free(Wal *self) {
    if (self == nullptr)
        return;
    if ((*self).file != nullptr)
        File_close((File*) (*self).file);
    Memory_free(self);
}

bool Wal_isOpen(const Wal *self) { return self != nullptr && (*self).open; }
const char *Wal_path(const Wal *self) { return self == nullptr ? "" : (*self).path; }
WalCode Wal_code(const Wal *self) { return self == nullptr ? WAL_INVALID : (WalCode) (*self).lastCode; }
uint64_t Wal_sequence(const Wal *self) { return self == nullptr ? 0u : (*self).sequence; }
uint64_t Wal_baseSequence(const Wal *self) { return self == nullptr ? 0u : (*self).baseSequence; }
uint32_t Wal_count(const Wal *self) { return self == nullptr ? 0u : (*self).count; }
uint32_t Wal_recovered(const Wal *self) { return self == nullptr ? 0u : (*self).recovered; }
bool Wal_tailTorn(const Wal *self) { return self != nullptr && (*self).tailTorn; }

WalCode Wal_append(Wal *self, uint32_t op, const uint8_t *payload, uint32_t length,
                   uint64_t *outSequence) {
    if (self == nullptr || !(*self).open) {
        THROW("Wal_append: the owner is not open");
        if (self != nullptr)
            (*self).lastCode = WAL_CLOSED;
        return WAL_CLOSED;
    }
    if (op < WAL_OP_INSERT || op > WAL_OP_CHECKPOINT ||
        (payload == nullptr && length != 0u) || length > WAL_PAYLOAD_MAX) {
        THROW("Wal_append: unknown op, null payload or an over-large payload");
        (*self).lastCode = WAL_INVALID;
        return WAL_INVALID;
    }
    File *file = (File*) (*self).file;
    uint64_t sequence = (*self).sequence + 1u;
    uint8_t prefix[WAL_RECORD_FIXED];
    putU32(&prefix[0], WAL_MAGIC);
    putU32(&prefix[4], op);
    putU64(&prefix[8], sequence);
    putU32(&prefix[16], length);
    uint32_t crc = Crc32_update(CRC32_SEED, prefix, WAL_RECORD_FIXED);
    crc = Crc32_update(crc, payload, length);
    uint8_t trailer[4];
    putU32(trailer, Crc32_finalize(crc));
    // Whole record validated above; only now does the file get touched. The end is
    // REFRESHED, not read from the cached size: after a checkpoint truncated the log
    // a stale size would position this record past the new end and leave a hole.
    if (!File_seek(file, File_refreshSize(file)) || !writeExact(file, prefix, WAL_RECORD_FIXED) ||
        (length > 0u && !writeExact(file, payload, length)) || !writeExact(file, trailer, 4u)) {
        THROW("Wal_append: the record could not be written");
        (*self).lastCode = WAL_IO;
        return WAL_IO;
    }
    ++(*self).count;
    (*self).sequence = sequence;
    if (outSequence != nullptr)
        *outSequence = sequence;
    (*self).lastCode = WAL_OK;
    return WAL_OK;
}

WalCode Wal_sync(Wal *self) {
    if (self == nullptr || !(*self).open) {
        THROW("Wal_sync: the owner is not open");
        return WAL_CLOSED;
    }
    if (!syncFile(self)) {
        THROW("Wal_sync: the log could not be made durable");
        (*self).lastCode = WAL_IO;
        return WAL_IO;
    }
    (*self).lastCode = WAL_OK;
    return WAL_OK;
}

WalCode Wal_replay(Wal *self, uint8_t *scratch, size_t scratchCap,
                   WalVisitFn visit, void *context) {
    if (self == nullptr || !(*self).open) {
        THROW("Wal_replay: the owner is not open");
        if (self != nullptr)
            (*self).lastCode = WAL_CLOSED;
        return WAL_CLOSED;
    }
    if (visit != nullptr && scratch == nullptr) {
        THROW("Wal_replay: a visitor was given no replay buffer");
        (*self).lastCode = WAL_INVALID;
        return WAL_INVALID;
    }
    (*self).tailTorn = false;
    // No header re-read and no adoption here: the append position was established at
    // open, and a replay must leave it exactly as it found it.
    WalCode code = scanRecords(self, scratch, scratchCap, visit, context, false);
    (*self).lastCode = code;
    return code;
}

WalCode Wal_checkpoint(Wal *self) {
    if (self == nullptr || !(*self).open) {
        THROW("Wal_checkpoint: the owner is not open");
        return WAL_CLOSED;
    }
    if (!syncFile(self)) {
        THROW("Wal_checkpoint: the log could not be synced");
        (*self).lastCode = WAL_IO;
        return WAL_IO;
    }
    // The caller has already made the DATA durable; only then is the log retired.
    (*self).baseSequence = (*self).sequence;
    (*self).count = 0u;
    if (!writeHeader(self)) {
        THROW("Wal_checkpoint: the header could not be rewritten");
        (*self).lastCode = WAL_IO;
        return WAL_IO;
    }
    File *file = (File*) (*self).file;
#if defined(_WIN32)
    if (_chsize_s(_fileno(File_handle(file)), (long long) WAL_HEADER_BYTES) != 0) {
#else
    if (ftruncate(fileno(File_handle(file)), (off_t) WAL_HEADER_BYTES) != 0) {
#endif
        THROW("Wal_checkpoint: the log could not be truncated");
        (*self).lastCode = WAL_IO;
        return WAL_IO;
    }
    // The truncation changed the file's length behind the handle's back.
    (void) File_refreshSize(file);
    if (!File_seek(file, (int64_t) WAL_HEADER_BYTES) || !syncFile(self)) {
        THROW("Wal_checkpoint: the truncated log could not be made durable");
        (*self).lastCode = WAL_IO;
        return WAL_IO;
    }
    (*self).lastCode = WAL_OK;
    return WAL_OK;
}

const char *Wal_codeName(WalCode code) {
    switch (code) {
        case WAL_OK:        return "ok";
        case WAL_INVALID:   return "invalid";
        case WAL_IO:        return "io";
        case WAL_CORRUPT:   return "corrupt";
        case WAL_TRUNCATED: return "truncated";
        case WAL_CLOSED:    return "closed";
        default:            return "unknown";
    }
}

bool Wal_toString(const Wal *self, char *dest, size_t capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }
    int written = snprintf(dest, capacity, "Wal(open=%d, sequence=%llu, count=%u, base=%llu)",
                           self != nullptr && (*self).open ? 1 : 0,
                           (unsigned long long) Wal_sequence(self), Wal_count(self),
                           (unsigned long long) Wal_baseSequence(self));
    bool cut = written < 0 || (size_t) written >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}

bool Wal_toStringStruct(const Wal *self, char *dest, size_t capacity, bool *outTruncated) {
    if (dest == nullptr || capacity == 0u) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        return false;
    }
    int written = snprintf(dest, capacity,
                           "Wal{path=%s, open=%d, baseSequence=%llu, sequence=%llu, count=%u, "
                           "recovered=%u, tailTorn=%d, lastCode=%s}",
                           self == nullptr ? "" : (*self).path,
                           self != nullptr && (*self).open ? 1 : 0,
                           (unsigned long long) Wal_baseSequence(self),
                           (unsigned long long) Wal_sequence(self), Wal_count(self),
                           Wal_recovered(self), self != nullptr && (*self).tailTorn ? 1 : 0,
                           Wal_codeName(self == nullptr ? WAL_INVALID : (*self).lastCode));
    bool cut = written < 0 || (size_t) written >= capacity;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    return !cut;
}
