// database/database.c — the L2 Database: the entity/row switchboard.

#include "database/database.h"
#include "util/crc32.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "annotation/definition.h"
#include "annotation/overview.h"
#include "oop/type.h"
#include "exception/throw.h"
#include "io/file.h"
#include "nio/mem.h"
#include "oop/type.h"
#include "reflection/struct.h"
#include "relational/variable_slot.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Database
 * ============================================================================
 * Database is the native vex store's in-memory switchboard and the R3 entry to
 * persistence. It registers reflection Structs as entities (tables) and binds
 * either caller-owned borrowed rows or Database-owned copies. This is the
 * "everything is a pointer" thesis applied to storage: an entity IS a Struct,
 * its columns ARE Fields (behavior + physical layout), a row is addressed by a
 * stable pointer, and Database is the name -> entity -> rows index.
 *
 * The entity registry is a never-moved ChunkedList of private DbEntity slot
 * records (name, borrowed schema, per-entity row list). Each row list is a
 * ChunkedList of void* with stable addresses, so a row pointer handed to a
 * DatabaseResult stays valid across growth until the Database is freed.
 *
 * Each entity has one ownership mode: borrowed rows from Database_insert, or
 * individually Memory_alloc'd rows from Database_insertOwned/Database_load.
 * The latter two share one canonical row type and Database_free releases them.
 * Cold rejections (null/illegal name, duplicate, absent) are recorded on the
 * instance and reported once through THROW (the Cold-Strict, Hot-Minimal
 * Validation Law; the THROW Law). The class is owner-affine: one writer at a
 * time; a caller excludes concurrent mutation while a DatabaseResult walks.
 *
 * Lifetime: arena-allocated (a constructor result). Database_free releases the
 * per-entity row lists and the registry, then the block; borrowed rows and
 * schemas are never freed here. Cold path only for the reflective name lookups
 * (the Cold-Only Reflection Law): consumers that need a bind repeatedly resolve
 * once and hold the index/slot.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Database (database/database.c)
 * ============================================================================
 * the L2 Database entity/row switchboard.
 *
 * STRUCT FIELDS (Mirroring database/database.h):
 * ----------------------------------------------------------------------------
 *   Database {
 *     ChunkedList *entities;           // DbEntity rows
 *     uint32_t chunkBytes;             // leaf budget for per-entity row lists
 *     int32_t lastCode;                // last DATABASE_* code
 *     char lastError[DB_ERROR_MESSAGE_MAX]; // last rejection text
 *   }
 *
 * PRIVATE HELPERS (kept file-local, pure logic):
 *   findEntity(self, folded)   // linear name lookup over the entity rows (static)
 *   recordError(self, code, message) // store the last result + text (static)
 *
 * SLOT RECORD (owned by Database, behaviorless; all behavior hangs off Database):
 *   DbEntity {
 *     char name[24];        // folded entity name (atom grammar)
 *     uint32_t owned;       // 1 = every row is Database-owned
 *     uint32_t pad;         // explicit padding
 *     Struct *schema;       // borrowed reflection layout (the entity)
 *     ChunkedList *rows;    // row pointers (void*); stable addresses
 *   }
 * PRIVATE HELPER RECORD:
 *   DbLoadStage {
 *     DbEntity *entity;    // target untouched entity during staged loading
 *     ChunkedList *rows;   // temporary complete owned-row list
 *   }
 *
 * PRIVATE HELPERS (persistence, pure logic):
 *   writeAll(file, src, length)             // short-write-safe (static)
 *   readAll(file, dest, length)             // short-read-safe (static)
 *   loadPayload(self, bytes, len, count)         // validate snapshot records (static)
 *   loadCommit(self, bytes, len, count)          // stage rows then publish atomically (static)
 *   freeOwnedRows(rows)                          // free staged owned copies (static)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h) Database_0(), _1(chunkBytes), _init, _free
 * Public Core Functions: (.h) _kind, _check, _define, _entityIndex, _entityCount,
 *                             _schema, _insert, _insertOwned, _row, _count, _select
 * Public Persistence: (.h) _save, _load
 * Public Diagnostics: (.h) _errorCode, _lastError, _errorText
 * Public String Projections: (.h) _toString / _toStringStruct
 * CONTRACT: schemas and borrowed rows outlive Database; owned rows are copied
 * at exact Struct.size and freed once by Database_free. An entity cannot mix
 * ownership modes. Load stages all owned rows before publishing any entity;
 * rejection preserves every target row list and ownership flag.
 * ============================================================================
 */

// PRIVATE SLOT RECORD

