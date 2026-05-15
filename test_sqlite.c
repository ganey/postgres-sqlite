#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

int main() {
    sqlite3 *db;
    char *err_msg = 0;
    
    int rc = sqlite3_open("test.db", &db);
    
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }
    
    const char *sql = "CREATE TABLE IF NOT EXISTS Friends(Id INTEGER PRIMARY KEY, Name TEXT);";
    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    }
    
    sqlite3_close(db);

    // Second test: Select and Iterate
    rc = sqlite3_open("test.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    sqlite3_stmt *res;
    sql = "SELECT Id, Name FROM Friends;";
    
    rc = sqlite3_prepare_v2(db, sql, -1, &res, 0);
    
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to fetch data: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    while (sqlite3_step(res) == SQLITE_ROW) {
        printf("%s: %s\n", sqlite3_column_text(res, 0), sqlite3_column_text(res, 1));
    }

    sqlite3_finalize(res);

    // Third test: Transactions
    printf("\n--- Starting Transaction Test ---\n");
    rc = sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (BEGIN): %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    rc = sqlite3_exec(db, "INSERT INTO Friends(Id, Name) VALUES(100, 'TransactionFriend');", 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (INSERT): %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    rc = sqlite3_exec(db, "COMMIT;", 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error (COMMIT): %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    // Fourth test: Parameterized Queries
    printf("\n--- Starting Parameterized Query Test ---\n");
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, "INSERT INTO Friends(Id, Name) VALUES(?, ?);", -1, &stmt, 0);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, 200);
        sqlite3_bind_text(stmt, 2, "ParameterizedFriend", -1, SQLITE_STATIC);
        
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "Execution failed: %s\n", sqlite3_errmsg(db));
        } else {
            printf("Parameterized insert successful\n");
        }
        sqlite3_finalize(stmt);
    } else {
        fprintf(stderr, "Prepare failed: %s\n", sqlite3_errmsg(db));
    }

    // Fifth test: PRAGMA table_info
    printf("\n--- Starting PRAGMA table_info Test ---\n");
    rc = sqlite3_prepare_v2(db, "PRAGMA table_info(Friends);", -1, &stmt, 0);
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            printf("Column: %s, Type: %s\n", sqlite3_column_text(stmt, 1), sqlite3_column_text(stmt, 2));
        }
        sqlite3_finalize(stmt);
    } else {
        fprintf(stderr, "PRAGMA prepare failed: %s\n", sqlite3_errmsg(db));
    }

    // Sixth test: BLOBs
    printf("\n--- Starting BLOB Test ---\n");
    unsigned char blob_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x21, 0x42};
    int blob_size = sizeof(blob_data);
    
    rc = sqlite3_prepare_v2(db, "INSERT INTO Friends(Id, Name) VALUES(300, ?);", -1, &stmt, 0);
    if (rc == SQLITE_OK) {
        sqlite3_bind_blob(stmt, 1, blob_data, blob_size, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            printf("BLOB insert successful\n");
        } else {
            fprintf(stderr, "BLOB insert failed: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
    }

    rc = sqlite3_prepare_v2(db, "SELECT Name FROM Friends WHERE Id = 300;", -1, &stmt, 0);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *ret_blob = sqlite3_column_blob(stmt, 0);
            int ret_size = sqlite3_column_bytes(stmt, 0);
            printf("Retrieved BLOB size: %d\n", ret_size);
            if (ret_size == blob_size && memcmp(ret_blob, blob_data, blob_size) == 0) {
                printf("BLOB data matches!\n");
            } else {
                printf("BLOB data mismatch or wrong size.\n");
            }
        }
        sqlite3_finalize(stmt);
    }

    // Seventh test: Error Code Mapping
    printf("\n--- Starting Error Code Mapping Test ---\n");
    sqlite3_exec(db, "INSERT INTO Friends(Id, Name) VALUES(1, 'Original');", 0, 0, 0);
    rc = sqlite3_exec(db, "INSERT INTO Friends(Id, Name) VALUES(1, 'Duplicate');", 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        printf("Detected expected error: %s (code: %d)\n", sqlite3_errmsg(db), sqlite3_errcode(db));
        if (rc == SQLITE_CONSTRAINT) {
            printf("Success: Mapped to SQLITE_CONSTRAINT correctly!\n");
        } else {
            printf("Failure: Mapped to %d instead of SQLITE_CONSTRAINT\n", rc);
        }
        if (err_msg) sqlite3_free(err_msg);
    }

    // Eighth test: Global Functions (last_insert_rowid, changes)
    printf("\n--- Starting Global Functions Test ---\n");
    sqlite3_exec(db, "INSERT INTO Friends(Id, Name) VALUES(400, 'GlobalFuncFriend');", 0, 0, 0);
    
    printf("last_insert_rowid (API): %lld\n", sqlite3_last_insert_rowid(db));
    printf("changes (API): %d\n", sqlite3_changes(db));

    rc = sqlite3_prepare_v2(db, "SELECT last_insert_rowid(), changes();", -1, &stmt, 0);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            printf("last_insert_rowid (SQL): %lld\n", sqlite3_column_int64(stmt, 0));
            printf("changes (SQL): %d\n", sqlite3_column_int(stmt, 1));
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    printf("Success\n");
    return 0;
}
