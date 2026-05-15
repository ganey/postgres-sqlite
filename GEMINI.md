# SQLite to PostgreSQL LD_PRELOAD Compatibility Layer

## Objective
Create a universal compatibility layer that allows applications heavily dependent on SQLite (like Plex and Radarr/Sonarr) to use PostgreSQL as their backend. This is achieved by creating an LD_PRELOAD shared C library that intercepts C-level SQLite API calls and translates them into PostgreSQL calls using libpq.

## Key Files & Context
*   **Target Directory:** /mnt/c/Users/micha/github/postgres-sqlite
*   **Core Technology:** C, Rust, dlfcn.h, libsqlite3, libpq, sqlparser.

## Implementation Roadmap (Project Complete)

### Phase 1: Proof of Concept (The Stub) - DONE
1.  Initialize the project and build infrastructure.
2.  Intercept `sqlite3_open` and `sqlite3_exec`.
3.  Implement basic query forwarding using `dlsym(RTLD_NEXT)`.

### Phase 2: Postgres Connection (libpq) - DONE
1.  Establish real PostgreSQL connections via `libpq`.
2.  Support standard connection URIs and environment variables.

### Phase 3: The Statement Lifecycle - DONE
1.  Implement `sqlite3_prepare_v2`, `sqlite3_step`, and `sqlite3_column_*`.
2.  Manage statement-to-PostgreSQL-result mappings.

### Phase 4: Translation Engine - DONE
1.  Implement Rust-based AST translation using `sqlparser`.
2.  Handle Dialect differences: `AUTOINCREMENT`, `strftime`, `ifnull`, etc.
3.  Support Schema-Aware UPSERT (`INSERT OR REPLACE`).
4.  Map `COLLATE NOCASE` to PostgreSQL ICU collations.

### Phase 7: Schema & Metadata - DONE
1.  Map `PRAGMA table_info` to `information_schema`.
2.  Implement `last_insert_rowid()` and `changes()` tracking.

### Phase 8: Extended Types - DONE
1.  Full BLOB/BYTEA support with hex encoding/decoding.
2.  Native numeric column retrieval.

### Phase 9: Error & Exception Handling - DONE
1.  Map PostgreSQL SQLSTATE to SQLite error codes (`SQLITE_CONSTRAINT`, etc.).
2.  Hook `sqlite3_errmsg` and `sqlite3_errcode`.

### Phase 5: CI & Testing - DONE
1.  Setup GitHub Workflows with PostgreSQL matrix (v14, 15, 16).
2.  Add unit and integration tests.

## Summary of Achievement
The project successfully provides a high-fidelity compatibility layer. It allows standard SQLite applications to perform complex CRUD operations, discovery schema metadata, and utilize binary data and transactions on a PostgreSQL backend without any code changes to the host application.