typedef struct DbEntity {
    char name[DB_ENTITY_NAME_BYTES]; // folded entity name (atom grammar)
    uint32_t owned;                  // 1 = every row is Database-owned
    uint32_t pad;                    // explicit padding
    Struct *schema;                  // borrowed reflection layout (the entity)
    ChunkedList *rows;               // row pointers (void*); stable addresses
} DbEntity;

// PRIVATE HELPER RECORD
typedef struct DbLoadStage {
    DbEntity *entity;       // target entity, unchanged until every row is ready
    ChunkedList *rows;      // complete temporary owned row list
} DbLoadStage;

// PRIVATE HELPERS

/**
 * Stores the latest error code and bounded message when the Database exists.
 * A null message clears the stored text; a null Database is ignored.
 */
static void recordError(Database *self, int32_t code, const char *message) {
    if (self == nullptr)
        return;
    (*self).lastCode = code;
    if (message == nullptr) {
        (*self).lastError[0] = '\0';
        return;
    }
    size_t len = strlen(message);
    size_t cap = sizeof((*self).lastError);
    size_t copy = len < cap - 1u ? len : cap - 1u;
    memcpy((*self).lastError, message, copy);
    (*self).lastError[copy] = '\0';
}

/**
 * Finds an entity by its already-folded name, returning its registry index or
 * -1 when the inputs, registry, or matching entity are absent.
 */
static int32_t findEntity(const Database *self, const char *folded) {
    if (self == nullptr || folded == nullptr || (*self).entities == nullptr)
        return -1;
    uint32_t n = ChunkedList_size((*self).entities);
    for (uint32_t i = 0u; i < n; i++) {
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, i);
        if (entity != nullptr && strcmp((*entity).name, folded) == 0)
            return (int32_t) i;
    }
    return -1;
}

// CONSTRUCTORS

/**
 * Initializes caller-provided storage and its entity registry. A zero or too-
 * small chunk budget selects the default; failure returns false.
 */
bool Database_init(Database *self, uint32_t chunkBytes) {
    if (self == nullptr)
        return false;
    memset(self, 0, sizeof(*self));
    uint32_t budget = chunkBytes == 0u ? VEX_CHUNKED_BYTES_DEFAULT : chunkBytes;
    if (budget < 16u)
        budget = VEX_CHUNKED_BYTES_DEFAULT;
    (*self).entities = ChunkedList_3(ID_DB_DATABASE, (uint32_t) sizeof(DbEntity), budget);
    if ((*self).entities == nullptr)
        return false;
    (*self).chunkBytes = budget;
    (*self).lastCode = DATABASE_OK;
    (*self).lastError[0] = '\0';
    return true;
}

/**
 * Allocates and initializes a Database using the requested per-entity chunk
 * budget, returning nullptr if allocation or initialization fails.
 */
Database *Database_1(uint32_t chunkBytes) {
    Database *self = (Database*) Memory_alloc(TYPE_DB_DATABASE_SINGLETON, sizeof(Database));
    if (self == nullptr)
        return nullptr;
    if (!Database_init(self, chunkBytes)) {
        Memory_free(self);
        return nullptr;
    }
    return self;
}

/**
 * Allocates and initializes a Database with the default chunk budget.
 */
Database *Database_0(void) {
    return Database_1(VEX_CHUNKED_BYTES_DEFAULT);
}

/**
 * Releases entity row lists and the Database allocation; only rows loaded from
 * persistence are owned and freed, while schemas and live rows are borrowed.
 */
void Database_free(Database *self) {
    if (self == nullptr)
        return;
    if ((*self).entities != nullptr) {
        uint32_t n = ChunkedList_size((*self).entities);
        for (uint32_t i = 0u; i < n; i++) {
            DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, i);
            if (entity == nullptr || (*entity).rows == nullptr)
                continue;
            if ((*entity).owned != 0u) {
                uint32_t rows = ChunkedList_size((*entity).rows);
                for (uint32_t r = 0u; r < rows; r++) {
                    uint8_t *slot = ChunkedList_slot((*entity).rows, r);
                    if (slot != nullptr)
                        Memory_free(*(void**) slot);
                }
            }
            ChunkedList_free((*entity).rows);
        }
        ChunkedList_free((*self).entities);
        (*self).entities = nullptr;
    }
    Memory_free(self);
}

// CORE FUNCTIONS

/**
 * Returns the allocation's runtime type id, or zero for a null Database.
 */
uint64_t Database_kind(const Database *self) {
    if (self == nullptr)
        return 0u;
    return Memory_type((void*) self);
}

/**
 * Reports whether a non-null Database allocation has the requested type id.
 */
bool Database_check(const Database *self, uint64_t typeId) {
    if (self == nullptr)
        return false;
    return Memory_type((void*) self) == typeId;
}

