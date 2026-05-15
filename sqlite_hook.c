#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sqlite3.h>
#include <libpq-fe.h>

// Function pointers to hold the original SQLite functions
static int (*original_sqlite3_open)(const char*, sqlite3**) = NULL;
static int (*original_sqlite3_exec)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**) = NULL;
static int (*original_sqlite3_prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = NULL;
static int (*original_sqlite3_step)(sqlite3_stmt*) = NULL;
static const unsigned char* (*original_sqlite3_column_text)(sqlite3_stmt*, int) = NULL;
static int (*original_sqlite3_finalize)(sqlite3_stmt*) = NULL;
static int (*original_sqlite3_reset)(sqlite3_stmt*) = NULL;
static int (*original_sqlite3_bind_int)(sqlite3_stmt*, int, int) = NULL;
static int (*original_sqlite3_bind_text)(sqlite3_stmt*, int, const char*, int, void(*)(void*)) = NULL;
static int (*original_sqlite3_bind_null)(sqlite3_stmt*, int) = NULL;
static int (*original_sqlite3_bind_double)(sqlite3_stmt*, int, double) = NULL;
static int (*original_sqlite3_bind_blob)(sqlite3_stmt*, int, const void*, int, void(*)(void*)) = NULL;
static const void* (*original_sqlite3_column_blob)(sqlite3_stmt*, int) = NULL;
static int (*original_sqlite3_column_bytes)(sqlite3_stmt*, int) = NULL;
static int (*original_sqlite3_column_int)(sqlite3_stmt*, int) = NULL;
static sqlite3_int64 (*original_sqlite3_column_int64)(sqlite3_stmt*, int) = NULL;
static double (*original_sqlite3_column_double)(sqlite3_stmt*, int) = NULL;
static sqlite3_int64 (*original_sqlite3_last_insert_rowid)(sqlite3*) = NULL;
static int (*original_sqlite3_changes)(sqlite3*) = NULL;
static const char* (*original_sqlite3_errmsg)(sqlite3*) = NULL;
static int (*original_sqlite3_errcode)(sqlite3*) = NULL;

static PGconn *pg_conn = NULL;
static sqlite3_int64 last_rowid = 0;
static int affected_rows = 0;
static int last_sqlite_error = SQLITE_OK;
static char last_pg_error_msg[1024] = {0};

// Helper to initialize the original function pointers
static void initialize_hooks() {
    if (!original_sqlite3_open) {
        original_sqlite3_open = dlsym(RTLD_NEXT, "sqlite3_open");
        original_sqlite3_exec = dlsym(RTLD_NEXT, "sqlite3_exec");
        original_sqlite3_prepare_v2 = dlsym(RTLD_NEXT, "sqlite3_prepare_v2");
        original_sqlite3_step = dlsym(RTLD_NEXT, "sqlite3_step");
        original_sqlite3_column_text = dlsym(RTLD_NEXT, "sqlite3_column_text");
        original_sqlite3_finalize = dlsym(RTLD_NEXT, "sqlite3_finalize");
        original_sqlite3_reset = dlsym(RTLD_NEXT, "sqlite3_reset");
        original_sqlite3_bind_int = dlsym(RTLD_NEXT, "sqlite3_bind_int");
        original_sqlite3_bind_text = dlsym(RTLD_NEXT, "sqlite3_bind_text");
        original_sqlite3_bind_null = dlsym(RTLD_NEXT, "sqlite3_bind_null");
        original_sqlite3_bind_double = dlsym(RTLD_NEXT, "sqlite3_bind_double");
        original_sqlite3_bind_blob = dlsym(RTLD_NEXT, "sqlite3_bind_blob");
        original_sqlite3_column_blob = dlsym(RTLD_NEXT, "sqlite3_column_blob");
        original_sqlite3_column_bytes = dlsym(RTLD_NEXT, "sqlite3_column_bytes");
        original_sqlite3_column_int = dlsym(RTLD_NEXT, "sqlite3_column_int");
        original_sqlite3_column_int64 = dlsym(RTLD_NEXT, "sqlite3_column_int64");
        original_sqlite3_column_double = dlsym(RTLD_NEXT, "sqlite3_column_double");
        original_sqlite3_last_insert_rowid = dlsym(RTLD_NEXT, "sqlite3_last_insert_rowid");
        original_sqlite3_changes = dlsym(RTLD_NEXT, "sqlite3_changes");
        original_sqlite3_errmsg = dlsym(RTLD_NEXT, "sqlite3_errmsg");
        original_sqlite3_errcode = dlsym(RTLD_NEXT, "sqlite3_errcode");
    }
}

extern char* translate_sql(const char* sql);
extern void free_translated_sql(char* ptr);

#define MAX_PARAMS 128
#define BYTEAOID 17

typedef struct {
    sqlite3_stmt *stmt;
    PGresult *pg_res;
    int current_row;
    char *translated_sql;
    char *param_values[MAX_PARAMS];
    int num_params;
    unsigned char *blob_buffers[MAX_PARAMS]; // To store unhexed column blobs
    int blob_sizes[MAX_PARAMS];
} StmtMapping;

#define MAX_STMTS 1024
static StmtMapping stmt_mappings[MAX_STMTS];

static void add_mapping(sqlite3_stmt *stmt, char *translated_sql) {
    for (int i = 0; i < MAX_STMTS; i++) {
        if (stmt_mappings[i].stmt == NULL) {
            stmt_mappings[i].stmt = stmt;
            stmt_mappings[i].pg_res = NULL;
            stmt_mappings[i].current_row = -1;
            stmt_mappings[i].translated_sql = translated_sql ? strdup(translated_sql) : NULL;
            stmt_mappings[i].num_params = 0;
            memset(stmt_mappings[i].param_values, 0, sizeof(stmt_mappings[i].param_values));
            memset(stmt_mappings[i].blob_buffers, 0, sizeof(stmt_mappings[i].blob_buffers));
            memset(stmt_mappings[i].blob_sizes, 0, sizeof(stmt_mappings[i].blob_sizes));
            return;
        }
    }
}

static StmtMapping* get_mapping(sqlite3_stmt *stmt) {
    for (int i = 0; i < MAX_STMTS; i++) {
        if (stmt_mappings[i].stmt == stmt) {
            return &stmt_mappings[i];
        }
    }
    return NULL;
}

static void clear_blob_buffers(StmtMapping *m) {
    for (int j = 0; j < MAX_PARAMS; j++) {
        if (m->blob_buffers[j]) {
            free(m->blob_buffers[j]);
            m->blob_buffers[j] = NULL;
            m->blob_sizes[j] = 0;
        }
    }
}

static void remove_mapping(sqlite3_stmt *stmt) {
    for (int i = 0; i < MAX_STMTS; i++) {
        if (stmt_mappings[i].stmt == stmt) {
            if (stmt_mappings[i].pg_res) {
                PQclear(stmt_mappings[i].pg_res);
            }
            if (stmt_mappings[i].translated_sql) {
                free(stmt_mappings[i].translated_sql);
            }
            for (int j = 0; j < MAX_PARAMS; j++) {
                if (stmt_mappings[i].param_values[j]) {
                    free(stmt_mappings[i].param_values[j]);
                }
            }
            clear_blob_buffers(&stmt_mappings[i]);
            stmt_mappings[i].stmt = NULL;
            stmt_mappings[i].pg_res = NULL;
            return;
        }
    }
}

static int map_pg_error(const char *sqlstate) {
    if (!sqlstate) return SQLITE_ERROR;
    if (strcmp(sqlstate, "23505") == 0) return SQLITE_CONSTRAINT; // Unique
    if (strcmp(sqlstate, "23503") == 0) return SQLITE_CONSTRAINT; // FK
    if (strcmp(sqlstate, "23502") == 0) return SQLITE_CONSTRAINT; // Not null
    if (strcmp(sqlstate, "23514") == 0) return SQLITE_CONSTRAINT; // Check
    if (sqlstate[0] == '2' && sqlstate[1] == '3') return SQLITE_CONSTRAINT; // Any integrity constraint
    return SQLITE_ERROR;
}

static void set_last_error(PGresult *res) {
    if (!res) {
        last_sqlite_error = SQLITE_ERROR;
        snprintf(last_pg_error_msg, sizeof(last_pg_error_msg), "No result set from Postgres");
        return;
    }
    const char *state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    last_sqlite_error = map_pg_error(state);
    const char *msg = PQresultErrorMessage(res);
    if (msg) {
        strncpy(last_pg_error_msg, msg, sizeof(last_pg_error_msg)-1);
    }
}

static char* discover_upsert_clause(PGconn *conn, const char *table_name) {
    if (!conn) return NULL;
    char query[1024];
    snprintf(query, sizeof(query), 
        "SELECT kcu.column_name "
        "FROM information_schema.table_constraints AS tc "
        "JOIN information_schema.key_column_usage AS kcu ON tc.constraint_name = kcu.constraint_name "
        "WHERE tc.constraint_type = 'PRIMARY KEY' AND tc.table_name = '%s'", table_name);
    
    PGresult *res = PQexec(conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        return NULL;
    }
    char *pk_col = strdup(PQgetvalue(res, 0, 0));
    PQclear(res);

    snprintf(query, sizeof(query), 
        "SELECT column_name FROM information_schema.columns "
        "WHERE table_name = '%s' AND column_name != '%s'", table_name, pk_col);
    
    res = PQexec(conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        free(pk_col);
        PQclear(res);
        return NULL;
    }

    char clause[2048];
    snprintf(clause, sizeof(clause), " ON CONFLICT (%s) DO UPDATE SET ", pk_col);
    for (int i = 0; i < PQntuples(res); i++) {
        char *col = PQgetvalue(res, i, 0);
        strcat(clause, col);
        strcat(clause, " = EXCLUDED.");
        strcat(clause, col);
        if (i < PQntuples(res) - 1) strcat(clause, ", ");
    }
    free(pk_col);
    PQclear(res);
    return strdup(clause);
}

static char* process_schema_aware_upsert(PGconn *conn, char *sql) {
    char *marker = strstr(sql, "/* PG_UPSERT: ");
    if (!marker) return sql;
    char table_name[256];
    if (sscanf(marker, "/* PG_UPSERT: %255s */", table_name) == 1) {
        char *end = strstr(table_name, " ");
        if (end) *end = '\0';
        char *upsert_clause = discover_upsert_clause(conn, table_name);
        if (upsert_clause) {
            size_t new_len = strlen(sql) + strlen(upsert_clause) + 1;
            char *new_sql = malloc(new_len);
            size_t prefix_len = marker - sql;
            strncpy(new_sql, sql, prefix_len);
            new_sql[prefix_len] = '\0';
            strcat(new_sql, upsert_clause);
            free(upsert_clause);
            free(sql);
            return new_sql;
        }
    }
    return sql;
}

static char* replace_marker(char *sql, const char *marker, int value) {
    char *p = strstr(sql, marker);
    if (!p) return sql;

    char buf[32];
    snprintf(buf, sizeof(buf), " %d", value);
    
    char *zero = strstr(p, " 0");
    if (!zero) return sql;

    size_t val_len = strlen(buf);
    size_t new_len = strlen(sql) + val_len + 1;
    char *new_sql = malloc(new_len);
    size_t prefix_len = zero - sql;
    strncpy(new_sql, sql, prefix_len);
    new_sql[prefix_len] = '\0';
    strcat(new_sql, buf);
    strcat(new_sql, zero + 2);
    free(sql);
    return new_sql;
}

static char* process_internal_functions(char *sql) {
    if (!sql) return sql;
    sql = replace_marker(sql, "/* PG_INTERNAL_FUNC: changes */", affected_rows);
    sql = replace_marker(sql, "/* PG_INTERNAL_FUNC: total_changes */", affected_rows);
    return sql;
}

static void update_rowid_and_changes(PGconn *conn, PGresult *res) {
    if (!conn || !res) return;
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        const char *cmd_tuples = PQcmdTuples(res);
        if (cmd_tuples) affected_rows = atoi(cmd_tuples);
        const char *cmd_status = PQcmdStatus(res);
        if (cmd_status && strncmp(cmd_status, "INSERT", 6) == 0) {
            PGresult *rowid_res = PQexec(conn, "SELECT lastval();");
            if (rowid_res && PQresultStatus(rowid_res) == PGRES_TUPLES_OK && PQntuples(rowid_res) > 0) {
                last_rowid = atoll(PQgetvalue(rowid_res, 0, 0));
            }
            if (rowid_res) PQclear(rowid_res);
        }
    }
}

