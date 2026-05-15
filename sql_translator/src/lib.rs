use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use sqlparser::dialect::SQLiteDialect;
use sqlparser::parser::Parser;
use sqlparser::ast::{Statement, SqliteOnConflict};

#[no_mangle]
pub extern "C" fn translate_sql(sql: *const c_char) -> *mut c_char {
    if sql.is_null() {
        return std::ptr::null_mut();
    }

    let c_str = unsafe { CStr::from_ptr(sql) };
    let sql_str = match c_str.to_str() {
        Ok(s) => s,
        Err(_) => return std::ptr::null_mut(),
    };

    let translated = perform_translation(sql_str);
    
    let c_string = CString::new(translated).unwrap();
    c_string.into_raw()
}

fn perform_translation(sql: &str) -> String {
    let dialect = SQLiteDialect {};
    
    let preprocessed = preprocess_sql(sql);

    if let Some(translated_pragma) = handle_pragma(&preprocessed) {
        return translated_pragma;
    }

    if is_transaction_start(&preprocessed) {
        return "BEGIN".to_string();
    }

    let ast = match Parser::parse_sql(&dialect, &preprocessed) {
        Ok(ast) => ast,
        Err(_) => return fallback_translation(&preprocessed),
    };

    let mut translated_statements = Vec::new();
    for mut stmt in ast {
        let upsert_info = handle_statement(&mut stmt);
        let mut s = stmt.to_string();
        if let Some(table_name) = upsert_info {
            s = format!("{} /* PG_UPSERT: {} */", s, table_name);
        }
        translated_statements.push(s);
    }

    let joined = translated_statements.join("; ");
    convert_placeholders(&joined)
}

fn convert_placeholders(sql: &str) -> String {
    let mut result = String::new();
    let mut count = 1;
    let mut in_quote = false;
    let mut chars = sql.chars().peekable();

    while let Some(c) = chars.next() {
        match c {
            '\'' => {
                in_quote = !in_quote;
                result.push(c);
            }
            '?' if !in_quote => {
                result.push_str(&format!("${}", count));
                count += 1;
            }
            _ => result.push(c),
        }
    }
    result
}

fn is_transaction_start(sql: &str) -> bool {
    let upper = sql.to_uppercase().trim().to_string();
    upper.starts_with("BEGIN TRANSACTION") || 
    upper.starts_with("BEGIN IMMEDIATE") || 
    upper.starts_with("BEGIN EXCLUSIVE") ||
    upper == "BEGIN"
}

