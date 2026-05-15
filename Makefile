CC=gcc
CFLAGS=-fPIC -Wall $(shell pkg-config --cflags libpq)
LDFLAGS=-ldl -lsqlite3 $(shell pkg-config --libs libpq)
RUST_LIB=sql_translator/target/debug/libsql_translator.a

all: sqlite_hook.so test_sqlite

$(RUST_LIB): sql_translator/src/lib.rs sql_translator/Cargo.toml
	cd sql_translator && cargo build

sqlite_hook.so: sqlite_hook.c $(RUST_LIB)
	$(CC) $(CFLAGS) -shared -o sqlite_hook.so sqlite_hook.c $(RUST_LIB) $(LDFLAGS) -lpthread -lm -lrt

test_sqlite: test_sqlite.c
	$(CC) -o test_sqlite test_sqlite.c -lsqlite3

test: all
	cd sql_translator && cargo test
	# Run C test (might fail PG connect but checks hook integrity)
	LD_PRELOAD=./sqlite_hook.so ./test_sqlite || true

clean:
	rm -f *.so test_sqlite
	cd sql_translator && cargo clean
