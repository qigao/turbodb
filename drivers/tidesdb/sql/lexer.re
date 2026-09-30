// re2c --lang c
#include "sql.h"

orm_tidesdb_sql_token orm_tidesdb_sql_next(const char **cursor, const char *end) {
  const char *YYCURSOR = *cursor;
  const char *YYLIMIT = end;
  const char *YYMARKER;
  const char *start;
  int kind;
  for (;;) {
    start = YYCURSOR;
    /*!re2c
      re2c:define:YYCTYPE = "unsigned char";
      re2c:yyfill:enable = 0;
      re2c:eof = 0;
      [ \t\r\n]+ { continue; }
      [a-zA-Z_][a-zA-Z_0-9]* { kind = TDB_SQL_WORD; break; }
      "-"? [0-9]+ { kind = TDB_SQL_INTEGER; break; }
      "'" ([^'\x00] | "''")* "'" { kind = TDB_SQL_STRING; break; }
      "<=" { kind = TDB_SQL_LE; break; }
      ">=" { kind = TDB_SQL_GE; break; }
      "<>" | "!=" { kind = TDB_SQL_NE; break; }
      [(),;?=<>] { kind = (unsigned char)*start; break; }
      $ { kind = TDB_SQL_END; break; }
      * { kind = TDB_SQL_INVALID; break; }
    */
  }
  *cursor = YYCURSOR;
  return (orm_tidesdb_sql_token){kind, {start, (size_t)(YYCURSOR - start)}};
}
