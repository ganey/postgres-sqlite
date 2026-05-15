use regex::Regex;

fn main() {
    let s = "CREATE VIRTUAL TABLE fts_table USING fts4(content='my_table', tokenize=collating)";
    let re = Regex::new(r"(?i)[,\s]*tokenize\s*=\s*collating\b").unwrap();
    let s2 = re.replace_all(&s, "").to_string();
    println!("{}", s2);
}
