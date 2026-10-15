# darkbase — Repo-Local Living Preferences
> Repo-local preferences governed by the Living Documentation Law.
> Universal Supreme Constitution: workspace gist/PREFERENCES.md, published on Gist.

## 0. Constitution Link (supreme)
- [preferences.md](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a) — real, Git-ignored workspace file at `../../gist/PREFERENCES.md`, not a tracked Vexspoke file or symlink.
- All universal laws in `../../gist/PREFERENCES.md` are mandatory and binding across the ecosystem.
- This document codifies **exclusive** preferences for `darkbase` (R3 Database Driver). Database semantics and persistence remain R3; allocation/storage and native span search belong to Relational Engine R2, with Vexspoke supplying CPU computation/behavior. Either R2 public contract may be borrowed. This blueprint has no implemented engine integration; migration and durability require owner proof.
- darkbase is a **native vex store, not a SQL engine**. The schema is C reflection; the native query contract is structured, and any text/SQL parser is an optional `cli/` convenience or a foreign driver's private translation.

## 1. Repo-Local Law Index (Binding Matrix)

Universal laws are inherited from the canonical `../../gist/PREFERENCES.md` Index; this table indexes the additional laws specific to this repository.

| Law Title | Scope | Enforcement |
| :--- | :--- | :--- |
| **Entity Model Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Reactive Program Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Store Versus Executable Separation Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Byte-Native Persistence Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Zero-Alloc Relational Table Scan Law** | R3 Database Driver | Mandatory for `darkbase` |
| **B-Tree Page Consistency Law** | R3 Database Driver | Mandatory for `darkbase` |
| **Row Borrow and Owned Copy Law** | R3 Database row ownership | Mandatory for `darkbase` |
| **Durable Page Store Boundary Law** | Single-file page-level Store | Embedded redo ordering, reopen, refusal and reuse owner proof |

## 2. Exclusive Repo-Local Laws (FULL PROSE RESTATEMENT)

### Entity Model Law

#### Definition:
darkbase defines no schema types of its own; it consumes R2 reflection. A
`struct` is a `Struct` (a table: an ordered `Field` list, whose `size` is the
entity row stride), a `class` is a `Class` (a `Struct` plus a constructor and
`Method`s), a `field` is a `Field`, and a `function` is a `Method`. `Field`
carries both behavior (name + read/set + target) and physical layout (typeId,
offset, size, flags), so one record serves live reflection, persistence and
export. Plain values are named typed bindings in the same scope.

#### The Why:
A schema expressed as C types needs no DDL text and cannot drift from the struct
it describes. One shared vocabulary lets rows, callable behavior, and plain
values live in one findable namespace and be read the same way by reflection,
the store, scripts, debuggers and the UI. It is the Relational Engine's "name to
value" thesis carried through to persistence.