static void connect_to_postgres() {
    if (pg_conn) return;
    const char *conninfo = getenv("PG_CONNINFO");
    if (!conninfo) conninfo = "";
    if (conninfo[0] != '\0') printf("[HOOK] Connecting to PostgreSQL with: %s\n", conninfo);
    else printf("[HOOK] Connecting to PostgreSQL using environment variables/defaults\n");
    pg_conn = PQconnectdb(conninfo);
    if (PQstatus(pg_conn) != CONNECTION_OK) {
        fprintf(stderr, "[HOOK] Connection to database failed: %s\n", PQerrorMessage(pg_conn));
        PQfinish(pg_conn);
        pg_conn = NULL;
    } else {
        printf("[HOOK] Connected to PostgreSQL successfully\n");
        PQexec(pg_conn, "CREATE COLLATION IF NOT EXISTS \"nocase\" (provider = icu, locale = 'und-u-ks-level2', deterministic = false);");
    }
}

int sqlite3_open(const char *filename, sqlite3 **ppDb) {
    initialize_hooks();
    printf("[HOOK] Intercepted sqlite3_open: %s\n", filename);
    connect_to_postgres();
    return original_sqlite3_open(filename, ppDb);
}

const char *sqlite3_errmsg(sqlite3* db) {
    initialize_hooks();
    if (last_sqlite_error != SQLITE_OK) return last_pg_error_msg;
    return original_sqlite3_errmsg(db);
}