/**
 * Registers a borrowed reflection schema under its folded name and creates an
 * empty row list. Returns its index, or -1 with a recorded/reported error.
 */
int32_t Database_define(Database *self, Struct *schema) {
    if (self == nullptr || schema == nullptr) {
        recordError(self, DATABASE_INVALID, "define: null database or schema");
        THROW("Database_define: null database or schema");
        return -1;
    }
    char name[DB_ENTITY_NAME_BYTES];
    if (Struct_getName(schema, name, sizeof(name)) < 0) {
        recordError(self, DATABASE_INVALID, "define: schema has no valid name");
        THROW("Database_define: schema has no valid name");
        return -1;
    }
    char folded[DB_ENTITY_NAME_BYTES];
    if (!VariableSlot_foldName(name, folded)) {
        recordError(self, DATABASE_INVALID, "define: illegal entity name");
        THROW("Database_define: illegal entity name");
        return -1;
    }
    if (findEntity(self, folded) >= 0) {
        recordError(self, DATABASE_DUPLICATE, "define: entity already registered");
        THROW("Database_define: duplicate entity");
        return -1;
    }
    uint8_t *slot = ChunkedList_addSlot((*self).entities);
    if (slot == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "define: entity allocation failed");
        THROW("Database_define: entity allocation failed");
        return -1;
    }
    DbEntity *entity = (DbEntity*) slot;
    memcpy((*entity).name, folded, strlen(folded) + 1u);
    (*entity).schema = schema;
    (*entity).rows = ChunkedList_3(ID_DB_DATABASE_ROW, (uint32_t) sizeof(void*), (*self).chunkBytes);
    if ((*entity).rows == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "define: row list allocation failed");
        THROW("Database_define: row list allocation failed");
        return -1;
    }
    recordError(self, DATABASE_OK, "");
    return (int32_t) (ChunkedList_size((*self).entities) - 1u);
}

/**
 * Resolves a valid entity name to its registry index, or returns -1 when the
 * Database/name is null, malformed, or not registered.
 */
int32_t Database_entityIndex(const Database *self, const char *entity) {
    if (self == nullptr || entity == nullptr)
        return -1;
    char folded[DB_ENTITY_NAME_BYTES];
    if (!VariableSlot_foldName(entity, folded))
        return -1;
    return findEntity(self, folded);
}

/**
 * Returns the number of registered entities, or zero for an unavailable
 * registry.
 */
uint32_t Database_entityCount(const Database *self) {
    if (self == nullptr || (*self).entities == nullptr)
        return 0u;
    return ChunkedList_size((*self).entities);
}

/**
 * Returns the borrowed schema for a registered entity, or nullptr if lookup
 * fails.
 */
Struct *Database_schema(const Database *self, const char *entity) {
    int32_t index = Database_entityIndex(self, entity);
    if (index < 0)
        return nullptr;
    DbEntity *row = (DbEntity*) ChunkedList_slot((*self).entities, (uint32_t) index);
    return row ? (*row).schema : nullptr;
}

/**
 * Binds a caller-owned row pointer to a live entity and returns its row index.
 * Rejections leave the row unbound and return -1 with a recorded error.
 */
int32_t Database_insert(Database *self, const char *entity, void *row) {
    if (self == nullptr || entity == nullptr || row == nullptr) {
        recordError(self, DATABASE_INVALID, "insert: null argument");
        THROW("Database_insert: null argument");
        return -1;
    }
    char folded[DB_ENTITY_NAME_BYTES];
    if (!VariableSlot_foldName(entity, folded)) {
        recordError(self, DATABASE_INVALID, "insert: illegal entity name");
        THROW("Database_insert: illegal entity name");
        return -1;
    }
    int32_t index = findEntity(self, folded);
    if (index < 0) {
        recordError(self, DATABASE_ABSENT, "insert: no such entity");
        THROW("Database_insert: no such entity");
        return -1;
    }
    DbEntity *target = (DbEntity*) ChunkedList_slot((*self).entities, (uint32_t) index);
    if (target == nullptr || (*target).rows == nullptr) {
        recordError(self, DATABASE_ABSENT, "insert: entity unavailable");
        THROW("Database_insert: entity unavailable");
        return -1;
    }
    if ((*target).owned != 0u) {
        recordError(self, DATABASE_INVALID, "insert: entity holds loaded rows");
        THROW("Database_insert: cannot bind a live row into a file-loaded entity");
        return -1;
    }
    uint8_t *slot = ChunkedList_addSlot((*target).rows);
    if (slot == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "insert: row allocation failed");
        THROW("Database_insert: row allocation failed");
        return -1;
    }
    *((void**) slot) = row;
    recordError(self, DATABASE_OK, "");
    return (int32_t) (ChunkedList_size((*target).rows) - 1u);
}

