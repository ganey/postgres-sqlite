fn fallback_translation(sql: &str) -> String {
    let mut translated = sql.to_string();
    translated = translated.replace("DATETIME('now')", "NOW()");

    if translated.to_uppercase().contains("INSERT OR REPLACE") {
        translated = translated.replace("INSERT OR REPLACE", "INSERT");
        // We could extract table name with regex here for fallback
    }

    if translated.to_uppercase().contains("CREATE VIRTUAL TABLE") {
         // Drop tokenizer=collating if it wasn't caught by preprocessing
         let re = regex::Regex::new(r"(?i)[,\s]*tokenize\s*=\s*collating\b").unwrap();
         translated = re.replace_all(&translated, "").to_string();
         // Postgres does not have virtual tables or FTS in the same way,
         // so we should probably map it to a regular table, or a custom one if possible.
    }
    translated
}