int sqlite3_errcode(sqlite3* db) {
    initialize_hooks();
    if (last_sqlite_error != SQLITE_OK) return last_sqlite_error;
    return original_sqlite3_errcode(db);
}

int sqlite3_prepare_v2(sqlite3 *db, const char *zSql, int nByte, sqlite3_stmt **ppStmt, const char **pzTail) {
    initialize_hooks();
    printf("[HOOK] Intercepted sqlite3_prepare_v2: %s\n", zSql);
    char* translated_sql = translate_sql(zSql);
    if (translated_sql) {
        translated_sql = process_schema_aware_upsert(pg_conn, translated_sql);
        translated_sql = process_internal_functions(translated_sql);
        printf("[HOOK] Translated SQL: %s\n", translated_sql);
    }
    int rc = original_sqlite3_prepare_v2(db, zSql, nByte, ppStmt, pzTail);
    if (rc == SQLITE_OK && translated_sql) {
        add_mapping(*ppStmt, translated_sql);
        free_translated_sql(translated_sql);
    } else if (translated_sql) free_translated_sql(translated_sql);
    return rc;
}

int sqlite3_bind_blob(sqlite3_stmt *pStmt, int i, const void *zData, int nData, void (*xDel)(void*)) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m && i > 0 && i <= MAX_PARAMS) {
        char *hex = malloc(nData * 2 + 3);
        strcpy(hex, "\\x");
        for (int j = 0; j < nData; j++) sprintf(hex + 2 + j * 2, "%02x", ((unsigned char*)zData)[j]);
        if (m->param_values[i-1]) free(m->param_values[i-1]);
        m->param_values[i-1] = hex;
        if (i > m->num_params) m->num_params = i;
    }
    return original_sqlite3_bind_blob(pStmt, i, zData, nData, xDel);
}