/**
 * Copies one exact-stride row into Database ownership. The entity must be empty
 * or already own all its rows; rejection frees any staged copy and preserves
 * both row count and ownership mode.
 */
int32_t Database_insertOwned(Database *self, const char *entity, const void *rowBytes,
                             size_t rowBytesLength) {
    if (self == nullptr || entity == nullptr || rowBytes == nullptr) {
        recordError(self, DATABASE_INVALID, "insertOwned: null argument");
        THROW("Database_insertOwned: null argument");
        return -1;
    }
    char folded[DB_ENTITY_NAME_BYTES];
    if (!VariableSlot_foldName(entity, folded)) {
        recordError(self, DATABASE_INVALID, "insertOwned: illegal entity name");
        THROW("Database_insertOwned: illegal entity name");
        return -1;
    }
    int32_t index = findEntity(self, folded);
    if (index < 0) {
        recordError(self, DATABASE_ABSENT, "insertOwned: no such entity");
        THROW("Database_insertOwned: no such entity");
        return -1;
    }
    DbEntity *target = (DbEntity*) ChunkedList_slot((*self).entities, (uint32_t) index);
    if (target == nullptr || (*target).schema == nullptr || (*target).rows == nullptr) {
        recordError(self, DATABASE_ABSENT, "insertOwned: entity unavailable");
        THROW("Database_insertOwned: entity unavailable");
        return -1;
    }
    uint32_t stride = Struct_getSize((*target).schema);
    if (stride == 0u || rowBytesLength != (size_t) stride) {
        recordError(self, DATABASE_INVALID, "insertOwned: row length must equal schema stride");
        THROW("Database_insertOwned: row length does not equal schema stride");
        return -1;
    }
    uint32_t count = ChunkedList_size((*target).rows);
    if ((*target).owned == 0u && count != 0u) {
        recordError(self, DATABASE_INVALID, "insertOwned: entity already has borrowed rows");
        THROW("Database_insertOwned: cannot mix borrowed and owned rows");
        return -1;
    }
    void *copy = Memory_alloc(TYPE_DB_DATABASE_ROW_SINGLETON, rowBytesLength);
    if (copy == nullptr) {
        recordError(self, DATABASE_EXHAUSTED, "insertOwned: row allocation failed");
        THROW("Database_insertOwned: row allocation failed");
        return -1;
    }
    memcpy(copy, rowBytes, rowBytesLength);
    uint8_t *slot = ChunkedList_addSlot((*target).rows);
    if (slot == nullptr) {
        Memory_free(copy);
        recordError(self, DATABASE_EXHAUSTED, "insertOwned: row-list allocation failed");
        THROW("Database_insertOwned: row-list allocation failed");
        return -1;
    }
    *((void**) slot) = copy;
    (*target).owned = 1u;
    recordError(self, DATABASE_OK, "");
    return (int32_t) (ChunkedList_size((*target).rows) - 1u);
}

/**
 * Returns the borrowed row pointer at an entity's index, or nullptr when the
 * entity, row list, index, or stored pointer is unavailable.
 */
void *Database_row(const Database *self, const char *entity, uint32_t index) {
    int32_t e = Database_entityIndex(self, entity);
    if (e < 0)
        return nullptr;
    DbEntity *target = (DbEntity*) ChunkedList_slot((*self).entities, (uint32_t) e);
    if (target == nullptr || (*target).rows == nullptr)
        return nullptr;
    uint8_t *slot = ChunkedList_slot((*target).rows, index);
    return slot ? *((void**) slot) : nullptr;
}

/**
 * Returns the number of rows bound to an entity, or zero when lookup fails.
 */
uint32_t Database_count(const Database *self, const char *entity) {
    int32_t e = Database_entityIndex(self, entity);
    if (e < 0)
        return 0u;
    DbEntity *target = (DbEntity*) ChunkedList_slot((*self).entities, (uint32_t) e);
    return target && (*target).rows ? ChunkedList_size((*target).rows) : 0u;
}

/**
 * Creates a cursor over an entity's current row list; absent/unavailable
 * entities return nullptr and update the Database error state.
 */
DatabaseResult *Database_select(Database *self, const char *entity) {
    int32_t e = Database_entityIndex(self, entity);
    if (e < 0) {
        recordError(self, DATABASE_ABSENT, "select: no such entity");
        THROW("Database_select: no such entity");
        return nullptr;
    }
    DbEntity *target = (DbEntity*) ChunkedList_slot((*self).entities, (uint32_t) e);
    if (target == nullptr || (*target).rows == nullptr) {
        recordError(self, DATABASE_ABSENT, "select: entity unavailable");
        THROW("Database_select: entity unavailable");
        return nullptr;
    }
    recordError(self, DATABASE_OK, "");
    return DatabaseResult_1((*target).rows);
}

