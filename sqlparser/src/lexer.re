// re2c --lang c
#include "internal.h"
#include "sqlparser_grammar_gen.h"

/* The document owns an exact-length copy plus one NUL sentinel. YYLIMIT marks
 * that sentinel; embedded NUL has already been rejected at the API boundary. */
int sqlp_lex(sqlp_lexer *lexer, sqlparser_span *token) {
  const char *YYCURSOR = lexer->cursor;
  const char *YYLIMIT = lexer->end;
  const char *YYMARKER;
  const char *start;
  int kind;
  const bool sqlite = lexer->dialect == SQLPARSER_SQLITE;
  for (;;) {
    start = YYCURSOR;
    if (sqlite) {
      /*!re2c
        re2c:define:YYCTYPE = "unsigned char";
        re2c:yyfill:enable = 0;
        re2c:eof = 0;
        sqlite_word = [a-zA-Z_\x80-\xff][a-zA-Z_0-9$\x80-\xff]*;
        sqlite_digits = [0-9] ("_"? [0-9])*;
        sqlite_exponent = [eE][+-]? sqlite_digits;
        "--" [^\n\x00]* { continue; }
        "'" ([^'\x00] | "''")* "'" { kind = SQLTK_STRING; break; }
        '"' ([^"\x00] | '""')* '"' { kind = SQLTK_QID; break; }
        "[" [^\]\x00]* "]" { kind = SQLTK_QID; break; }
        "'" | '"' | "[" { kind = -1; break; }
        [xX] "'" ([0-9a-fA-F][0-9a-fA-F])* "'" { kind = SQLTK_BLOB; break; }
        [xX] "'" { kind = -1; break; }
        "0" [xX] [0-9a-fA-F] ("_"? [0-9a-fA-F])* { kind = SQLTK_NUMBER; break; }
        sqlite_digits ("." sqlite_digits?)? sqlite_exponent? |
        "." sqlite_digits sqlite_exponent? { kind = SQLTK_NUMBER; break; }
        (sqlite_digits ("." sqlite_digits?)? sqlite_exponent? |
         "." sqlite_digits sqlite_exponent?) sqlite_word { kind = -1; break; }
        "?" [0-9]+ { kind = SQLTK_PARAMETER; break; }
        [:@] sqlite_word | "$" sqlite_word ("::" sqlite_word)* ("(" [^ \t\r\n)\x00]* ")")? {
          kind = SQLTK_PARAMETER; break;
        }
        "==" { kind = SQLTK_EQ; break; }
        "||" { kind = SQLTK_CONCAT; break; }
        "&&" | "<=>" | "@@" | "#" { kind = -1; break; }
        $ { kind = 0; break; }
        * { YYCURSOR = start; goto common_token; }
      */
    } else {
      if (lexer->mysql_no_backslash_escapes) {
        /*!re2c
          re2c:define:YYCTYPE = "unsigned char";
          re2c:yyfill:enable = 0;
          re2c:eof = 0;
          "'" ([^'\x00] | "''")* "'" |
          '"' ([^"\x00] | '""')* '"' { kind = SQLTK_STRING; break; }
          "'" | '"' { kind = -1; break; }
          $ { kind = 0; break; }
          * { YYCURSOR = start; goto mysql_name; }
        */
      }
mysql_name:
      // A qualifier's dot is a separator, never the start of a decimal literal.
      if (lexer->mysql_after_name && *YYCURSOR == '.') {
        ++YYCURSOR; kind = SQLTK_DOT; break;
      }
      if (lexer->mysql_qualified) {
        /*!re2c
          re2c:define:YYCTYPE = "unsigned char";
          re2c:yyfill:enable = 0;
          re2c:eof = 0;
          [a-zA-Z_0-9$\x80-\xff]+ { kind = SQLTK_ID; break; }
          $ { kind = 0; break; }
          * { YYCURSOR = start; goto mysql_literal; }
        */
      }
mysql_literal:
      /*!re2c
        re2c:define:YYCTYPE = "unsigned char";
        re2c:yyfill:enable = 0;
        re2c:eof = 0;
        // MySQL recognizes a complete exponent before a following identifier.
        [0-9]+ [eE][+-]? [0-9]+ { kind = SQLTK_NUMBER; break; }
        "0x" [0-9a-fA-F]+ / [^a-zA-Z_0-9$\x80-\xff] { kind = SQLTK_BLOB; break; }
        "0b" [01]+ / [^a-zA-Z_0-9$\x80-\xff] { kind = SQLTK_BLOB; break; }
        [xX] "'" ([0-9a-fA-F][0-9a-fA-F])* "'" |
        [bB] "'" [01]* "'" { kind = SQLTK_BLOB; break; }
        [xXbB] "'" { kind = -1; break; }
        $ { kind = 0; break; }
        * { YYCURSOR = start; goto common_token; }
      */
    }
common_token:
    /*!re2c
      re2c:define:YYCTYPE = "unsigned char";
      re2c:yyfill:enable = 0;
      re2c:eof = 0;
      word = [a-zA-Z_\x80-\xff][a-zA-Z_0-9$\x80-\xff]*;
      digits = [0-9]+;
      exponent = [eE][+-]? digits;
      [ \t\r\n\f]+ { continue; }
      "--" ([ \t\r\f] [^\n\x00]* | "\n") | "#" [^\n\x00]* { continue; }
      "/*" ([^*\x00] | "*"+ [^*/\x00])* "*"+ "/" {
        if (!sqlite && (start[2] == '!' || start[2] == '+')) { kind = -1; break; }
        continue;
      }
      "/*" ([^*\x00] | "*"+ [^*/\x00])* "*"* {
        if (sqlite && YYCURSOR == YYLIMIT) continue;
        kind = -1; break;
      }
      "'" ([^'\\\x00] | "''" | "\\" [^\x00])* "'" |
      '"' ([^"\\\x00] | '""' | "\\" [^\x00])* '"' { kind = SQLTK_STRING; break; }
      "`" ([^`\x00] | "``")* "`" { kind = SQLTK_QID; break; }
      digits ("." [0-9]*)? exponent? | "." digits exponent? { kind = SQLTK_NUMBER; break; }
      digits [a-zA-Z_$\x80-\xff][a-zA-Z_0-9$\x80-\xff]* { kind = sqlite ? -1 : SQLTK_ID; break; }
      "?" { kind = SQLTK_PARAMETER; break; }
      "@" "@"? word { kind = SQLTK_VARIABLE; break; }
      'select' { kind = SQLTK_SELECT; break; }
      'distinct' { kind = SQLTK_DISTINCT; break; }
      'all' { kind = SQLTK_ALL; break; }
      'from' { kind = SQLTK_FROM; break; }
      'where' { kind = SQLTK_WHERE; break; }
      'group' { kind = SQLTK_GROUP; break; }
      'by' { kind = SQLTK_BY; break; }
      'having' { kind = SQLTK_HAVING; break; }
      'order' { kind = SQLTK_ORDER; break; }
      'asc' { kind = SQLTK_ASC; break; }
      'desc' { kind = SQLTK_DESC; break; }
      'limit' { kind = SQLTK_LIMIT; break; }
      'offset' { kind = SQLTK_OFFSET; break; }
      'as' { kind = SQLTK_AS; break; }
      'join' { kind = SQLTK_JOIN; break; }
      'lateral' { kind = sqlite ? SQLTK_ID : SQLTK_LATERAL; break; }
      'inner' { kind = SQLTK_INNER; break; }
      'left' { kind = SQLTK_LEFT; break; }
      'right' { kind = SQLTK_RIGHT; break; }
      'outer' { kind = SQLTK_OUTER; break; }
      'cross' { kind = SQLTK_CROSS; break; }
      'natural' { kind = SQLTK_NATURAL; break; }
      'on' { kind = SQLTK_ON; break; }
      'using' { kind = SQLTK_USING; break; }
      'union' { kind = SQLTK_UNION; break; }
      'insert' { kind = SQLTK_INSERT; break; }
      'replace' { kind = SQLTK_REPLACE; break; }
      'into' { kind = SQLTK_INTO; break; }
      'values' { kind = SQLTK_VALUES; break; }
      'update' { kind = SQLTK_UPDATE; break; }
      'delete' { kind = SQLTK_DELETE; break; }
      'duplicate' { kind = sqlite ? SQLTK_ID : SQLTK_DUPLICATE; break; }
      'set' { kind = SQLTK_SET; break; }
      'and' | "&&" { kind = SQLTK_AND; break; }
      'or' | "||" { kind = SQLTK_OR; break; }
      'not' { kind = SQLTK_NOT; break; }
      'is' { kind = SQLTK_IS; break; }
      'null' { kind = SQLTK_NULL; break; }
      'true' | 'false' { kind = SQLTK_BOOLEAN; break; }
      'between' { kind = SQLTK_BETWEEN; break; }
      'in' { kind = SQLTK_IN; break; }
      'like' { kind = SQLTK_LIKE; break; }
      'escape' { kind = SQLTK_ESCAPE; break; }
      'exists' { kind = SQLTK_EXISTS; break; }
      'case' { kind = SQLTK_CASE; break; }
      'when' { kind = SQLTK_WHEN; break; }
      'then' { kind = SQLTK_THEN; break; }
      'else' { kind = SQLTK_ELSE; break; }
      'end' { kind = SQLTK_END; break; }
      'begin' { kind = SQLTK_BEGIN; break; }
      'start' { kind = sqlite ? SQLTK_ID : SQLTK_START; break; }
      'transaction' { kind = SQLTK_TRANSACTION; break; }
      'commit' { kind = SQLTK_COMMIT; break; }
      'rollback' { kind = SQLTK_ROLLBACK; break; }
      'work' { kind = sqlite ? SQLTK_ID : SQLTK_WORK; break; }
      'names' { kind = sqlite ? SQLTK_ID : SQLTK_NAMES; break; }
      'character' { kind = sqlite ? SQLTK_ID : SQLTK_CHARACTER; break; }
      'global' { kind = sqlite ? SQLTK_ID : SQLTK_GLOBAL; break; }
      'session' { kind = sqlite ? SQLTK_ID : SQLTK_SESSION; break; }
      'local' { kind = sqlite ? SQLTK_ID : SQLTK_LOCAL; break; }
      'show' { kind = sqlite ? SQLTK_ID : SQLTK_SHOW; break; }
      'databases' | 'schemas' { kind = sqlite ? SQLTK_ID : SQLTK_DATABASES; break; }
      'tables' { kind = sqlite ? SQLTK_ID : SQLTK_TABLES; break; }
      'table' { kind = SQLTK_TABLE; break; }
      'status' { kind = SQLTK_STATUS; break; }
      'warnings' { kind = sqlite ? SQLTK_ID : SQLTK_WARNINGS; break; }
      'variables' { kind = SQLTK_VARIABLES; break; }
      'collation' { kind = sqlite ? SQLTK_ID : SQLTK_COLLATION; break; }
      'full' { kind = SQLTK_FULL; break; }
      'create' { kind = SQLTK_CREATE; break; }
      'temporary' { kind = SQLTK_TEMPORARY; break; }
      'if' { kind = SQLTK_IF; break; }
      'primary' { kind = SQLTK_PRIMARY; break; }
      'key' { kind = SQLTK_KEY; break; }
      'unique' { kind = SQLTK_UNIQUE; break; }
      'index' { kind = SQLTK_INDEX; break; }
      'default' { kind = SQLTK_DEFAULT; break; }
      'auto_increment' { kind = sqlite ? SQLTK_ID : SQLTK_AUTO_INCREMENT; break; }
      'unsigned' { kind = sqlite ? SQLTK_ID : SQLTK_UNSIGNED; break; }
      'references' { kind = SQLTK_REFERENCES; break; }
      'check' { kind = SQLTK_CHECK; break; }
      'constraint' { kind = SQLTK_CONSTRAINT; break; }
      'foreign' { kind = SQLTK_FOREIGN; break; }
      'engine' { kind = SQLTK_ENGINE; break; }
      'charset' { kind = SQLTK_CHARSET; break; }
      'collate' { kind = SQLTK_COLLATE; break; }
      'drop' { kind = SQLTK_DROP; break; }
      'cast' { kind = sqlite || *YYCURSOR == '(' ? SQLTK_CAST : SQLTK_ID; break; }
      /* MySQL 8.4's special function names are keywords only immediately
       * before '('. ADDDATE/SUBDATE/SESSION_USER/SYSTEM_USER remain identifiers
       * even there, as documented by the upstream parser.test cases. */
      'count' { kind = !sqlite && *YYCURSOR == '(' ? SQLTK_COUNT : SQLTK_ID; break; }
      'bit_and' | 'bit_or' | 'bit_xor' | 'curdate' | 'curtime' |
      'date_add' | 'date_sub' | 'extract' | 'group_concat' | 'max' | 'mid' |
      'min' | 'position' | 'std' | 'stddev' | 'stddev_pop' |
      'stddev_samp' | 'substr' | 'substring' | 'sum' | 'sysdate' | 'trim' |
      'variance' | 'var_pop' | 'var_samp' {
        kind = !sqlite && *YYCURSOR == '(' ? SQLTK_MYSQL_FUNCTION : SQLTK_ID;
        break;
      }
      'triggers' { kind = sqlite ? SQLTK_ID : SQLTK_TRIGGERS; break; }
      'events' { kind = sqlite ? SQLTK_ID : SQLTK_EVENTS; break; }
      'open' { kind = sqlite ? SQLTK_ID : SQLTK_OPEN; break; }
      'columns' { kind = sqlite ? SQLTK_ID : SQLTK_COLUMNS; break; }
      'fields' { kind = sqlite ? SQLTK_ID : SQLTK_FIELDS; break; }
      'keys' { kind = sqlite ? SQLTK_ID : SQLTK_KEYS; break; }
      'indexes' { kind = sqlite ? SQLTK_ID : SQLTK_INDEXES; break; }
      'procedure' { kind = sqlite ? SQLTK_ID : SQLTK_PROCEDURE; break; }
      'function' { kind = sqlite ? SQLTK_ID : SQLTK_FUNCTION; break; }
      'extended' { kind = sqlite ? SQLTK_ID : SQLTK_EXTENDED; break; }
      'chain' { kind = sqlite ? SQLTK_ID : SQLTK_CHAIN; break; }
      'read' { kind = sqlite ? SQLTK_ID : SQLTK_READ; break; }
      'write' { kind = sqlite ? SQLTK_ID : SQLTK_WRITE; break; }
      'only' { kind = sqlite ? SQLTK_ID : SQLTK_ONLY; break; }
      'consistent' { kind = sqlite ? SQLTK_ID : SQLTK_CONSISTENT; break; }
      'snapshot' { kind = sqlite ? SQLTK_ID : SQLTK_SNAPSHOT; break; }
      'isolation' { kind = sqlite ? SQLTK_ID : SQLTK_ISOLATION; break; }
      'level' { kind = sqlite ? SQLTK_ID : SQLTK_LEVEL; break; }
      'repeatable' { kind = sqlite ? SQLTK_ID : SQLTK_REPEATABLE; break; }
      'committed' { kind = sqlite ? SQLTK_ID : SQLTK_COMMITTED; break; }
      'uncommitted' { kind = sqlite ? SQLTK_ID : SQLTK_UNCOMMITTED; break; }
      'serializable' { kind = sqlite ? SQLTK_ID : SQLTK_SERIALIZABLE; break; }
      'lock' { kind = sqlite ? SQLTK_ID : SQLTK_LOCK; break; }
      'unlock' { kind = sqlite ? SQLTK_ID : SQLTK_UNLOCK; break; }
      'option' { kind = sqlite ? SQLTK_ID : SQLTK_OPTION; break; }
      'cascaded' { kind = sqlite ? SQLTK_ID : SQLTK_CASCADED; break; }
      'format' { kind = sqlite ? SQLTK_ID : SQLTK_FORMAT; break; }
      'traditional' { kind = sqlite ? SQLTK_ID : SQLTK_TRADITIONAL; break; }
      'json' { kind = sqlite ? SQLTK_ID : SQLTK_JSON; break; }
      'tree' { kind = sqlite ? SQLTK_ID : SQLTK_TREE; break; }
      'over' { kind = sqlite ? SQLTK_ID : SQLTK_OVER; break; }
      'window' { kind = sqlite ? SQLTK_ID : SQLTK_WINDOW; break; }
      'rows' { kind = sqlite ? SQLTK_ID : SQLTK_ROWS; break; }
      'range' { kind = sqlite ? SQLTK_ID : SQLTK_RANGE; break; }
      'current' { kind = sqlite ? SQLTK_ID : SQLTK_CURRENT; break; }
      'unbounded' { kind = sqlite ? SQLTK_ID : SQLTK_UNBOUNDED; break; }
      'preceding' { kind = sqlite ? SQLTK_ID : SQLTK_PRECEDING; break; }
      'following' { kind = sqlite ? SQLTK_ID : SQLTK_FOLLOWING; break; }
      'interval' { kind = sqlite ? SQLTK_ID : SQLTK_INTERVAL; break; }
      'partition' { kind = sqlite ? SQLTK_ID : SQLTK_PARTITION; break; }
      'prepare' { kind = sqlite ? SQLTK_ID : SQLTK_PREPARE; break; }
      'xor' { kind = sqlite ? SQLTK_ID : SQLTK_XOR; break; }
      'low_priority' { kind = sqlite ? SQLTK_ID : SQLTK_LOW_PRIORITY; break; }
      'execute' { kind = sqlite ? SQLTK_ID : SQLTK_EXECUTE; break; }
      'deallocate' { kind = sqlite ? SQLTK_ID : SQLTK_DEALLOCATE; break; }
      'truncate' { kind = sqlite ? SQLTK_ID : SQLTK_TRUNCATE; break; }
      'zerofill' { kind = sqlite ? SQLTK_ID : SQLTK_ZEROFILL; break; }
      'current_timestamp' | 'localtime' | 'localtimestamp' { kind = sqlite ? SQLTK_ID : SQLTK_CURRENT_TIMESTAMP; break; }
      'now' { kind = !sqlite && *YYCURSOR == '(' ? SQLTK_CURRENT_TIMESTAMP : SQLTK_ID; break; }
      'sql_calc_found_rows' { kind = sqlite ? SQLTK_ID : SQLTK_MYSQL_CALC_FOUND_ROWS; break; }
      'pragma' { kind = sqlite ? SQLTK_PRAGMA : SQLTK_ID; break; }
      'temp' { kind = sqlite ? SQLTK_TEMPORARY : SQLTK_ID; break; }
      'deferred' { kind = sqlite ? SQLTK_DEFERRED : SQLTK_ID; break; }
      'immediate' { kind = sqlite ? SQLTK_IMMEDIATE : SQLTK_ID; break; }
      'exclusive' { kind = sqlite ? SQLTK_EXCLUSIVE : SQLTK_ID; break; }
      'savepoint' { kind = SQLTK_SAVEPOINT; break; }
      'release' { kind = SQLTK_RELEASE; break; }
      'to' { kind = SQLTK_TO; break; }
      'view' { kind = SQLTK_VIEW; break; }
      'attach' { kind = sqlite ? SQLTK_ATTACH : SQLTK_ID; break; }
      'detach' { kind = sqlite ? SQLTK_DETACH : SQLTK_ID; break; }
      'database' { kind = sqlite ? SQLTK_DATABASE : SQLTK_ID; break; }
      'intersect' { kind = SQLTK_INTERSECT; break; }
      'except' { kind = SQLTK_EXCEPT; break; }
      'with' { kind = SQLTK_WITH; break; }
      'recursive' { kind = SQLTK_RECURSIVE; break; }
      'glob' { kind = sqlite ? SQLTK_GLOB : SQLTK_ID; break; }
      'regexp' { kind = sqlite ? SQLTK_REGEXP : SQLTK_ID; break; }
      'match' { kind = SQLTK_MATCH; break; }
      'isnull' { kind = sqlite ? SQLTK_ISNULL : SQLTK_ID; break; }
      'notnull' { kind = sqlite ? SQLTK_NOTNULL : SQLTK_ID; break; }
      'autoincrement' { kind = sqlite ? SQLTK_AUTOINCREMENT : SQLTK_ID; break; }
      'conflict' { kind = sqlite ? SQLTK_CONFLICT : SQLTK_ID; break; }
      'abort' { kind = sqlite ? SQLTK_ABORT : SQLTK_ID; break; }
      'fail' { kind = sqlite ? SQLTK_FAIL : SQLTK_ID; break; }
      'ignore' { kind = SQLTK_IGNORE; break; }
      'without' { kind = sqlite ? SQLTK_WITHOUT : SQLTK_ID; break; }
      'rowid' { kind = sqlite ? SQLTK_ROWID : SQLTK_ID; break; }
      'strict' { kind = sqlite ? SQLTK_STRICT : SQLTK_ID; break; }
      'virtual' { kind = sqlite ? SQLTK_VIRTUAL : SQLTK_ID; break; }
      'trigger' { kind = sqlite ? SQLTK_TRIGGER : SQLTK_ID; break; }
      'before' { kind = sqlite ? SQLTK_BEFORE : SQLTK_ID; break; }
      'after' { kind = SQLTK_AFTER; break; }
      'first' { kind = sqlite ? SQLTK_ID : SQLTK_FIRST; break; }
      'instead' { kind = sqlite ? SQLTK_INSTEAD : SQLTK_ID; break; }
      'of' { kind = sqlite ? SQLTK_OF : SQLTK_ID; break; }
      'for' { kind = sqlite ? SQLTK_FOR : SQLTK_ID; break; }
      'each' { kind = sqlite ? SQLTK_EACH : SQLTK_ID; break; }
      'row' { kind = SQLTK_ROW; break; }
      'raise' { kind = sqlite ? SQLTK_RAISE : SQLTK_ID; break; }
      'explain' { kind = SQLTK_EXPLAIN; break; }
      'describe' { kind = sqlite ? SQLTK_ID : SQLTK_EXPLAIN; break; }
      'query' { kind = sqlite ? SQLTK_QUERY : SQLTK_ID; break; }
      'plan' { kind = sqlite ? SQLTK_PLAN : SQLTK_ID; break; }
      'analyze' { kind = SQLTK_ANALYZE; break; }
      'reindex' { kind = sqlite ? SQLTK_REINDEX : SQLTK_ID; break; }
      'vacuum' { kind = sqlite ? SQLTK_VACUUM : SQLTK_ID; break; }
      'alter' { kind = SQLTK_ALTER; break; }
      'rename' { kind = SQLTK_RENAME; break; }
      'column' { kind = SQLTK_COLUMN; break; }
      'add' { kind = SQLTK_ADD; break; }
      'cascade' { kind = SQLTK_CASCADE; break; }
      'restrict' { kind = SQLTK_RESTRICT; break; }
      'no' { kind = SQLTK_NO; break; }
      'action' { kind = SQLTK_ACTION; break; }
      'partial' { kind = SQLTK_PARTIAL; break; }
      'simple' { kind = SQLTK_SIMPLE; break; }
      'deferrable' { kind = sqlite ? SQLTK_DEFERRABLE : SQLTK_ID; break; }
      'initially' { kind = sqlite ? SQLTK_INITIALLY : SQLTK_ID; break; }
      'indexed' { kind = sqlite ? SQLTK_INDEXED : SQLTK_ID; break; }

      "(" { kind = SQLTK_LP; break; }
      ")" { kind = SQLTK_RP; break; }
      "," { kind = SQLTK_COMMA; break; }
      ";" { kind = SQLTK_SEMI; break; }
      "." { kind = SQLTK_DOT; break; }
      "=" { kind = SQLTK_EQ; break; }
      ":=" { kind = sqlite ? -1 : SQLTK_ASSIGN; break; }
      "!=" | "<>" { kind = SQLTK_NE; break; }
      "<" { kind = SQLTK_LT; break; }
      "<=" { kind = SQLTK_LE; break; }
      ">" { kind = SQLTK_GT; break; }
      ">=" { kind = SQLTK_GE; break; }
      "<=>" { kind = SQLTK_NULL_SAFE_EQ; break; }
      "+" { kind = SQLTK_PLUS; break; }
      "-" { kind = SQLTK_MINUS; break; }
      "*" { kind = SQLTK_STAR; break; }
      "/" { kind = SQLTK_SLASH; break; }
      "%" { kind = SQLTK_MOD; break; }
      'mod' { kind = sqlite ? SQLTK_ID : SQLTK_MYSQL_MOD; break; }
      'div' { kind = sqlite ? SQLTK_ID : SQLTK_DIV; break; }
      "&" { kind = SQLTK_BITAND; break; }
      "|" { kind = SQLTK_BITOR; break; }
      "~" { kind = SQLTK_BITNOT; break; }
      "<<" { kind = SQLTK_LSHIFT; break; }
      ">>" { kind = SQLTK_RSHIFT; break; }
      word { kind = SQLTK_ID; break; }
      $ { kind = 0; break; }
      * { kind = -1; break; }
    */
  }
  *token = (sqlparser_span){(size_t)(start - lexer->base), (size_t)(YYCURSOR - start)};
  lexer->cursor = YYCURSOR;
  lexer->mysql_qualified = kind == SQLTK_DOT && lexer->mysql_after_name;
  lexer->mysql_after_name = kind == SQLTK_ID || kind == SQLTK_QID ||
      kind == SQLTK_STATUS || kind == SQLTK_VARIABLES || kind == SQLTK_ENGINE ||
      kind == SQLTK_CHARSET || kind == SQLTK_SAVEPOINT || kind == SQLTK_VIEW ||
      kind == SQLTK_TRIGGERS || kind == SQLTK_EVENTS || kind == SQLTK_OPEN ||
      kind == SQLTK_COLUMNS || kind == SQLTK_FIELDS || kind == SQLTK_INDEXES ||
      kind == SQLTK_FUNCTION || kind == SQLTK_EXTENDED || kind == SQLTK_CHAIN ||
      kind == SQLTK_NO || kind == SQLTK_ONLY || kind == SQLTK_CONSISTENT ||
      kind == SQLTK_SNAPSHOT || kind == SQLTK_ISOLATION || kind == SQLTK_LEVEL ||
      kind == SQLTK_REPEATABLE || kind == SQLTK_COMMITTED ||
      kind == SQLTK_UNCOMMITTED || kind == SQLTK_SERIALIZABLE || kind == SQLTK_TRUNCATE ||
      kind == SQLTK_PREPARE || kind == SQLTK_EXECUTE || kind == SQLTK_DEALLOCATE ||
      kind == SQLTK_FORMAT || kind == SQLTK_TRADITIONAL || kind == SQLTK_JSON || kind == SQLTK_TREE;
  return kind;
}