int sqlite3_bind_int(sqlite3_stmt *pStmt, int i, int iValue) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m && i > 0 && i <= MAX_PARAMS) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", iValue);
        if (m->param_values[i-1]) free(m->param_values[i-1]);
        m->param_values[i-1] = strdup(buf);
        if (i > m->num_params) m->num_params = i;
    }
    return original_sqlite3_bind_int(pStmt, i, iValue);
}

int sqlite3_bind_text(sqlite3_stmt *pStmt, int i, const char *zData, int nData, void (*xDel)(void*)) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m && i > 0 && i <= MAX_PARAMS) {
        if (m->param_values[i-1]) free(m->param_values[i-1]);
        m->param_values[i-1] = strdup(zData);
        if (i > m->num_params) m->num_params = i;
    }
    return original_sqlite3_bind_text(pStmt, i, zData, nData, xDel);
}

int sqlite3_reset(sqlite3_stmt *pStmt) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m) {
        if (m->pg_res) { PQclear(m->pg_res); m->pg_res = NULL; }
        m->current_row = -1;
        clear_blob_buffers(m);
    }
    return original_sqlite3_reset(pStmt);
}

int sqlite3_step(sqlite3_stmt *pStmt) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m) {
        if (m->pg_res == NULL && m->translated_sql) {
            if (pg_conn) {
                printf("[HOOK] Executing on Postgres: %s with %d params\n", m->translated_sql, m->num_params);
                m->pg_res = PQexecParams(pg_conn, m->translated_sql, m->num_params, NULL, 
                                         (const char * const *)m->param_values, NULL, NULL, 0);
                if (m->pg_res) {
                    if (PQresultStatus(m->pg_res) != PGRES_COMMAND_OK && PQresultStatus(m->pg_res) != PGRES_TUPLES_OK) {
                        set_last_error(m->pg_res);
                        fprintf(stderr, "[HOOK] Postgres execution failed: %s\n", PQerrorMessage(pg_conn));
                        return last_sqlite_error;
                    }
                    update_rowid_and_changes(pg_conn, m->pg_res);
                    last_sqlite_error = SQLITE_OK;
                }
            }
        }
        if (m->pg_res) {
            m->current_row++;
            clear_blob_buffers(m);
            if (m->current_row < PQntuples(m->pg_res)) return SQLITE_ROW;
            return SQLITE_DONE;
        }
    }
    return original_sqlite3_step(pStmt);
}

