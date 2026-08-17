# ToyDB - Slotted-Page Record Layer & B+ Tree Indexing

[![C](https://img.shields.io/badge/Language-C-blue.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![DBMS](https://img.shields.io/badge/Domain-Database%20Internals-orange.svg)]()
[![CS631](https://img.shields.io/badge/Course-CS631%20DBMS-green.svg)]()

A lightweight, multi-layered relational database engine implementing a **Slotted-Page Record Storage Architecture** and **$B^+$ Tree Access Methods** on top of a **Paged Physical File Abstraction**.

---

## 📑 Table of Contents
- [Architecture Overview](#-architecture-overview)
- [Storage Engine & Slotted-Page Design](#-storage-engine--slotted-page-design)
  - [Page Layout](#page-layout)
  - [Record Addressing (RecId)](#record-addressing-recid)
  - [Tuple Serialization & Codec](#tuple-serialization--codec)
- [Layer Breakdown](#-layer-breakdown)
  - [1. Physical File Layer (`pflayer`)](#1-physical-file-layer-pflayer)
  - [2. Access Method Index Layer (`amlayer`)](#2-access-method-index-layer-amlayer)
  - [3. Database & Record Layer (`dblayer`)](#3-database--record-layer-dblayer)
- [Bug Analysis & Fixes](#-bug-analysis--fixes)
- [Build & Execution Guide](#-build--execution-guide)
  - [Prerequisites](#prerequisites)
  - [Compilation](#compilation)
  - [Data Ingestion (`loaddb`)](#data-ingestion-loaddb)
  - [Data Retrieval (`dumpdb`)](#data-retrieval-dumpdb)
- [Verification & Automated Testing](#-verification--automated-testing)

---

## 🏛 Architecture Overview

ToyDB is organized into a modular three-tier storage engine architecture:

```
+-------------------------------------------------------------------+
|                        Application Layer                          |
|           (loaddb: CSV Loader / dumpdb: Table Scanner)            |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                     Database & Record Layer                       |
|   - tbl.c / tbl.h: Slotted-Page Record Manager (Table_Insert,     |
|                    Table_Get, Table_Scan, Table_Open, Close)      |
|   - codec.c: Type Serialization (VARCHAR, INT, LONG)              |
|   - util.c: Schema & CSV Parser                                   |
+-------------------------------------------------------------------+
              |                                     |
              v                                     v
+-----------------------------+   +---------------------------------+
|  Access Method Layer (AM)   |   |    Physical File Layer (PF)     |
|  - B+ Tree Secondary Index  |   |  - Paged File Manager (4KB)     |
|  - Multi-level Node Split   |   |  - Buffer Pool Manager          |
|  - Range Scans & Equality   |   |  - Page Pinning & LRU Caching   |
+-----------------------------+   +---------------------------------+
              |                                     |
              +------------------+------------------+
                                 |
                                 v
                     +-----------------------+
                     |  Linux File System    |
                     | (data.db / data.db.0) |
                     +-----------------------+
```

---

## 📦 Storage Engine & Slotted-Page Design

### Page Layout
Each physical page (`PF_PAGE_SIZE = 4096 bytes`) is structured as a **Slotted Page**, accommodating variable-length records with zero internal fragmentation:

```
+-------------------------------------------------------------------+
| Offset 0..1 | Free Space Pointer (2 bytes, short)                 |
| Offset 2..3 | Slot Count `N`     (2 bytes, short)                 |
| Offset 4..5 | Slot 0 Offset      (2 bytes, short)                 |
| Offset 6..7 | Slot 1 Offset      (2 bytes, short)                 |
| ...         | ...                                                 |
| Offset 4+2N | Slot N-1 Offset    (2 bytes, short)                 |
|-------------+-----------------------------------------------------|
|             |                FREE SPACE                           |
|             |       (grows downward as slots are added)           |
|             v                                                     |
|                                                                   |
|             ^                                                     |
|             |       (grows upward as records are inserted)        |
|-------------+-----------------------------------------------------|
| Record N-1  | [Record Data (Variable Length)]                     |
| ...         | ...                                                 |
| Record 1    | [Record Data (Variable Length)]                     |
| Record 0    | [Record Data (Variable Length)]                     |
| (Page 4095) | (End of Page)                                       |
+-------------------------------------------------------------------+
```

#### Key Invariants:
1. **Header Growth**: Slots grow **top-down** from offset `4` (`Header Size = 4 + 2 * numSlots`).
2. **Data Growth**: Records are packed **bottom-up** starting from byte `4096`.
3. **Contiguous Free Space**: Contiguous available bytes = `FreeSpaceOffset - (4 + 2 * (numSlots + 1))`.
4. **Record Boundaries**:
   - For Slot $0$, the record boundaries are `[slotOffset[0], 4096)`. Length = `4096 - slotOffset[0]`.
   - For Slot $k > 0$, the record boundaries are `[slotOffset[k], slotOffset[k-1])`. Length = `slotOffset[k-1] - slotOffset[k]`.

### Record Addressing (`RecId`)
Each record is globally addressed by a 32-bit identifier `RecId`:
$$\text{RecId} = (\text{pageNum} \ll 16) \mid \text{slotNumber}$$
- **High 16 bits**: Page number within the physical file.
- **Low 16 bits**: Slot index in the page header.

### Tuple Serialization & Codec
- **`VARCHAR`**: Encoded with a 2-byte length prefix followed by string bytes without a null terminator (`[len: short][char bytes]`).
- **`INT`**: Encoded as a 4-byte standard integer (`memcpy`).
- **`LONG`**: Encoded as an 8-byte 64-bit integer (`long long`).

---

## 🧩 Layer Breakdown

### 1. Physical File Layer (`pflayer`)
- Manages block-level I/O on Unix files in fixed 4096-byte blocks.
- Implements an in-memory page buffer pool with page fixing (`PF_GetFirstPage`, `PF_GetNextPage`, `PF_GetThisPage`, `PF_AllocPage`) and unfixing (`PF_UnfixPage` with dirty flag).

### 2. Access Method Index Layer (`amlayer`)
- Provides balanced $B^+$ Tree secondary index storage.
- File naming convention: Base relation file `fileName` has index file `fileName.indexNo` (e.g., `data.db.0`).
- Supports index insertion (`AM_InsertEntry`), deletion (`AM_DeleteEntry`), and predicate scans (`AM_OpenIndexScan`, `AM_FindNextEntry`, `AM_CloseIndexScan`).

### 3. Database & Record Layer (`dblayer`)
- **`Table_Open(dbname, schema, overwrite, &ptable)`**: Opens or initializes a table and associates it with the physical file descriptor.
- **`Table_Close(tbl)`**: Closes open file descriptors and frees table structures.
- **`Table_Insert(tbl, record, len, &rid)`**: Finds existing page with sufficient free space (or allocates a new page), places the record bottom-up, updates the slot table, and returns the assigned `RecId`.
- **`Table_Get(tbl, rid, record, maxlen)`**: Fetches a record by its `RecId`, computes length via slot boundaries, and copies into destination buffer.
- **`Table_Scan(tbl, callbackObj, callbackfn)`**: Sequentially scans all pages and records, invoking the callback for each valid row.

---

## 🛠 Bug Analysis & Fixes

During the audit of the initial implementation, several critical bugs were identified and resolved:

| Component | Bug Description | Root Cause | Resolution |
| :--- | :--- | :--- | :--- |
| **`loaddb.c`** | **Segmentation Fault on Insert** | `AM_CreateIndex("data.db.0", 0, ...)` created `"data.db.0.0"`, while `indexFD` opened `"data.db.0"`, pointing to an uninitialized PF file. | Called `AM_CreateIndex("data.db", 0, ...)` to generate `"data.db.0"` and opened the resulting descriptor correctly. |
| **`tbl.c`** | **Negative / Zero Record Length** | Record length in `Table_Get` & `Table_Scan` was computed as `nextSlot - currSlot` (negative for bottom-up storage) or `freeSpace - currSlot` (0 for last slot). | Fixed length computation to `prevBoundary - currSlot`, where `prevBoundary` is `slot == 0 ? 4096 : slotOffset[slot - 1]`. |
| **`tbl.c`** | **Buffer Pool Pinning Leak** | `PF_GetFirstPage` and `PF_GetNextPage` fix pages in the buffer pool. In `Table_Insert` and `Table_Scan`, pages were not unfixed when traversing, exhausting the buffer pool. | Added explicit `PF_UnfixPage(tbl->fileDesc, prevPage, FALSE)` on every page transition. |
| **`tbl.c`** | **Uninitialized Error Checking** | `Table_Open` checked uninitialized variable `err` with `checkerr(err)`, risking random crashes on garbage stack values. | Removed invalid check and properly verified return codes of `PF_CreateFile` and `PF_OpenFile`. |
| **`dumpdb.c`** | **Hardcoded Operator Magic Numbers** | Index scan passed raw integers `4` and `3` instead of defined operator constants. | Replaced with `LESS_THAN_EQUAL` and `GREATER_THAN` from `am.h`. |

---

## 🚀 Build & Execution Guide

### Prerequisites
- GCC / Clang C compiler
- Make build system
- Standard POSIX environment (Linux)

### Compilation
Build all three layers in order:

```bash
# 1. Build Physical File Layer
make -C pflayer

# 2. Build Access Method Index Layer
make -C amlayer

# 3. Build Database Layer (loaddb & dumpdb binaries)
make -C dblayer
```

To clean all build artifacts:
```bash
make -C pflayer clean && make -C amlayer clean && make -C dblayer clean
```

---

### Data Ingestion (`loaddb`)
Ingests data from `dblayer/data.csv` (194 country records), creates the table `data.db`, and builds the $B^+$ Tree index on the `Population` column (`data.db.0`):

```bash
cd dblayer
./loaddb
```

Output format during loading:
```
<RecId> <Country>
0 Afghanistan
1 Albania
...
65536 Singapore
...
```

---

### Data Retrieval (`dumpdb`)

#### 1. Sequential Table Scan (`dumpdb s`)
Performs a sequential scan of the table via `Table_Scan` and outputs records in page storage order:

```bash
./dumpdb s
```

#### 2. $B^+$ Tree Index Scan (`dumpdb i`)
Performs an index scan using `AM_OpenIndexScan` on the `Population` index for $\le 100,000$ and $> 100,000$, reconstructing the database ordered by population:

```bash
./dumpdb i
```

---

## 🧪 Verification & Automated Testing

To verify data correctness and ensure zero data corruption:

```bash
cd dblayer

# Step 1: Ingest database
./loaddb

# Step 2: Verify Sequential Scan against original CSV (194 rows)
diff -u <(tail -n +2 data.csv | sort) <(./dumpdb s | sort)
echo "Sequential Scan Diff Exit Code: $?" # Must be 0 (No difference)

# Step 3: Verify Index Scan against original CSV (194 rows)
diff -u <(tail -n +2 data.csv | sort) <(./dumpdb i | sort)
echo "Index Scan Diff Exit Code: $?" # Must be 0 (No difference)
```
