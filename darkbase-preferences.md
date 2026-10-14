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

;;INTENTION("R3 Database Driver: native vex entity store over the shared reflection vocabulary (Struct=entity, Field=entity field with physical layout, Method=function) with reactive store-event programs; zero-alloc table scans; strict page consistency; structured native query, not a SQL engine.")

---

## 4. Readiness Cross-Reference (Living Documentation Law)

- Feature readiness matrix: [darkbase](https://gist.github.com/vex-graph/6943f92acb931b25dad1073c46da6ce7#file-darkbase-md).
- Open blockers and deferred decisions: [ecosystem blockers Gist](https://gist.github.com/vex-graph/e921fa188eebbd0c68c4e59646109887).