#define BOOLOID 16
#define INT8OID 20
#define INT2OID 21
#define INT4OID 23
#define TEXTOID 25
#define FLOAT4OID 700
#define FLOAT8OID 701
#define TIMESTAMPOID 1114

int sqlite3_column_type(sqlite3_stmt *pStmt, int iCol) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m && m->pg_res && m->current_row >= 0) {
        Oid type = PQftype(m->pg_res, iCol);
        switch (type) {
            case INT2OID: case INT4OID: case INT8OID: return SQLITE_INTEGER;
            case FLOAT4OID: case FLOAT8OID: return SQLITE_FLOAT;
            case BYTEAOID: return SQLITE_BLOB;
            case BOOLOID: case TEXTOID: case TIMESTAMPOID: return SQLITE_TEXT;
            default: return SQLITE_TEXT;
        }
    }
    return SQLITE_TEXT;
}

const void *sqlite3_column_blob(sqlite3_stmt *pStmt, int iCol) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m && m->pg_res && m->current_row >= 0 && m->current_row < PQntuples(m->pg_res)) {
        if (PQgetisnull(m->pg_res, m->current_row, iCol)) return NULL;
        if (m->blob_buffers[iCol]) return m->blob_buffers[iCol];
        size_t len;
        unsigned char *data = PQunescapeBytea((unsigned char*)PQgetvalue(m->pg_res, m->current_row, iCol), &len);
        if (data) {
            m->blob_buffers[iCol] = malloc(len);
            memcpy(m->blob_buffers[iCol], data, len);
            m->blob_sizes[iCol] = (int)len;
            PQfreemem(data);
            return m->blob_buffers[iCol];
        }
    }
    return original_sqlite3_column_blob(pStmt, iCol);
}

