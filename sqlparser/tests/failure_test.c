#include "internal.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

/* Fault injection is confined to this test translation unit. The generated
 * parser uses the same AST helpers, so stack growth participates as well. */
static size_t fail_at, attempts;
static int live_blocks, live_strings, live_vectors;
static bool fail(void) { return ++attempts == fail_at; }
static void *allocate(size_t size) {
  if (fail()) return NULL;
  void *p = malloc(size);
  if (p) ++live_blocks;
  return p;
}
static void *allocate_zero(size_t count, size_t size) {
  if (fail()) return NULL;
  void *p = calloc(count, size);
  if (p) ++live_blocks;
  return p;
}
static void *resize(void *old, size_t size) {
  if (fail()) return NULL;
  const bool fresh = old == NULL;
  void *p = realloc(old, size);
  if (p && fresh) ++live_blocks;
  return p;
}
static void release(void *p) {
  if (p) --live_blocks;
  free(p);
}
static tstr copy_sql(const void *input, size_t size) {
  if (fail()) return NULL;
  tstr text = tstr_new_len(input, size);
  if (text) ++live_strings;
  return text;
}
static void release_sql(tstr text) {
  if (text) --live_strings;
  tstr_free(text);
}
static stl_status initialize_nodes(vec_t *nodes, size_t size, size_t alignment, size_t limit) {
  if (fail()) return STL_OUT_OF_MEMORY;
  stl_status status = vec_init_bytes(nodes, size, alignment, limit);
  if (status == STL_OK) ++live_vectors;
  return status;
}
static stl_status append_node(vec_t *nodes, const void *node) {
  return fail() ? STL_OUT_OF_MEMORY : vec_push(nodes, node);
}
static void release_nodes(vec_t *nodes) {
  if (nodes->initialized) --live_vectors;
  vec_destroy(nodes);
}

#define malloc allocate
#define calloc allocate_zero
#define realloc resize
#define free release
#define tstr_new_len copy_sql
#define tstr_free release_sql
#define vec_init_bytes initialize_nodes
#define vec_push append_node
#define vec_destroy release_nodes
#include "../src/parser.c"
#include "../src/ast.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef tstr_new_len
#undef tstr_free
#undef vec_init_bytes
#undef vec_push
#undef vec_destroy

