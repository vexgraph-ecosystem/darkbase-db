# darkbase — R3 native vex database store

Build with [b](https://github.com/vex-graph/b):
`./tools/b build darkbase` from the workspace root.

Store format (specified, not implemented): [`docs/store-format.md`](docs/store-format.md).

## Current State

**Role:** R3 Driver — the `Database` interface owner and its native vex store
(future name `darkbase-db`).

**What is implemented and proven (macOS arm64):**
- The class registry (`src/oop/type.h`, `PROJ_DARKBASE`) and the workspace
  build entry (`setup_darkbase`); header contract and the `b` graph verified.
- **In-memory store** — `Database` registers reflection `Struct` entities and
  supports borrowed rows plus exact-stride owned copy-in; `DatabaseResult` is a
  dest-last cursor. Owner
  tests `database_test` and `database_result_test` pass under
  `-Wall -Wextra -Werror`.
- **Legacy snapshot compatibility slice** — `Database_save`/`Database_load`
  still read and write the superseded `.vexdb` M2 dump (64-byte `VEXDB01`
  header, flat row bytes by `Struct.size`, trailing CRC32). Load validates the
  complete payload and stages every row copy before publishing any entity.
  This dump is not the specified persistence direction; pages + WAL are the
  forward design in `docs/store-format.md` and `wal/`.

**Stubbed, draft, or planned:** field-level predicates and an offset-based codec;
an mmap/paged store; Database mutation/replay integration with the now-implemented
standalone WAL; transactions; CSV/JSON/manifest and generated-header export; reactive
`DbTrigger` programs; external drivers; the R5 `darkbase` executable. Registry
ids are declared ahead of their classes.

**Platforms proven:** macOS arm64 (Apple Silicon) only. Windows is unproven.

**Evidence:** `../../tests/darkbase` owners and `../../gist/test-checklist.md`. This is a
lab pass, not a visual or cross-platform claim.

## What it is

`darkbase` is where ecosystem persistence lives: the `Database` interface plus a
native vex-backed implementation engineered for bounded, in-budget operation (no
vendor SDKs in the hot path). `api-haven` holds only connector/catalog shapes
(`DbProvider`, `DbSqliteFile` descriptors) — execution delegates here, per the
Vertical Integration Law.

It is **not** a SQL engine. The native contract is a structured relational API;
the schema is C reflection, not DDL. Text/SQL appears only as an optional
`cli/` convenience or a foreign driver's private translation, never in the core.

## The entity model — one shared vocabulary

darkbase does not define its own schema types; it consumes R2 reflection. A
`struct` is a `Struct`, a `field` is a `Field`, a `class` is a `Class` (a `Struct`
plus a constructor and `Method`s), and a `function` is a `Method`:

| C vocabulary | shared type | Meaning |
| :--- | :--- | :--- |
| `struct` | `Struct` (reflection) | a table: an ordered `Field` list = one entity; its `size` is the row stride |
| `class` | `Class` (reflection) | a `Struct` + constructor + `Method[]` (an entity with behavior) |
| `field` | `Field` (reflection) | a column: name + read/set, plus physical layout (offset, size, value typeId, flags) |
| `function` | `Method` (reflection) | a named callable binding |
| `trigger` | `DbTrigger` (darkbase) | a reactive program bound to a store event (embeds a `Method` + event) |

`Field` is the keystone, and it already exists in R2 reflection: vexspoke extends
it with the physical layout (offset/size/typeId/flags) so one record both
reads/writes a value and describes where its Bytes live. That is what makes
reflection, persistence and export speak one language; `Field_setTypeId` derives
the byte width from `Stride_get` when the size is unset. darkbase adds no
parallel descriptor.

Plain values are named typed bindings in the same scope, so structs, functions,
and values share one vocabulary.

## Reactive programs

Named programs bound to store events are the SQL analog of triggers, views, and
stored procedures. A program declares an event (`beforeInsert`, `afterInsert`,
`beforeUpdate`, `afterDelete`, `onOpen`, `onCommit`, `onRollback`) plus a
callback, and the store fires it on the cold admission path — never per element
on a hot read (Cold-Strict, Hot-Minimal Validation Law; Cold-Only Reflection
Law). Views are derived `Struct` rows computed by a program. Programs export as
a descriptor; their code rebinds by name on load, like functions.

## Future naming: store vs executable

- R3 (this repository) is the `Database` interface and native vex store. A future
  rename makes it `darkbase-db`.
- R5 (future) is `darkbase`: the interactive executable that opens a `.vexdb` and
  works with it — an application over the R3 library via opaque handles, never an
  include. `darkbase-interactable` is that host.

## Depends on (Vertical Integration Law allowlist)

Either R2 public contract: Vexspoke CPU computation/behavior (`oop/type.h`,
`reflection/*`) or Relational Engine memory/storage (`nio/mem.h`, `io/file.h`,
`io/vexhome.h`, stable rows, `VariableRegistry`, native C search). Never
R1/R4/R5 or `api-haven` headers. GPU shaders, dispatch and graphics composition
stay Graphvex R3; a GPU-backed store is a future borrow, not a dependency.
Database semantics and persistence remain Darkbase R3.

## Layout

- `src/oop/type.h` — the class registry (`PROJ_DARKBASE`, 1..N).
- `src/database` — the L2 `Database` interface, entity/row switchboard, dest-last
  cursor and legacy `.vexdb` snapshot compatibility (implemented: `database.{h,c}`,
  `database_result.{h,c}`).
- `src/schema/` — registration over reflection `Struct`/`Field`/`Class` (future).
- `src/store/`, `src/codec/`, `src/index/`, `src/tx/`, `src/export/` (future).
- `..` — quarantined external driver dylibs (future: sqlite, postgres, ...).
- Tests: the shared `../../tests/darkbase` partition (mirrored per unit, the
  Test Tree Mirror Law); no test file lives inside this repo's source
  directories (the Test Segregation Law).

## Laws that govern work here

- Constitution: the [canonical preferences.md Gist](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a); one real, Git-ignored workspace `../../gist/PREFERENCES.md`, not a Vexspoke file or symlink.
- Commits land in THIS repo root, one cohesive unit each; never push unless asked.
- One public class per `.h`/`.c` pair, `(*ptr).field` (never `->`), dest-last
  params, `-Wall -Wextra -Werror`.

## Scope and Limitations

**Scope:** darkbase owns the `Database` interface and the native vex store —
entity/row semantics over R2 reflection, structured (non-SQL) queries, and
export. Its legacy flat snapshot API is compatibility-only; pages + WAL are the
specified persistence direction. It borrows Relational Engine memory/IO and Vexspoke CPU/reflection
contracts; it owns no OS window, GPU, or network behavior.

**Deliberately not covered:**
- Not a SQL engine: no text parser, planner, or executor in the core; SQL is at
  most an optional `cli/` convenience or a foreign driver's private translation.
- No external database client libraries in the default build; foreign drivers
  (SQLite/Postgres/MariaDB/cloud) are future quarantined dylibs.
- No R1/R4/R5 or `api-haven` headers (the Vertical Integration Law).

**Known limits and gaps:**
- `Database_insert` borrows rows; they and every schema must outlive Database.
  `Database_insertOwned` copies exactly one `Struct.size` row. Each entity uses
  only one mode; owned copies and loaded rows are released by `Database_free`.
- The legacy `.vexdb` snapshot format is native-endian (little-endian hosts)
  with whole-row bytes by `Struct.size`; there is no per-field codec.
- Save publishes atomically (temp write + `fsync` + rename), so a failed save
  never destroys the previous snapshot. Load validates the complete payload and
  stages all rows before publishing any entity.
- Save/load stream through the engine `File`; the compatibility path has no
  mmap/paged store or page directory. The specified forward design is pages +
  WAL, not this flat snapshot. Transactional mutations and durable audit remain
  unimplemented.
- Persistence is owner-affine and single-writer; no concurrency or multi-process
  coordination is claimed.
- Proven only on macOS arm64; Windows is untested.