int sqlite3_column_bytes(sqlite3_stmt *pStmt, int iCol) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m && m->pg_res && m->current_row >= 0 && m->current_row < PQntuples(m->pg_res)) {
        if (PQgetisnull(m->pg_res, m->current_row, iCol)) return 0;
        if (m->blob_sizes[iCol] > 0) return m->blob_sizes[iCol];
        return (int)strlen(PQgetvalue(m->pg_res, m->current_row, iCol));
    }
    return original_sqlite3_column_bytes(pStmt, iCol);
}

const unsigned char *sqlite3_column_text(sqlite3_stmt *pStmt, int iCol) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m && m->pg_res && m->current_row >= 0 && m->current_row < PQntuples(m->pg_res)) {
        if (PQgetisnull(m->pg_res, m->current_row, iCol)) return NULL;
        return (const unsigned char*)PQgetvalue(m->pg_res, m->current_row, iCol);
    }
    return original_sqlite3_column_text(pStmt, iCol);
}

int sqlite3_column_int(sqlite3_stmt *pStmt, int iCol) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m && m->pg_res && m->current_row >= 0) return atoi(PQgetvalue(m->pg_res, m->current_row, iCol));
    return original_sqlite3_column_int(pStmt, iCol);
}

sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *pStmt, int iCol) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m && m->pg_res && m->current_row >= 0) return atoll(PQgetvalue(m->pg_res, m->current_row, iCol));
    return original_sqlite3_column_int64(pStmt, iCol);
}

double sqlite3_column_double(sqlite3_stmt *pStmt, int iCol) {
    initialize_hooks();
    StmtMapping *m = get_mapping(pStmt);
    if (m && m->pg_res && m->current_row >= 0) return atof(PQgetvalue(m->pg_res, m->current_row, iCol));
    return original_sqlite3_column_double(pStmt, iCol);
}

sqlite3_int64 sqlite3_last_insert_rowid(sqlite3* db) {
    initialize_hooks();
    return last_rowid;
}

int sqlite3_changes(sqlite3* db) {
    initialize_hooks();
    return affected_rows;
}

int sqlite3_finalize(sqlite3_stmt *pStmt) {
    initialize_hooks();
    printf("[HOOK] Intercepted sqlite3_finalize\n");
    remove_mapping(pStmt);
    return original_sqlite3_finalize(pStmt);
}

int sqlite3_exec(sqlite3* db, const char *sql, int (*callback)(void*,int,char**,char**), void *arg, char **errmsg) {
    initialize_hooks();
    printf("[HOOK] Intercepted sqlite3_exec: %s\n", sql);
    char* translated_sql = translate_sql(sql);
    if (translated_sql) {
        translated_sql = process_schema_aware_upsert(pg_conn, translated_sql);
        translated_sql = process_internal_functions(translated_sql);
        printf("[HOOK] Translated SQL: %s\n", translated_sql);
        if (pg_conn) {
            printf("[HOOK] Forwarding query to PostgreSQL...\n");
            PGresult *res = PQexec(pg_conn, translated_sql);
            if (res) {
                if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK) {
                    set_last_error(res);
                    fprintf(stderr, "[HOOK] Postgres execution failed: %s\n", PQerrorMessage(pg_conn));
                    if (errmsg) *errmsg = strdup(last_pg_error_msg);
                    PQclear(res);
                    free(translated_sql);
                    return last_sqlite_error;
                }
                update_rowid_and_changes(pg_conn, res);
                last_sqlite_error = SQLITE_OK;
                PQclear(res);
            }
        }
        free_translated_sql(translated_sql);
    }
    return original_sqlite3_exec(db, sql, callback, arg, errmsg);
}