fn handle_pragma(sql: &str) -> Option<String> {
    let upper = sql.to_uppercase().trim().to_string();
    if upper.starts_with("PRAGMA TABLE_INFO") {
        let re = regex::Regex::new(r#"(?i)PRAGMA\s+table_info\s*\(?['"]?(\w+)['"]?\)?"#).unwrap();
        if let Some(caps) = re.captures(sql) {
            let table_name = &caps[1];
            return Some(format!(
                "SELECT ordinal_position - 1 as cid, column_name as name, data_type as type, \
                 CASE WHEN is_nullable = 'NO' THEN 1 ELSE 0 END as notnull, \
                 column_default as dflt_value, \
                 0 as pk \
                 FROM information_schema.columns \
                 WHERE table_name = '{}' \
                 ORDER BY ordinal_position",
                table_name
            ));
        }
    } else if upper.starts_with("PRAGMA USER_VERSION") {
        if upper.contains("=") {
             return Some("SELECT 0".to_string());
        } else {
             return Some("SELECT 0 as user_version".to_string());
        }
    }
    None
}

fn preprocess_sql(sql: &str) -> String {
    let mut s = sql.to_string();
    if s.contains("INTEGER PRIMARY KEY AUTOINCREMENT") {
        s = s.replace("INTEGER PRIMARY KEY AUTOINCREMENT", "SERIAL PRIMARY KEY");
    } else if s.contains("INTEGER PRIMARY KEY") {
        s = s.replace("INTEGER PRIMARY KEY", "SERIAL PRIMARY KEY");
    }
    
    // Map SQLite NOCASE to our custom PG collation
    if s.to_uppercase().contains("COLLATE NOCASE") {
        let re = regex::Regex::new(r"(?i)COLLATE\s+NOCASE").unwrap();
        s = re.replace_all(&s, "COLLATE \"nocase\"").to_string();
    }

    // Map SQLite strftime to PG to_char
    s = translate_strftime(&s);

    // Map SQLite global functions
    s = translate_functions(&s);
    
    s
}

fn translate_functions(sql: &str) -> String {
    let mut result = sql.to_string();
    
    // 1. last_insert_rowid() -> lastval()
    let re_last_id = regex::Regex::new(r#"(?i)last_insert_rowid\s*\(\s*\)"#).unwrap();
    result = re_last_id.replace_all(&result, "lastval()").to_string();

    // 2. ifnull(x, y) -> coalesce(x, y)
    let re_ifnull = regex::Regex::new(r#"(?i)ifnull\s*\("#).unwrap();
    result = re_ifnull.replace_all(&result, "coalesce(").to_string();

    // 3. instr(s, f) -> strpos(s, f)
    let re_instr = regex::Regex::new(r#"(?i)instr\s*\("#).unwrap();
    result = re_instr.replace_all(&result, "strpos(").to_string();

    // 4. changes() and total_changes()
    // These need internal hook values. We'll map them to markers.
    let re_changes = regex::Regex::new(r#"(?i)changes\s*\(\s*\)"#).unwrap();
    result = re_changes.replace_all(&result, "/* PG_INTERNAL_FUNC: changes */ 0").to_string();

    let re_total_changes = regex::Regex::new(r#"(?i)total_changes\s*\(\s*\)"#).unwrap();
    result = re_total_changes.replace_all(&result, "/* PG_INTERNAL_FUNC: total_changes */ 0").to_string();

    result
}

fn translate_strftime(sql: &str) -> String {
    let mut result = sql.to_string();
    let re_now = regex::Regex::new(r#"(?i)strftime\s*\(\s*'([^']+)'\s*,\s*'now'\s*\)"#).unwrap();
    result = re_now.replace_all(&result, |caps: &regex::Captures| {
        let fmt = &caps[1];
        if fmt == "%s" {
            return "extract(epoch from NOW())::bigint".to_string();
        }
        let pg_fmt = map_strftime_format(fmt);
        format!("to_char(NOW(), '{}')", pg_fmt)
    }).to_string();

    let re_col = regex::Regex::new(r#"(?i)strftime\s*\(\s*'([^']+)'\s*,\s*([a-zA-Z_]\w*)\s*\)"#).unwrap();
    result = re_col.replace_all(&result, |caps: &regex::Captures| {
        let fmt = &caps[1];
        let col = &caps[2];
        if fmt == "%s" {
            return format!("extract(epoch from {})::bigint", col);
        }
        let pg_fmt = map_strftime_format(fmt);
        format!("to_char({}, '{}')", col, pg_fmt)
    }).to_string();

    result
}

fn map_strftime_format(sqlite_fmt: &str) -> String {
    sqlite_fmt.replace("%Y", "YYYY")
              .replace("%m", "MM")
              .replace("%d", "DD")
              .replace("%H", "HH24")
              .replace("%M", "MI")
              .replace("%S", "SS")
              .replace("%f", "SS.MS")
}

fn handle_statement(stmt: &mut Statement) -> Option<String> {
    match stmt {
        Statement::Insert(insert) => {
            if let Some(SqliteOnConflict::Replace) = insert.or {
                insert.or = None;
                // Return table name to trigger schema-aware UPSERT in C hook
                let table_name = insert.table_name.to_string();
                return Some(table_name);
            }
        }
        _ => {}
    }
    None
}

fn fallback_translation(sql: &str) -> String {
    let mut translated = sql.to_string();
    translated = translated.replace("DATETIME('now')", "NOW()");
    
    if translated.to_uppercase().contains("INSERT OR REPLACE") {
        translated = translated.replace("INSERT OR REPLACE", "INSERT");
        // We could extract table name with regex here for fallback
    }
    
    convert_placeholders(&translated)
}

#[no_mangle]
pub extern "C" fn free_translated_sql(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        let _ = CString::from_raw(ptr);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_upsert_marker() {
        let input = "INSERT OR REPLACE INTO Friends (Id, Name) VALUES (1, 'micha')";
        let output = perform_translation(input);
        assert!(output.contains("/* PG_UPSERT: Friends */"));
    }

    #[test]
    fn test_strftime() {
        let input = "SELECT strftime('%Y-%m-%d', 'now')";
        let output = perform_translation(input).to_uppercase();
        assert!(output.contains("TO_CHAR"));
        assert!(output.contains("NOW()"));
        assert!(output.contains("YYYY-MM-DD"));
    }

    #[test]
    fn test_placeholders() {
        let input = "INSERT INTO users (name, age) VALUES (?, ?)";
        let output = perform_translation(input);
        assert!(output.contains("$1"));
        assert!(output.contains("$2"));
    }

    #[test]
    fn test_placeholders_in_quotes() {
        let input = "SELECT * FROM users WHERE name = '?' AND age = ?";
        let output = perform_translation(input);
        assert!(output.contains("'?'"));
        assert!(output.contains("$1"));
        assert!(!output.contains("$2"));
    }

    #[test]
    fn test_transactions() {
        assert_eq!(perform_translation("BEGIN IMMEDIATE TRANSACTION"), "BEGIN");
        assert_eq!(perform_translation("BEGIN EXCLUSIVE"), "BEGIN");
        assert_eq!(perform_translation("COMMIT"), "COMMIT");
        assert_eq!(perform_translation("ROLLBACK"), "ROLLBACK");
    }

    #[test]
    fn test_pragma_table_info() {
        let input = "PRAGMA table_info('users')";
        let output = perform_translation(input);
        assert!(output.contains("information_schema.columns"));
        assert!(output.contains("WHERE table_name = 'users'"));
    }
}