#### The Rule:
1. **Reflection IS the schema.** Structs are entities, Fields are entity fields, Methods are functions. No parallel ad-hoc schema language, and no darkbase-owned descriptor duplicating `Field`.
2. **Field owns the physical layout.** Persistence and export read offset, size and typeId from the `Field` (or the `Struct`'s row size), never from a hand-maintained table. Vexspoke's reflection `Field` is the single source of truth; a need for more reads it there first.
3. **One binding namespace.** Structs, Methods, and plain values resolve through the same scope and naming grammar.

---

### Row Borrow and Owned Copy Law

#### Definition:
`Database_insert` stores a borrowed row pointer; the caller keeps that row and
its schema alive for the Database lifetime. `Database_insertOwned` copies one
exact `Struct.size` byte row into engine-owned memory. A given entity has one
row ownership mode and never mixes borrowed and owned pointers. Loaded rows use
the owned mode and are released with the Database.

#### The Why:
The original pointer-binding contract remains useful for live, caller-owned
records, while service startup and accepted writes need a row lifetime owned by
the store. Making the distinction explicit preserves the zero-copy borrowed
path without confusing a copied row's lifetime or its release owner.

#### The Rule:
1. Reject a copy-in whose byte length is not exactly the registered schema
   stride; rejection leaves count and ownership mode unchanged.
2. Reject mixing modes on one entity. Owned insertion copies bytes and never
   retains the caller's address.
3. Free each owned row exactly once with its Database; never free borrowed
   rows. Schemas remain borrowed and outlive the Database.
4. Snapshot compatibility loading validates the full input and stages all
   owned rows before publishing any entity. This all-or-nothing guarantee is
   limited to that legacy loader; it does not turn the M2 flat dump into the
   specified persistence design, which is pages plus WAL.

---

### Reactive Program Law

#### Definition:
darkbase has a reactive layer — the SQL analog of triggers, views, and stored
procedures — as first-class named programs bound to store events
(`beforeInsert`, `afterInsert`, `beforeUpdate`, `afterDelete`, `onOpen`,
`onCommit`, `onRollback`).

#### The Why:
Persistence hooks (validation, indexing, derived views, audit, cache
invalidation) belong to the store, not scattered across every caller. Making
them named programs keeps behavior with the data and exportable with it.

#### The Rule:
1. **Programs fire on the cold admission path only.** Never per element on a hot read (Cold-Strict, Hot-Minimal Validation Law; Cold-Only Reflection Law).
2. **Programs are data plus behavior.** They export as a descriptor; their code rebinds by name on load, never by a stored address.
3. **Views are derived Structs.** A view's rows are computed by a program against the authoritative store, not a second synchronized copy.

---

### Store Versus Executable Separation Law

#### Definition:
The R3 repository is the `Database` interface and native vex store (future name
`darkbase-db`). The R5 executable is `darkbase`, an interactive host that opens a
`.vexdb` and works with it.

#### The Why:
The store must remain a resident library that any host borrows; the workbench is
an application with its own lifecycle. Conflating them couples the store to a
UI and blocks other consumers.

#### The Rule:
1. **The store never includes R5.** Hosts borrow an opaque `Database*` plus callbacks (Vertical Integration Law).
2. **The executable is a separate application** over the store, owning no store internals.
3. **Naming is deliberate:** store = `darkbase-db` (future), executable = `darkbase`.

---

### Byte-Native Persistence Law

#### Definition:
Values in darkbase are raw byte spans (`uint8_t[]`). A value is written in
memory order and read back the same way, with **no byte-order machinery**: no
endianness detection, no byte swapping, no text encoding. `toString` is a cold
converter for humans only, never a storage format. The canonical variable is
`{ uint8_t name[24], void* pointer }` — name bytes plus a pointer to the value
bytes (the engine's `VariableSlot` already carries this shape).

#### The Why:
Every supported host is little-endian (the Apple Silicon arm64 floor; Windows
x86_64), so there is no cross-endian peer to serve. Byte-in-order is more native
than a swap layer and needs no complication; converting numbers to strings for
storage would add encoding that the schema already provides. A `void*` value
pointer is the same "everything is a pointer" model the relational engine uses.

#### The Rule:
1. **Bytes, not text, for storage.** Durable values are `uint8_t[]` spans; the schema (`Field.typeId` + offset/size) interprets them. No string encoding in a file.
2. **No endianness code.** Write and read bytes in memory order (native, little-endian floor). No swap helpers, no byte-order fields. A `// INTENTIONAL(vex)` note states this at the format definition.
3. **`toString` is a cold converter.** String projections exist for debugging only (the toString Law) and are never the persisted form.
4. **Pointer-free rows.** Persisted row bytes carry no raw pointers; references and functions are rebound by name/index on load.
5. **Canonical variable.** `{ uint8_t name[24], void* pointer }`; names are 24-byte byte spans, the value is a pointer to its bytes.

---

### Zero-Alloc Relational Table Scan Law

#### Definition:
Table scans, range queries, and projection operations in `darkbase` operate directly across memory-mapped relational row slices using strided record offsets with zero heap allocation during query execution.

#### The Why:
Database query engines that allocate row objects during iteration thrash caches and bottleneck on the memory allocator. Strided memory-mapped scanning enables near-memory-bus throughput.

#### The Rule:
1. **Strided Projections:** Query iterators yield pointers to existing record spans.
2. **Zero Row Allocs:** Never allocate temporary row wrappers in scan loops.

---

### B-Tree Page Consistency Law

#### Definition:
B-tree node splits, leaf balance operations, and index updates execute strictly within fixed-size page blocks allocated from dedicated database memory pools.

#### The Why:
Predictable index geometry guarantees bounded traversal depth and prevents fragmentation across database storage files.

#### The Rule:
1. **Page Bounded:** B-tree pages are fixed-size power-of-two blocks.
2. **Atomic Splits:** Page balance updates commit transactionally before updating parent pointers.

---

## 3. Repo-Local Extensions (managed, per the Conflict Triage Law)

### Durable Page Store Boundary Law

`store/store.{h,c}` owns a page-level durable owner over R2's real Rust growable
mapping, Page and Wal. One physical file holds an immutable bootstrap page, a
caller-sized embedded redo region and data pages. No sidecar or C mapping clone
is introduced. Full detached sealed page images enter redo; its commit marker
is synced before mapped data changes. Data sync precedes success. Reopen admits
only after replay and free-chain validation; no checkpoint baseline other than
zero is supported. Uncertain IO blocks further admission until close/reopen,
and a failed call may be recovered as committed.

This is a separate primitive, not a durable `Database`/Headless integration,
schema/catalog or ordered row index. Release/reuse operates on logical page ids;
there is no stale row-handle generation promise. Embedded history is retained:
safe journal recycling/checkpoint compaction remains unimplemented, and writes
refuse/count when its explicit capacity or the data reservation is exhausted.
`Store_sync` never truncates or retires this embedded history.

`Database_saveStore`/`Database_loadStore` provide the initial Database bridge:
a dedicated reflected catalog encoding and raw owned-row spans are split into
page chains and published through one catalog root-page replacement; they do not
route through the legacy M2 snapshot parser. Loading requires the same schemas
registered first and stages all owned rows before publication. This is
whole-image persistence, not incremental row addressing or a multi-page
transaction. Old catalog/data pages and redo records are retained; capacity refusal
before root publication preserves the previously selected image, while uncertain
root commit requires close/reopen. This bridge does not provide checkpointing,
schema migration, structured indexes, or make Store operations thread-safe.

`Database_queryEqual` is the first structured query operation. It resolves one
reflected Field cold, validates its nonzero span against the entity row stride,
and compares exact bytes across the in-memory rows. It returns a cursor owning
only its temporary row-pointer list; matching row values remain borrowed and the
cursor must be freed before Database. It is not numeric comparison, projection,
ordering, persistent catalog lookup, or an index. Database and
query results remain caller-serialized/owner-affine.

`Database_queryInt32Range` interprets one exact-width field span as native
`int32_t` and returns rows within inclusive signed bounds. It performs no type
coercion; callers must use it only where the field's row bytes carry that
representation. Reversed bounds and invalid spans reject loudly without changing
Database state. It uses the entity's declared int32 index when one covers the
field and scans the row list otherwise, returning the same matches either way.

`Database_indexBuild` declares and builds the in-memory int32 index for one
field, and admits only a Field carrying `REFLECT_FIELD_INDEXED` whose span is
exactly `int32_t` — the schema's flag stays the single declaration point. The
records are a key-sorted array of borrowed row pointers. Rows are append-only,
so an insert only marks the records stale and the next query rebuilds them,
which keeps admission allocation-free and free of partial state. While an index
exists, the indexed field of a bound row must not be mutated in place; the
Database does not detect that.

`Database_saveStore` persists each declared index as `{int32_t key; uint32_t
rowIndex}` records placed after the entity's row span, inside the same catalog
payload and therefore under the same single root-page publication: rows and index
cannot tear apart. `Database_loadStore` re-validates every record before
publishing anything — key ordering, `rowIndex` range, and each key against the
bytes of the row it names — and refuses the whole image when the registered
schema no longer declares that exact indexed span. Addresses are never persisted;
the records are re-pointed at the loaded rows. An entity whose image carries no
index keeps its local declaration and rebuilds on the next query. This is still a
whole-image rewrite, not incremental index pages: no page-level index, no ordered
scan of other field types, and no multi-field index.

`Database_storeCatalogEntry` returns one copied persisted entity descriptor by
catalog index after validating the root, page chain, checksum and record spans.
The value contains name, row stride and row count and does not borrow Store
memory. It supports discovery only; loading data still requires the caller to
register matching reflected schemas.

The caller owns one serialization/exclusion domain, stable trusted paths and
all external alias/truncation exclusion. No OS lock or hostile-writer sandbox
is inferred. Geometry, owned pointers, page count, free head, sequence and
failure state mutate only through construction, admission and durable operations;
validated getters, not lifetime-forging setters, expose them. This is a scoped
managed accessor exception under the Conflict Triage Law and Single Class Per
File Law (Java Law). A non-reentrant observation callback may observe commit
boundaries but production correctness never depends on it. Initial creation is
not a crash-atomic publication and may retain a rejected artifact for recovery.

Registered independent proof is `tests/darkbase-engine/store/run.py` (workspace
root command), including mirrored Store/Page/Wal owners with fresh native C
dependencies, real Rust archive, optimized assertions and C ASan/UBSan. Actual
power loss, Rust sanitizer instrumentation, allocator failure, full OS-error and
other-platform/runtime-floor proof remain explicit gaps. The global graph's
post-migration ownership-ratchet failure is separate and not silently bypassed.

;;INTENTION("R3 Database Driver: native vex entity store over the shared reflection vocabulary (Struct=entity, Field=entity field with physical layout, Method=function) with reactive store-event programs; zero-alloc table scans; strict page consistency; structured native query, not a SQL engine.")

---

## 4. Readiness Cross-Reference (Living Documentation Law)

- Feature readiness matrix: [darkbase](https://gist.github.com/vex-graph/6943f92acb931b25dad1073c46da6ce7#file-darkbase-md).
- Open blockers and deferred decisions: [ecosystem blockers Gist](https://gist.github.com/vex-graph/e921fa188eebbd0c68c4e59646109887).
