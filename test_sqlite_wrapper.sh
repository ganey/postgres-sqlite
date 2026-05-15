#!/bin/bash
set -e

echo "--- Debug Info ---"
ls -l
ldd ./sqlite_hook.so
echo "PG_CONNINFO: $PG_CONNINFO"
echo "------------------"

# Run the test and capture output
# We use stdbuf to ensure output isn't buffered if it crashes
output=$(LD_PRELOAD=./sqlite_hook.so ./test_sqlite 2>&1)
echo "$output"

# Verify connection
if echo "$output" | grep -q "Connected to PostgreSQL successfully"; then
    echo "Verification Success: Connected to Postgres."
else
    echo "Verification Failure: Did not connect to Postgres."
    # Check for specific error message
    if echo "$output" | grep -q "Connection to database failed"; then
        echo "Postgres connection error detected."
    fi
    exit 1
fi
