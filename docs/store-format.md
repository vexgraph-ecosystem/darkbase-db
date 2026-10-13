# darkbase store format — v1 specification

**Status: specified, not implemented.** This supersedes the M2 dump format
(`VEXDB01`: 64-byte header, flat rows by `Struct.size`, trailing CRC32), which cannot
represent a message longer than one fixed slot and rewrites the whole store on every
save. Nothing has shipped against it, so it is orphaned rather than migrated.

## 1. One file, one database

- **One file per database.** The store is the system of record, not a cache.
- The file is opened inside a **fixed virtual reservation** chosen at create and mapped
  once, so growth never re-maps and pointers survive it (Growable File Mapping Law).
- Segregation — by country, tenant, or whatever the developer wants — means **separate
  files**, each with its own writer. It is a deployment choice, not a format feature.
- **One writer at a time.** Readers are excluded before destructive operations.

## 2. Pages

- **Fixed size, power of two**, chosen at create and recorded in the header (4 KiB
  default).
- Every page carries a **checksum**. Corruption names a *page*, not the whole file.
- Pages are **appended at the end inside the reservation**. Nothing existing moves.
- A **free list** of released pages lets the file plateau instead of ratcheting to a
  permanent high-water mark.

## 3. Header

Magic `VEXDB02`, format version, page size, reservation size, committed length, page
count, root page, free-list head.

## 4. Entities and fields

- An **entity IS a reflection `Struct`**; the **row stride IS `Struct.size`** (Entity
  Model Law). No parallel schema language.
- Every `Field` declares its own facts, independently, in any order and at any time —
  nothing accumulates, nothing depends on call sequence:

  | on the field | meaning |
  | :--- | :--- |
  | name | 24-byte folded atom — the canonical identity (cold lookup) |
  | `offset` | **location**: byte offset within the row |
  | `size` | byte width of the value |
  | `typeId` | the type of the **value** (not of the field) |
  | `flags` | includes `REFLECT_FIELD_KEY` — "part of the identity" |
  | `fieldId` | optional, **declared, stable** alias (compact; hot path) |

- **Identity is a flag, not a call sequence.** There is no `addKey`.
- **Key precedence is field order (offset).** The C struct's own layout decides which
  keyed field leads; reordering the struct changes precedence loudly, never silently.
- **`fieldId` is a contract.** It is declared, unique within the entity, and **stable** —
  never derived from position, or reordering the struct would silently rewrite the ids
  that indexes and saved queries refer to.
- **Fields are reachable two ways**: by **name** (canonical; resolve once on a cold path
  and hold the `Field*` — Cold-Only Reflection Law) and by **`fieldId`** (compact, for
  index entries and hot paths). Name alone is enough to be correct; the id is optional.

## 5. Rows

- Fixed stride. A row is addressed logically as **`(pageId, slot)`**, always
  **re-resolved** through the tree — never a cached byte offset.
- Append-only in v1; page reuse comes from the free list.
- A returned row pointer is valid only within one open session, for a pinned/unmoved
  page.

## 6. Variable-length text

- **Chunk size: 4096 bytes** (bytes, not characters — the Byte-Native Persistence Law
  has no encoding, and one UTF-8 character is 1–4 bytes).
- The **first chunk lives inline in the owning row** — the common case costs nothing
  extra.
- Overflow chunks are **separate rows keyed `(messageId, chunkIndex)`** — *not* a linked
  list, so reading a long message is one contiguous read rather than pointer chasing.

## 7. The v1 entities (the chat shape)

```
message        key (channelId, messageId)   author, at, chunkCount, totalBytes, head chunk inline
message_chunk  key (messageId, chunkIndex)  one <=4096-byte slice
```

- Message ids are **made by the application**, not the store: time-sortable (timestamp
  high bits + counter) so messages order themselves.
- History read = *"start at this channel's newest and walk back N"* — one bounded read,
  no sort, no scan.

## 8. Durability

- **Write-ahead log**: append the intent, fsync, apply, checkpoint. Replay on open.
- **Per-page checksums** in addition to the log.
- `flush` requests writeback; `sync` requests durability. Neither is implicit.

## 9. The addressing invariant

A row's address is a **logical name re-resolved through the tree**, read from a
checksummed page, in a file opened **by descriptor (not path)**, via 64-bit
bounds-checked positional access, mutated only through the log. A cached offset is a
cache of that resolution — nothing more.

## 10. Explicitly not in v1

- No SQL. The native query contract is structured.
- No multi-statement transactions beyond single-writer commit.
- No in-place shrink or compaction (`VACUUM` later).
- No wire protocol yet — see the setup/endpoint distinction, which promotes a service
  surface from optional to required.
- No migration from the M2 dump format.