// DIAGNOSTICS

/**
 * Returns the latest Database error code, using DATABASE_INVALID for nullptr.
 */
int32_t Database_errorCode(const Database *self) {
    return self ? (*self).lastCode : DATABASE_INVALID;
}

/**
 * Copies the last error text into a bounded destination and returns copied
 * bytes, or -1 for invalid arguments; truncation is NUL-terminated.
 */
int Database_lastError(const Database *self, char *out, size_t outCap) {
    if (self == nullptr || out == nullptr || outCap == 0u)
        return -1;
    size_t len = strlen((*self).lastError);
    size_t copy = len < outCap - 1u ? len : outCap - 1u;
    memcpy(out, (*self).lastError, copy);
    out[copy] = '\0';
    return (int) copy;
}

/**
 * Maps a Database error code to static human-readable text, including unknown
 * values.
 */
const char *Database_errorText(int32_t code) {
    switch (code) {
        case DATABASE_OK:        return "ok";
        case DATABASE_INVALID:   return "invalid argument";
        case DATABASE_DUPLICATE: return "duplicate entity";
        case DATABASE_ABSENT:    return "absent entity";
        case DATABASE_EXHAUSTED: return "allocation exhausted";
        default:                 return "unknown";
    }
}

// PERSISTENCE (M2, .vexdb)

#define VEX_DB_MAGIC "VEXDB01"
#define VEX_DB_VERSION 1u
#define VEX_DB_HEADER_BYTES 64u
#define VEX_DB_TRAILER_BYTES 4u

// INTENTIONAL(vex): NO ENDIANNESS NEEDED!! We just write the value bytes in
// memory order (left to right) and call it a day — not laziness, but because a
// byte[] right-to-the-point is more native than byte-swap machinery and needs
// no complication. Every supported host is little-endian (Apple Silicon arm64
// floor; Windows x86_64), so there is no cross-endian peer to serve. Values are
// raw uint8_t[] spans — nothing is encoded as text. toString() is a cold
// converter for humans, never a storage format.

// File header (64 Bytes). A CRC32 of the payload (every byte between the header
// and the trailer) is written as a 4-byte trailer, so a reader validates the
// whole file before mutating the registry.
typedef struct VexDbHeader {
    char magic[8];         // "VEXDB01\0"
    uint32_t version;      // VEX_DB_VERSION
    uint32_t entityCount;  // entity records that follow
    uint64_t rowCount;     // total rows across entities
    uint32_t reserved0;
    uint32_t reserved1;
    uint8_t pad[32];       // to 64
} VexDbHeader;
_Static_assert(sizeof(VexDbHeader) == VEX_DB_HEADER_BYTES, "VexDbHeader must stay 64 Bytes");

// One entity record (32 Bytes): rowCount * stride raw row Bytes follow it.
typedef struct VexDbEntity {
    char name[DB_ENTITY_NAME_BYTES]; // folded entity name
    uint32_t stride;                 // entity row byte stride
    uint32_t rowCount;               // rows that follow
} VexDbEntity;
_Static_assert(sizeof(VexDbEntity) == 32u, "VexDbEntity must stay 32 Bytes");

/**
 * Writes the full byte span, retrying short writes; returns false on a
 * non-progressing or failed write.
 */
