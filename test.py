import re

sql = "CREATE VIRTUAL TABLE fts_table USING fts4(content='my_table', tokenize=collating)"

sql = re.sub(r'(?i)\bVIRTUAL\s+', '', sql)
sql = re.sub(r'(?i)\bUSING\s+fts\d+\s*\((.*?)\)', r'(\1)', sql)
print(sql)
