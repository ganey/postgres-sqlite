# SQLite to PostgreSQL Compatibility Layer

This project provides an `LD_PRELOAD` shared library that intercepts SQLite API calls and transparently redirects them to a PostgreSQL backend. It's designed to allow applications hard-coded for SQLite (like Plex Media Server) to utilize the scalability, concurrency, and features of PostgreSQL without modification.

## Architecture

1.  **C Hook Library (`sqlite_hook.so`)**: Intercepts C-level SQLite calls (`sqlite3_open`, `sqlite3_prepare_v2`, `sqlite3_step`, etc.) using `dlfcn.h`. It manages connection state and statement lifecycle.
2.  **Rust Translation Engine (`sql_translator`)**: A high-performance Rust library linked into the C hook. It uses `sqlparser` and regex-based preprocessing to translate SQLite-specific SQL dialects and functions into PostgreSQL-compatible syntax.
3.  **PostgreSQL Bridge**: Uses `libpq` to communicate with the target PostgreSQL database, handling data type mapping and error code translation.

## Key Features

- **Transparent Interception**: Works with existing binaries using `LD_PRELOAD`.
- **Dialect Translation**:
    - `INSERT OR REPLACE` -> `INSERT ... ON CONFLICT DO UPDATE` (Schema-Aware).
    - `AUTOINCREMENT` / `INTEGER PRIMARY KEY` -> `SERIAL`.
    - SQLite functions (`strftime`, `ifnull`, `instr`, `last_insert_rowid`, `changes`).
- **Rich Data Types**: Transparent mapping for `TEXT`, `INTEGER`, `REAL`, and `BLOB` (`BYTEA`).
- **Transaction Support**: Full support for `BEGIN`, `COMMIT`, and `ROLLBACK`.
- **Metadata Discovery**: Maps `PRAGMA table_info` to PostgreSQL catalog queries.
- **Case Insensitivity**: Maps `COLLATE NOCASE` to PostgreSQL ICU collations.
- **Error Mapping**: Translates PostgreSQL constraint violations to SQLite error codes.

## Prerequisites

- **OS**: Linux (tested on Ubuntu/WSL2).
- **Build Tools**: `gcc`, `make`, `pkg-config`.
- **Dependencies**: `libsqlite3-dev`, `libpq-dev`.
- **Rust**: `rustc` and `cargo` (installed via [rustup.rs](https://rustup.rs/)).

## Setup & Build

1.  **Install System Dependencies**:
    ```bash
    sudo apt update
    sudo apt install -y build-essential libsqlite3-dev libpq-dev pkg-config
    ```

2.  **Build the Project**:
    ```bash
    make
    ```
    This will compile the Rust engine and the C shared library.

## Usage

Set the `PG_CONNINFO` environment variable to your PostgreSQL connection string and use `LD_PRELOAD` to launch your application.

```bash
# Using URI format (recommended)
export PG_CONNINFO="postgresql://user:password@localhost:5432/dbname"
# OR use standard Postgres environment variables
export PGHOST=localhost
export PGUSER=postgres

LD_PRELOAD=./sqlite_hook.so ./your_sqlite_application
```

## Running Tests

The project includes both Rust unit tests and a comprehensive C integration test.

```bash
npm test
# OR
make test
```

## Implementation Progress

| Feature | Status |
| :--- | :--- |
| Basic Interception (`LD_PRELOAD`) | ✅ Done |
| Postgres Connectivity (`libpq`) | ✅ Done |
| Statement Lifecycle (`prepare`, `step`) | ✅ Done |
| Transaction Support (`BEGIN`/`COMMIT`) | ✅ Done |
| SQL Dialect Translation (UPSERT, etc.) | ✅ Done |
| Data Type Mapping (Blobs, Numerics) | ✅ Done |
| Parameter Binding (`bind_*`) | ✅ Done |
| Schema Discovery (`PRAGMA table_info`) | ✅ Done |
| Global Functions (`strftime`, `rowid`) | ✅ Done |
| Case Insensitivity (`NOCASE`) | ✅ Done |
| Error Code Mapping | ✅ Done |

## Future Roadmap

- [ ] `PRAGMA foreign_key_list` and `PRAGMA index_list`.
- [ ] Support for Virtual Tables and FTS5 mapping.
- [ ] Connection pooling for high-concurrency applications.
- [ ] Support for custom SQLite extensions.