static bool writeAll(File *file, const void *src, size_t length) {
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

/**
 * Reads the requested byte span, retrying short reads; returns false on EOF or
 * a failed read before completion.
 */
static bool readAll(File *file, void *dest, size_t length) {
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

/**
 * Flushes the File stream and fsyncs its OS handle before publication. This is
 * implemented for the macOS/POSIX floor; Windows durability is not provided.
 */
static bool syncFile(File *file) {
    if (file == nullptr || File_flush(file) != true)
        return false;
    FILE *handle = File_handle(file);
    if (handle == nullptr)
        return false;
    return fsync(fileno(handle)) == 0;
}

/** Checks payload records against registered, empty entities without mutation. */
static int32_t loadPayload(Database *self, const uint8_t *payload, size_t length,
                           uint32_t entityCount) {
    size_t offset = 0u;
    for (uint32_t e = 0u; e < entityCount; e++) {
        if (offset > length || length - offset < sizeof(VexDbEntity))
            return DATABASE_INVALID;
        VexDbEntity record;
        memcpy(&record, payload + offset, sizeof(record));
        offset += sizeof(record);
        if (record.stride == 0u)
            return DATABASE_INVALID;
        if (memchr(record.name, '\0', sizeof(record.name)) == nullptr)
            return DATABASE_INVALID;
        size_t rowBytes = (size_t) record.stride * (size_t) record.rowCount;
        if (rowBytes > length || offset > length - rowBytes)
            return DATABASE_INVALID;
        int32_t index = Database_entityIndex(self, record.name);
        if (index < 0)
            return DATABASE_ABSENT;
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, (uint32_t) index);
        if (entity == nullptr || (*entity).schema == nullptr || (*entity).rows == nullptr)
            return DATABASE_ABSENT;
        if (Struct_getSize((*entity).schema) != record.stride)
            return DATABASE_INVALID;
        if (ChunkedList_size((*entity).rows) != 0u)
            return DATABASE_DUPLICATE;
        for (uint32_t prior = 0u; prior < e; prior++) {
            size_t previousOffset = 0u;
            VexDbEntity previous = {0};
            for (uint32_t p = 0u; p <= prior; p++) {
                memcpy(&previous, payload + previousOffset, sizeof(previous));
                if (p == prior)
                    break;
                previousOffset += sizeof(previous) +
                    (size_t) previous.stride * (size_t) previous.rowCount;
            }
            if (memchr(previous.name, '\0', sizeof(previous.name)) == nullptr)
                return DATABASE_INVALID;
            if (strcmp(previous.name, record.name) == 0)
                return DATABASE_DUPLICATE;
        }
        offset += rowBytes;
    }
    return offset == length ? DATABASE_OK : DATABASE_INVALID;
}

/** Frees every owned row in a temporary list and then releases the list. */
static void freeOwnedRows(ChunkedList *rows) {
    if (rows == nullptr)
        return;
    uint32_t count = ChunkedList_size(rows);
    for (uint32_t i = 0u; i < count; i++) {
        uint8_t *slot = ChunkedList_slot(rows, i);
        if (slot != nullptr)
            Memory_free(*(void**) slot);
    }
    ChunkedList_free(rows);
}

/**
 * Builds every entity's rows off to the side, then swaps all lists only after
 * every allocation succeeds. Rejection leaves every target entity untouched.
 */
static int32_t loadCommit(Database *self, const uint8_t *payload, size_t length,
                          uint32_t entityCount) {
    if (entityCount == 0u)
        return length == 0u ? DATABASE_OK : DATABASE_INVALID;
    if ((size_t) entityCount > SIZE_MAX / sizeof(DbLoadStage))
        return DATABASE_EXHAUSTED;
    size_t stageBytes = (size_t) entityCount * sizeof(DbLoadStage);
    DbLoadStage *stages = (DbLoadStage*) Memory_alloc(0u, stageBytes);
    if (stages == nullptr)
        return DATABASE_EXHAUSTED;
    memset(stages, 0, stageBytes);

    int32_t status = DATABASE_OK;
    uint32_t staged = 0u;
    size_t offset = 0u;
    for (uint32_t e = 0u; e < entityCount && status == DATABASE_OK; e++) {
        VexDbEntity record;
        memcpy(&record, payload + offset, sizeof(record));
        offset += sizeof(record);
        int32_t index = Database_entityIndex(self, record.name);
        DbEntity *entity = index < 0 ? nullptr : (DbEntity*) ChunkedList_slot(
            (*self).entities, (uint32_t) index);
        if (entity == nullptr) {
            status = DATABASE_ABSENT;
            break;
        }
        for (uint32_t p = 0u; p < staged; p++)
            if (stages[p].entity == entity)
                status = DATABASE_DUPLICATE;
        if (status != DATABASE_OK)
            break;

        ChunkedList *rows = ChunkedList_3(ID_DB_DATABASE_ROW,
            (uint32_t) sizeof(void*), (*self).chunkBytes);
        if (rows == nullptr) {
            status = DATABASE_EXHAUSTED;
            break;
        }
        stages[staged].entity = entity;
        stages[staged].rows = rows;
        staged++;
        if (!ChunkedList_reserve(rows, record.rowCount)) {
            status = DATABASE_EXHAUSTED;
            break;
        }
        for (uint32_t r = 0u; r < record.rowCount; r++) {
            void *copy = Memory_alloc(TYPE_DB_DATABASE_ROW_SINGLETON, record.stride);
            if (copy == nullptr) {
                status = DATABASE_EXHAUSTED;
                break;
            }
            memcpy(copy, payload + offset + (size_t) r * record.stride, record.stride);
            uint8_t *slot = ChunkedList_addSlot(rows);
            if (slot == nullptr) {
                Memory_free(copy);
                status = DATABASE_EXHAUSTED;
                break;
            }
            *((void**) slot) = copy;
        }
        offset += (size_t) record.stride * (size_t) record.rowCount;
    }
    if (status == DATABASE_OK && offset != length)
        status = DATABASE_INVALID;

    if (status == DATABASE_OK) {
        for (uint32_t i = 0u; i < staged; i++) {
            DbEntity *entity = stages[i].entity;
            ChunkedList *oldRows = (*entity).rows;
            (*entity).rows = stages[i].rows;
            (*entity).owned = 1u;
            stages[i].rows = nullptr;
            ChunkedList_free(oldRows);
        }
    }
    for (uint32_t i = 0u; i < staged; i++)
        freeOwnedRows(stages[i].rows);
    Memory_free(stages);
    return status;
}

/**
 * Writes a CRC-protected native-endian snapshot to a temporary sibling, syncs
 * it, then renames it over the destination; failures preserve the old file.
 */
int32_t Database_save(Database *self, const char *path) {
    if (self == nullptr || path == nullptr) {
        recordError(self, DATABASE_INVALID, "save: null database or path");
        THROW("Database_save: null database or path");
        return DATABASE_INVALID;
    }
    uint32_t entities = Database_entityCount(self);
    uint64_t rowTotal = 0u;
    for (uint32_t i = 0u; i < entities; i++) {
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, i);
        if (entity == nullptr || (*entity).schema == nullptr || Struct_getSize((*entity).schema) == 0u) {
            recordError(self, DATABASE_INVALID, "save: entity has no row stride");
            THROW("Database_save: entity has no row stride");
            return DATABASE_INVALID;
        }
        rowTotal += (*entity).rows ? ChunkedList_size((*entity).rows) : 0u;
    }
    // Publish atomically: write a temporary sibling, fsync it, then rename it
    // over the destination. A failed or interrupted write leaves the previous
    // snapshot intact; the destination is only ever the old or the new file.
    char tmp[FILE_PATH_MAX + 32];
    int tmpLen = snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long) getpid());
    if (tmpLen < 0 || (size_t) tmpLen >= sizeof(tmp)) {
        recordError(self, DATABASE_INVALID, "save: path too long");
        THROW("Database_save: path too long");
        return DATABASE_INVALID;
    }
    File *file = File_open(tmp, FILE_MODE_WRITE | FILE_MODE_CREATE | FILE_MODE_TRUNCATE);
    if (file == nullptr) {
        recordError(self, DATABASE_INVALID, "save: cannot open temp file");
        THROW("Database_save: cannot open temp file");
        return DATABASE_INVALID;
    }
    VexDbHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, VEX_DB_MAGIC, 7u);
    header.version = VEX_DB_VERSION;
    header.entityCount = entities;
    header.rowCount = rowTotal;
    uint32_t crc = CRC32_SEED;
    bool ok = writeAll(file, &header, sizeof(header));
    for (uint32_t i = 0u; ok && i < entities; i++) {
        DbEntity *entity = (DbEntity*) ChunkedList_slot((*self).entities, i);
        uint32_t stride = Struct_getSize((*entity).schema);
        uint32_t count = (*entity).rows ? ChunkedList_size((*entity).rows) : 0u;
        VexDbEntity record;
        memset(&record, 0, sizeof(record));
        memcpy(record.name, (*entity).name, DB_ENTITY_NAME_BYTES);
        record.stride = stride;
        record.rowCount = count;
        ok = writeAll(file, &record, sizeof(record));
        crc = Crc32_update(crc, &record, sizeof(record));
        for (uint32_t r = 0u; ok && r < count; r++) {
            uint8_t *slot = ChunkedList_slot((*entity).rows, r);
            void *row = slot ? *(void**) slot : nullptr;
            if (row == nullptr) {
                ok = false;
                break;
            }
            ok = writeAll(file, row, stride);
            crc = Crc32_update(crc, row, stride);
        }
    }
    uint32_t trailer = Crc32_finalize(crc);
    if (ok)
        ok = writeAll(file, &trailer, VEX_DB_TRAILER_BYTES);
    if (ok)
        ok = syncFile(file);
    File_close(file);
    if (!ok) {
        remove(tmp);
        recordError(self, DATABASE_EXHAUSTED, "save: write failed");
        THROW("Database_save: write failed");
        return DATABASE_EXHAUSTED;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        recordError(self, DATABASE_EXHAUSTED, "save: publish failed");
        THROW("Database_save: publish rename failed");
        return DATABASE_EXHAUSTED;
    }
    recordError(self, DATABASE_OK, "");
    return DATABASE_OK;
}