spec("SQL parser allocation failure boundaries") {
  it("releases every owner and permits another request after each failure") {
    const char *query = "SELECT a FROM (SELECT id AS a FROM t) q WHERE a IN(1,2,3); INSERT INTO u VALUES(4),(5);";
    const struct { sqlparser_dialect dialect; const char *sql; } samples[] = {
      {SQLPARSER_MYSQL, query}, {SQLPARSER_SQLITE, query},
      {SQLPARSER_MYSQL,"SELECT CAST(CAST(? AS unsigned /* target */ integer) AS SIGNED INTEGER),CAST('12.9e3' AS DOUBLE),CAST(NULL AS REAL);"},
      {SQLPARSER_MYSQL,"SELECT CAST(CAST(? AS FLOAT(00024)) AS DOUBLE /* target */ PRECISION),CAST(NULL AS FLOAT(53));"},
      {SQLPARSER_MYSQL, "SELECT MOD(?,MOD(NULL,?) DIV ?) % ?,t.div FROM t WHERE 8 DIV 3 MOD 2=0;"},
      {SQLPARSER_SQLITE, "SELECT div,mod,DIV(1),MOD(5,2),5/2,5%2 FROM div;"},
      {SQLPARSER_MYSQL, "SELECT LAG(x,?,?) OVER w FROM t WINDOW w AS(ORDER BY x ROWS BETWEEN ? PRECEDING AND ? FOLLOWING); SELECT FIRST_VALUE(x) OVER(ORDER BY ts RANGE BETWEEN INTERVAL (1+?) DAY PRECEDING AND CURRENT ROW) FROM t;"},
      {SQLPARSER_MYSQL, "SELECT LAG(x,?,?) OVER (last ORDER BY id) AS n,RANK() OVER first AS r FROM t WINDOW last AS (first),first AS (PARTITION BY department) ORDER BY n LIMIT ?;"},
      {SQLPARSER_MYSQL, "SELECT e.n FROM (SELECT 1,2) AS d(x,y),LATERAL (SELECT d.x+d.y,?) e(n,v);"},
      {SQLPARSER_MYSQL, "ALTER TABLE t ADD c BIGINT FIRST; ALTER TABLE t ADD d DOUBLE NULL AFTER `id`;"},
      {SQLPARSER_MYSQL, "CREATE UNIQUE INDEX ix ON db.items (amount DESC, label(10), (score+1) ASC, (ABS(score)) DESC); CREATE INDEX iy ON items (id);"},
      {SQLPARSER_MYSQL, "DROP INDEX ix ON db.items; DROP INDEX `PRIMARY` ON t;"},
      {SQLPARSER_MYSQL, "LOCK TABLES t a READ LOCAL,u WRITE; UNLOCK TABLES; EXPLAIN FORMAT=TREE SELECT SQL_CALC_FOUND_ROWS SUM(x) OVER (PARTITION BY a ORDER BY b) FROM t; CREATE VIEW v AS SELECT 1 WITH LOCAL CHECK OPTION;"},
      {SQLPARSER_MYSQL, "SELECT @a:=@b:=1 XOR 0; INSERT LOW_PRIORITY INTO t VALUES(1); DELETE LOW_PRIORITY a.*,b FROM t a JOIN u b ON a.id=b.id WHERE a.id>0;"},
      {SQLPARSER_MYSQL, "PREPARE stmt FROM 'SELECT ? + ?'; EXECUTE stmt USING @first,@second; DEALLOCATE PREPARE stmt; PREPARE other FROM @sql; DROP PREPARE other;"},
      {SQLPARSER_MYSQL, "CREATE TABLE t(ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6), n INT ZEROFILL); CREATE TABLE u LIKE t; ALTER TABLE u ALTER n SET DEFAULT (1+2); ALTER TABLE u ENGINE=heap; TRUNCATE u;"},
      {SQLPARSER_MYSQL, "SHOW EXTENDED FULL COLUMNS FROM app.t WHERE Field='id'; SHOW KEYS FROM t; SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ ONLY; START TRANSACTION WITH CONSISTENT SNAPSHOT, READ WRITE; COMMIT AND CHAIN NO RELEASE;"},
      {SQLPARSER_MYSQL, "CREATE TABLE t(a BIGINT,b BIGINT,UNIQUE KEY ab(a DESC,b ASC),KEY ba(b,a)); SHOW CREATE TABLE t;"},
      {SQLPARSER_MYSQL, "SELECT d.n FROM t a LEFT JOIN LATERAL (WITH q AS (SELECT a.id+? AS n) SELECT n FROM q UNION ALL SELECT ?) d ON d.n=a.id;"},
      {SQLPARSER_MYSQL, "WITH c(n) AS(SELECT 1 UNION ALL SELECT 2 INTERSECT ALL SELECT 2 EXCEPT DISTINCT SELECT 3) (SELECT n FROM c ORDER BY n LIMIT ?) INTERSECT DISTINCT SELECT ? EXCEPT ALL SELECT ? ORDER BY n LIMIT ?;"},
      {SQLPARSER_MYSQL, "ALTER TABLE child ADD CONSTRAINT fk FOREIGN KEY child_idx(parent_id) REFERENCES parent(id) MATCH SIMPLE ON DELETE CASCADE ON UPDATE RESTRICT;"},
      {SQLPARSER_MYSQL, "INSERT INTO t VALUES(DEFAULT,DEFAULT(a)); REPLACE INTO t SET a=DEFAULT; UPDATE t SET a=DEFAULT(a)+1; SET sql_mode=DEFAULT,@@sql_select_limit=DEFAULT; SET NAMES DEFAULT;"},
      {SQLPARSER_MYSQL, "INSERT INTO t(a,b) VALUES(1,2),(3,4) ON DUPLICATE KEY UPDATE b=b+1,a=VALUES(a); INSERT u SET a=1 ON DUPLICATE KEY UPDATE a=a+1;"},
      {SQLPARSER_SQLITE, "CREATE TABLE t(a CONSTRAINT first CONSTRAINT second CHECK(a>0) CONSTRAINT tail,b,CONSTRAINT uk UNIQUE(a) CHECK(b>0) CONSTRAINT dangling, CHECK(a<20));"},
#if SQLPARSER_SQLITE_ENABLE_UPDATE_DELETE_LIMIT
      {SQLPARSER_SQLITE, "UPDATE t SET a=1 WHERE b>0 ORDER BY a DESC LIMIT 2 OFFSET 1; DELETE FROM t ORDER BY b LIMIT 3,4;"},
#endif
      {SQLPARSER_MYSQL, "WITH RECURSIVE q(n) AS (SELECT 7 UNION ALL SELECT n+1 FROM q WHERE n<9) UPDATE t SET n=10 WHERE n IN (SELECT n FROM q); SAVEPOINT checkpoint; ROLLBACK WORK TO checkpoint; RELEASE SAVEPOINT checkpoint;"},
      {SQLPARSER_MYSQL, "((SELECT a FROM t LIMIT 7) ORDER BY a LIMIT 4) LIMIT 2; INSERT INTO u(a) (SELECT a FROM t LIMIT 1); CREATE TABLE v ENGINE=heap (SELECT a FROM t);"},
      {SQLPARSER_SQLITE, "CREATE TABLE cross(a TEXT,b,PRIMARY KEY(a COLLATE 'nocase' DESC,b),UNIQUE('b' ASC)); SELECT 'alias'.* FROM cross AS 'alias',left ON 'alias'.a=left.a NATURAL INNER JOIN right;"},
      {SQLPARSER_SQLITE, "CREATE TABLE main.t(x DOUBLE PRECISION CONSTRAINT nn NOT NULL REFERENCES p ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED); CREATE VIRTUAL TABLE v USING custom(a,(b,c)); CREATE TRIGGER tr AFTER INSERT ON t BEGIN UPDATE t SET x=NEW.x; SELECT RAISE(ABORT,'bad'); END; SELECT * FROM (t NATURAL LEFT JOIN json_each('[1]')) AS g;"}
    };
    for (size_t sample = 0; sample < sizeof(samples)/sizeof(samples[0]); ++sample) {
      const sqlparser_dialect d = samples[sample].dialect;
      const char *sql = samples[sample].sql;
      sqlparser_document *document = NULL;
      sqlparser_error error;
      fail_at = SIZE_MAX;
      attempts = 0;
      check_equal(sqlparser_parse_dialect(sql, strlen(sql), (sqlparser_dialect)d, NULL, &document, &error), SQLPARSER_OK);
      size_t allocation_count = attempts;
      sqlparser_document_destroy(document);
      check_true(allocation_count > 10u);
      for (size_t i = 1; i <= allocation_count; ++i) {
        info("failure at allocation boundary %zu", i);
        document = NULL; attempts = 0; fail_at = i;
        check_equal(sqlparser_parse_dialect(sql, strlen(sql), (sqlparser_dialect)d, NULL, &document, &error), SQLPARSER_OUT_OF_MEMORY);
        check_null(document);
        check_equal(live_blocks, 0); check_equal(live_strings, 0); check_equal(live_vectors, 0);
        fail_at = SIZE_MAX;
        check_equal(sqlparser_parse_dialect("SELECT 1", 8, (sqlparser_dialect)d, NULL, &document, &error), SQLPARSER_OK);
        sqlparser_document_destroy(document);
        check_equal(live_blocks, 0); check_equal(live_strings, 0); check_equal(live_vectors, 0);
      }
    }
  }

  it("unwinds both allocated and embedded parser stacks on syntax errors") {
    const char *inputs[] = {"SELECT !", "SELECT CASE WHEN x=1 THEN (SELECT x FROM t) ELSE", "INSERT INTO t VALUES(1),(2),(",
      "INSERT INTO t VALUES(DEFAULT); SET @value=DEFAULT", "UPDATE t SET a=DEFAULT(a)+",
      "SHOW KEYS FROM t; COMMIT AND CHAIN RELEASE", "START TRANSACTION READ ONLY, READ WRITE",
      "SELECT 5 DIV", "SELECT MOD(5,", "SELECT CAST(1 AS UNSIGNED INT)",
      "SELECT CAST(1 AS SIGNED INTEGER", "SELECT CAST(1 AS DOUBLE INTEGER)"};
    for (size_t i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
      fail_at = SIZE_MAX;
      sqlparser_document *document = NULL;
      check_equal(sqlparser_parse(inputs[i], strlen(inputs[i]), NULL, &document, NULL), SQLPARSER_SYNTAX_ERROR);
      check_null(document);
      check_equal(live_blocks, 0); check_equal(live_strings, 0); check_equal(live_vectors, 0);
    }
  }

  it("releases each owner on allocation failure with explicit lexical options") {
    const char *sql = "INSERT INTO people (id,note) VALUES (1,'C:\\'); SELECT id FROM people WHERE note='a\\n'";
    const sqlparser_options options = {SQLPARSER_MYSQL, true};
    sqlparser_document *document = NULL;
    fail_at = SIZE_MAX; attempts = 0;
    check_equal(sqlparser_parse_with_options(sql, strlen(sql), &options, NULL, &document, NULL), SQLPARSER_OK);
    const size_t count = attempts;
    sqlparser_document_destroy(document);
    for (size_t i = 1; i <= count; ++i) {
      document = NULL; attempts = 0; fail_at = i;
      check_equal(sqlparser_parse_with_options(sql, strlen(sql), &options, NULL, &document, NULL), SQLPARSER_OUT_OF_MEMORY);
      check_null(document);
      check_equal(live_blocks, 0); check_equal(live_strings, 0); check_equal(live_vectors, 0);
    }
    fail_at = SIZE_MAX;
  }
}