/**
 * Validates a snapshot header, payload checksum, registered schemas, and empty
 * target entities before loading rows as Database-owned arena allocations.
 */
int32_t Database_load(Database *self, const char *path) {
    if (self == nullptr || path == nullptr) {
        recordError(self, DATABASE_INVALID, "load: null database or path");
        THROW("Database_load: null database or path");
        return DATABASE_INVALID;
    }
    File *file = File_open(path, FILE_MODE_READ);
    if (file == nullptr) {
        recordError(self, DATABASE_INVALID, "load: cannot open file");
        THROW("Database_load: cannot open file");
        return DATABASE_INVALID;
    }
    VexDbHeader header;
    if (!readAll(file, &header, sizeof(header)) ||
        memcmp(header.magic, VEX_DB_MAGIC, 7u) != 0 ||
        header.version != VEX_DB_VERSION) {
        File_close(file);
        recordError(self, DATABASE_INVALID, "load: bad header");
        THROW("Database_load: bad header");
        return DATABASE_INVALID;
    }
    int64_t size = File_size(file);
    int64_t payloadLen = size - (int64_t) VEX_DB_HEADER_BYTES - (int64_t) VEX_DB_TRAILER_BYTES;
    if (size < (int64_t) (VEX_DB_HEADER_BYTES + VEX_DB_TRAILER_BYTES) || payloadLen < 0) {
        File_close(file);
        recordError(self, DATABASE_INVALID, "load: truncated file");
        THROW("Database_load: truncated file");
        return DATABASE_INVALID;
    }
    uint8_t *payload = nullptr;
    if (payloadLen > 0) {
        payload = (uint8_t*) Memory_alloc(0u, (size_t) payloadLen);
        if (payload == nullptr) {
            File_close(file);
            recordError(self, DATABASE_EXHAUSTED, "load: payload allocation failed");
            THROW("Database_load: payload allocation failed");
            return DATABASE_EXHAUSTED;
        }
        if (!readAll(file, payload, (size_t) payloadLen)) {
            Memory_free(payload);
            File_close(file);
            recordError(self, DATABASE_INVALID, "load: payload read failed");
            THROW("Database_load: payload read failed");
            return DATABASE_INVALID;
        }
    }
    uint32_t trailer = 0u;
    bool haveTrailer = readAll(file, &trailer, VEX_DB_TRAILER_BYTES);
    File_close(file);
    if (!haveTrailer) {
        if (payload)
            Memory_free(payload);
        recordError(self, DATABASE_INVALID, "load: missing checksum");
        THROW("Database_load: missing checksum");
        return DATABASE_INVALID;
    }
    uint32_t crc = Crc32_finalize(Crc32_update(CRC32_SEED, payload, (size_t) payloadLen));
    if (crc != trailer) {
        if (payload)
            Memory_free(payload);
        recordError(self, DATABASE_INVALID, "load: checksum mismatch");
        THROW("Database_load: checksum mismatch");
        return DATABASE_INVALID;
    }
    int32_t status = loadPayload(self, payload, (size_t) payloadLen, header.entityCount);
    if (status == DATABASE_OK)
        status = loadCommit(self, payload, (size_t) payloadLen, header.entityCount);
    if (payload)
        Memory_free(payload);
    if (status != DATABASE_OK) {
        recordError(self, status, "load: payload rejected");
        THROW("Database_load: payload rejected");
        return status;
    }
    recordError(self, DATABASE_OK, "");
    return DATABASE_OK;
}

// STRING PROJECTIONS (the toString Law)

/**
 * Formats a bounded value summary of the Database; null self formats as
 * "nullptr" and reports truncation through outTruncated when supplied.
 */
void Database_toString(const Database *self, char *dest, size_t cap, bool *outTruncated) {
    if (outTruncated)
        *outTruncated = false;
    if (dest == nullptr || cap == 0u)
        return;
    if (self == nullptr) {
        snprintf(dest, cap, "nullptr");
        return;
    }
    int written = snprintf(dest, cap, "Database(entities=%u)", Database_entityCount(self));
    if (written < 0 || (size_t) written >= cap) {
        if (outTruncated)
            *outTruncated = true;
    }
}

/**
 * Formats the Database's own fields into a bounded structure summary; null
 * self formats as "nullptr" and reports truncation when possible.
 */
void Database_toStringStruct(const Database *self, char *dest, size_t cap, bool *outTruncated) {
    if (outTruncated)
        *outTruncated = false;
    if (dest == nullptr || cap == 0u)
        return;
    if (self == nullptr) {
        snprintf(dest, cap, "nullptr");
        return;
    }
    int written = snprintf(dest, cap, "Database { entities=0x%llx, entityCount=%u, chunkBytes=%u, lastCode=%d }",
                           (unsigned long long) (uintptr_t) (*self).entities, Database_entityCount(self),
                           (*self).chunkBytes, (*self).lastCode);
    if (written < 0 || (size_t) written >= cap) {
        if (outTruncated)
            *outTruncated = true;
    }
}
