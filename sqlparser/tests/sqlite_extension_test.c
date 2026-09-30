#include <sqlparser/sqlparser.h>
#include <sqlite3.h>
#include <tinytest.h>
#include <string.h>

static sqlparser_document *document;
static sqlite3 *database;

static void clear_document(void) {
  sqlparser_document_destroy(document);
  document = NULL;
}
static const sqlparser_node *node(sqlparser_id id) {
  const sqlparser_node *n = sqlparser_get_node(document, id);
  check_not_null(n);
  return n;
}
static const sqlparser_node *parse(const char *sql) {
  clear_document();
  sqlparser_error error;
  sqlparser_status status = sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_SQLITE, NULL, &document, &error);
  if (status != SQLPARSER_OK) info("SQL byte %zu: %s; input: %s", error.offset, error.message, sql);
  check_equal(status, SQLPARSER_OK);
  return node(sqlparser_statements(document).first);
}
static void text_is(sqlparser_id id, const char *expected) {
  const sqlparser_span span = node(id)->span;
  check_equal(span.length, strlen(expected));
  check_equal(memcmp(sqlparser_text(document, span), expected, span.length), 0);
}
static void rejected(const char *sql, sqlparser_dialect dialect) {
  clear_document();
  sqlparser_error error;
  info("reject dialect %d: %s", (int)dialect, sql);
  check_equal(sqlparser_parse_dialect(sql, strlen(sql), dialect, NULL, &document, &error), SQLPARSER_SYNTAX_ERROR);
  check_null(document);
  check_true(error.offset <= strlen(sql));
}
static void execute(const char *sql) {
  parse(sql);
  int status = sqlite3_exec(database, sql, NULL, NULL, NULL);
  if (status != SQLITE_OK) info("SQLite: %s; input: %s", sqlite3_errmsg(database), sql);
  check_equal(status, SQLITE_OK);
}

spec("SQLite extended syntax and AST contracts") {
  after_each() {
    clear_document();
    if (database) { check_equal(sqlite3_close(database), SQLITE_OK); database = NULL; }
  }

  it("preserves repeated and dangling constraint declarations with SQLite name scope") {
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    execute("CREATE TABLE historical(a INTEGER CONSTRAINT first_name CONSTRAINT second_name CHECK(a>0) CHECK(a<10) CONSTRAINT tail, b TEXT PRIMARY KEY CONSTRAINT unused, c, CONSTRAINT key1 UNIQUE(a,c) CONSTRAINT key2 CHECK(c>0) UNIQUE(c) CONSTRAINT table_tail, CHECK(a<20))");
    const sqlparser_node *table = node(sqlparser_statements(document).first);
    check_equal(table->as.create_table.elements.count, 8u);
    const sqlparser_node *column = node(table->as.create_table.elements.first);
    check_equal(column->as.column.constraints.count, 3u);
    const sqlparser_node *constraint = node(column->as.column.constraints.first);
    check_equal(constraint->as.constraint.kind, SQLPARSER_CHECK);
    text_is(constraint->as.constraint.name, "second_name");
    check_equal(constraint->as.constraint.declarations.count, 2u);
    text_is(constraint->as.constraint.declarations.first, "first_name");
    text_is(constraint->as.constraint.declarations.last, "second_name");
    check_equal(memcmp(sqlparser_text(document, constraint->span), "CONSTRAINT", strlen("CONSTRAINT")), 0);
    constraint = node(constraint->next);
    text_is(constraint->as.constraint.name, "second_name");
    check_equal(constraint->as.constraint.declarations.count, 0u);
    constraint = node(constraint->next);
    check_equal(constraint->as.constraint.kind, SQLPARSER_CONSTRAINT_DECLARATION);
    text_is(constraint->as.constraint.name, "tail");
    text_is(column->as.column.constraints.last, "CONSTRAINT tail");
    column = node(column->next);
    check_equal(node(column->as.column.constraints.first)->as.constraint.name, SQLPARSER_NONE);
    text_is(node(column->as.column.constraints.last)->as.constraint.name, "unused");
    constraint = node(node(column->next)->next);
    text_is(constraint->as.constraint.name, "key1");
    constraint = node(constraint->next);
    text_is(constraint->as.constraint.name, "key2");
    constraint = node(constraint->next);
    text_is(constraint->as.constraint.name, "key2");
    constraint = node(constraint->next);
    check_equal(constraint->as.constraint.kind, SQLPARSER_CONSTRAINT_DECLARATION);
    text_is(constraint->as.constraint.name, "table_tail");
    check_equal(node(constraint->next)->as.constraint.name, SQLPARSER_NONE);
    execute("CREATE TABLE loose(a CONSTRAINT only_name, CONSTRAINT table_only)");
    execute("CREATE TABLE conflict_check(a, CHECK(a>0) ON CONFLICT IGNORE)");
    execute("CREATE TABLE collation(a CONSTRAINT named COLLATE nocase CHECK(a<>''))");
    table = node(sqlparser_statements(document).first);
    constraint = node(node(table->as.create_table.elements.first)->as.column.constraints.first);
    text_is(constraint->as.constraint.name, "nocase");
    text_is(constraint->as.constraint.declarations.first, "named");
    text_is(node(constraint->next)->as.constraint.name, "named");
    execute("ALTER TABLE loose ADD COLUMN b CONSTRAINT added_tail");
    execute("CREATE TABLE inherited(a CONSTRAINT carried COLLATE nocase, CHECK(a>0) CHECK(a<10), CHECK(a<>5))");
    table = node(sqlparser_statements(document).first);
    constraint = node(node(table->as.create_table.elements.first)->next);
    text_is(constraint->as.constraint.name, "carried");
    text_is(node(constraint->next)->as.constraint.name, "carried");
    check_equal(node(table->as.create_table.elements.last)->as.constraint.name, SQLPARSER_NONE);
    check_equal(sqlite3_exec(database, "INSERT INTO inherited VALUES(-1)", NULL, NULL, NULL), SQLITE_CONSTRAINT);
    check_not_null(strstr(sqlite3_errmsg(database), "carried"));
  }

  it("rejects missing names and table-constraint boundary violations like SQLite") {
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    const char *invalid[] = {
      "CREATE TABLE t(a CONSTRAINT)",
      "CREATE TABLE t(a, CONSTRAINT)",
      "CREATE TABLE t(a, PRIMARY KEY(a), b)",
      "CREATE TABLE t(a, PRIMARY KEY(a) b)",
      "CREATE TABLE t(a, PRIMARY KEY(a),)",
      "CREATE TABLE t(a, CONSTRAINT dangling,)",
      "CREATE TABLE t(CONSTRAINT c CHECK(1))"
    };
    for (size_t i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
      rejected(invalid[i], SQLPARSER_SQLITE);
      check_equal(sqlite3_exec(database, invalid[i], NULL, NULL, NULL), SQLITE_ERROR);
    }
    rejected("CREATE TABLE t(a INT CONSTRAINT tail)", SQLPARSER_MYSQL);
    rejected("CREATE TABLE t(a INT,PRIMARY KEY(a) UNIQUE(a))", SQLPARSER_MYSQL);
  }

  it("accounts for all constraint declaration nodes within the configured budget") {
    const char *sql = "CREATE TABLE t(a CONSTRAINT n1 CONSTRAINT n2 CHECK(a>0) CONSTRAINT tail, CONSTRAINT n3 UNIQUE(a) CHECK(a<10) CONSTRAINT n4)";
    parse(sql);
    const size_t required = sqlparser_node_count(document);
    sqlparser_limits limits = sqlparser_default_limits();
    sqlparser_error error;
    for (size_t budget=1; budget<=required; ++budget) {
      clear_document();
      limits.max_nodes = budget;
      sqlparser_status status = sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_SQLITE, &limits, &document, &error);
      check_equal(status, budget==required ? SQLPARSER_OK : SQLPARSER_LIMIT_EXCEEDED);
      if (status!=SQLPARSER_OK) { check_null(document); check_true(error.offset<=strlen(sql)); }
    }
  }

  it("gates limited SQLite writes and preserves order, count, offset and conflict AST") {
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    execute("CREATE TABLE t(a,b); INSERT INTO t VALUES(1,2),(2,3),(3,4)");
    const char *valid[] = {
      "UPDATE OR IGNORE t NOT INDEXED SET a=a+1 WHERE b>0 ORDER BY b DESC LIMIT 2 OFFSET 1",
      "DELETE FROM t WHERE a>0 ORDER BY b LIMIT 3,4",
      "UPDATE t SET a=1 LIMIT -1",
      "DELETE FROM t LIMIT 0",
      "DELETE FROM t LIMIT (1+1) OFFSET -2",
      "WITH q(x) AS (SELECT 2) DELETE FROM t LIMIT (SELECT x FROM q)"
    };
    for (size_t i=0; i<sizeof(valid)/sizeof(valid[0]); ++i) {
      if (sqlparser_sqlite_update_delete_limit_enabled()) {
        const sqlparser_node *s = parse(valid[i]);
        if (i==0) {
          check_equal(s->as.update.conflict, SQLPARSER_CONFLICT_IGNORE);
          check_true(s->as.update.not_indexed);
          check_equal(s->as.update.order_by.count, 1u);
          check_true(node(s->as.update.order_by.first)->as.order.descending);
          text_is(node(s->as.update.limit)->as.limit.count, "2");
          text_is(node(s->as.update.limit)->as.limit.offset, "1");
        } else if (i==1) {
          check_equal(s->as.delete_stmt.order_by.count, 1u);
          text_is(node(s->as.delete_stmt.limit)->as.limit.count, "4");
          text_is(node(s->as.delete_stmt.limit)->as.limit.offset, "3");
        }
      } else rejected(valid[i], SQLPARSER_SQLITE);
      sqlite3_stmt *statement = NULL;
      int expected = sqlite3_compileoption_used("ENABLE_UPDATE_DELETE_LIMIT") ? SQLITE_OK : SQLITE_ERROR;
      check_equal(sqlite3_prepare_v2(database, valid[i], -1, &statement, NULL), expected);
      check_equal(sqlite3_finalize(statement), SQLITE_OK);
    }
    parse("UPDATE t SET a=1; DELETE FROM t WHERE a=0");
    const char *invalid[] = {
      "UPDATE t SET a=1 ORDER BY b",
      "DELETE FROM t ORDER BY b",
      "DELETE FROM t OFFSET 1",
      "DELETE FROM t LIMIT",
      "DELETE FROM t LIMIT 1,2 OFFSET 3",
      "UPDATE t SET a=1 LIMIT 1 ORDER BY b",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN UPDATE t SET a=1 LIMIT 1; END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN DELETE FROM t ORDER BY b LIMIT 1; END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN DELETE FROM t LIMIT 1; END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN UPDATE t SET a=1 ORDER BY b LIMIT 1; END"
    };
    for (size_t i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
      rejected(invalid[i], SQLPARSER_SQLITE);
      check_equal(sqlite3_exec(database, invalid[i], NULL, NULL, NULL), SQLITE_ERROR);
    }
    parse("CREATE TRIGGER allowed AFTER INSERT ON t BEGIN UPDATE t SET a=(SELECT b FROM t LIMIT 1); DELETE FROM t WHERE a IN(SELECT b FROM t LIMIT 1); END");
  }

  it("preserves opaque virtual-table arguments with nested parentheses and quoted commas") {
    const sqlparser_node *s = parse("CREATE VIRTUAL TABLE IF NOT EXISTS main.v USING custom(a != 'x,y', fn(1,(2,3)), tokenize='porter'); SELECT 1");
    check_equal(s->kind, SQLPARSER_CREATE_VIRTUAL_TABLE);
    check_true(s->as.virtual_table.if_not_exists);
    text_is(s->as.virtual_table.name, "main.v");
    text_is(s->as.virtual_table.module, "custom");
    check_equal(s->as.virtual_table.arguments.count, 3u);
    sqlparser_id argument = s->as.virtual_table.arguments.first;
    check_equal(node(argument)->kind, SQLPARSER_MODULE_ARGUMENT);
    text_is(argument, "a != 'x,y'");
    argument = node(argument)->next; text_is(argument, "fn(1,(2,3))");
    argument = node(argument)->next; text_is(argument, "tokenize='porter'");
    check_equal(sqlparser_statements(document).count, 2u);
    s = parse("CREATE VIRTUAL TABLE v USING custom");
    check_equal(s->as.virtual_table.arguments.count, 0u);
    s = parse("CREATE VIRTUAL TABLE v USING custom()");
    check_equal(s->as.virtual_table.arguments.count, 1u);
    text_is(s->as.virtual_table.arguments.first, "");
    rejected("CREATE VIRTUAL TABLE v USING custom(a,(b)", SQLPARSER_SQLITE);
    rejected("CREATE VIRTUAL TABLE v USING custom(a) nonsense", SQLPARSER_SQLITE);
    rejected("SELECT 1 nonsense nonsense", SQLPARSER_SQLITE);
    rejected("CREATE VIRTUAL TABLE v USING custom(a)", SQLPARSER_MYSQL);
  }

  it("retains trigger metadata and keeps body statements out of the top-level list") {
    const sqlparser_node *s = parse("CREATE TEMP TRIGGER IF NOT EXISTS tr INSTEAD OF UPDATE OF x,y ON main.v FOR EACH ROW WHEN NEW.x>0 BEGIN UPDATE OR IGNORE t SET x=NEW.x; SELECT RAISE(ABORT,'bad '||NEW.y); END; DROP TRIGGER IF EXISTS tr");
    check_equal(s->kind, SQLPARSER_CREATE_TRIGGER);
    check_equal(s->as.trigger.timing, SQLPARSER_TRIGGER_INSTEAD_OF);
    check_equal(s->as.trigger.event, SQLPARSER_TRIGGER_UPDATE);
    check_true(s->as.trigger.temporary); check_true(s->as.trigger.if_not_exists);
    check_equal(s->as.trigger.columns.count, 2u);
    check_equal(s->as.trigger.steps.count, 2u);
    check_equal(node(s->as.trigger.when)->kind, SQLPARSER_BINARY);
    check_equal(node(s->as.trigger.steps.first)->as.update.conflict, SQLPARSER_CONFLICT_IGNORE);
    const sqlparser_node *select = node(s->as.trigger.steps.last);
    const sqlparser_node *raise = node(node(select->as.select.columns.first)->as.projection.expression);
    check_equal(raise->kind, SQLPARSER_RAISE);
    check_equal(raise->as.raise.action, SQLPARSER_CONFLICT_ABORT);
    check_equal(node(raise->as.raise.message)->as.binary.op, SQLPARSER_OP_CONCAT);
    check_equal(sqlparser_statements(document).count, 2u);
    check_equal(node(s->next)->kind, SQLPARSER_DROP_TRIGGER);
    s = parse("CREATE TRIGGER tr DELETE ON t BEGIN SELECT RAISE(IGNORE); END");
    check_equal(s->as.trigger.timing, SQLPARSER_TRIGGER_BEFORE);
    check_equal(s->as.trigger.event, SQLPARSER_TRIGGER_DELETE);
  }

  it("rejects trigger-only syntax violations atomically and matches SQLite rejection") {
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    execute("CREATE TABLE t(x)");
    const char *invalid[] = {
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN INSERT INTO t DEFAULT VALUES; END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN UPDATE main.t SET x=1; END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN DELETE FROM main.t; END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN UPDATE t INDEXED BY ix SET x=1; END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN DELETE FROM t NOT INDEXED; END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN WITH q AS (SELECT 1) INSERT INTO t SELECT * FROM q; END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN CREATE TABLE u(x); END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN SELECT RAISE(REPLACE,'bad'); END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN SELECT 1 END",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN END"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
      rejected(invalid[i], SQLPARSER_SQLITE);
      check_equal(sqlite3_exec(database, invalid[i], NULL, NULL, NULL), SQLITE_ERROR);
    }
    rejected("SELECT 1; CREATE TRIGGER tr AFTER INSERT ON t BEGIN SELECT 1;", SQLPARSER_SQLITE);
    rejected("SELECT ADD", SQLPARSER_SQLITE);
    check_equal(sqlite3_exec(database, "SELECT ADD", NULL, NULL, NULL), SQLITE_ERROR);
    execute("CREATE TABLE \"a.b\"(x); CREATE TRIGGER dotted AFTER INSERT ON t BEGIN INSERT INTO \"a.b\" VALUES(NEW.x); END; INSERT INTO t VALUES(1)");
    execute("CREATE TRIGGER cte_select AFTER INSERT ON t BEGIN WITH q(x) AS (SELECT NEW.x) SELECT x FROM q; INSERT INTO \"a.b\" WITH q(x) AS (SELECT NEW.x) SELECT x FROM q; END; INSERT INTO t VALUES(2)");
  }

  it("counts trigger steps toward the configurable statement budget") {
    const char *sql = "CREATE TRIGGER tr AFTER INSERT ON t BEGIN SELECT 1; SELECT 2; END";
    sqlparser_limits limits = sqlparser_default_limits();
    sqlparser_error error;
    limits.max_statements = 2;
    check_equal(sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_SQLITE, &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
    limits.max_statements = 3;
    check_equal(sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_SQLITE, &limits, &document, &error), SQLPARSER_OK);
    check_equal(sqlparser_statements(document).count, 1u);
  }

  it("retains foreign-key actions deferrability named constraints and multiword types") {
    const sqlparser_node *s = parse("CREATE TABLE child(a DOUBLE PRECISION CONSTRAINT positive CHECK(a>0), b INT REFERENCES parent(id) ON UPDATE CASCADE ON DELETE SET NULL MATCH simple NOT NULL DEFERRABLE INITIALLY DEFERRED, FOREIGN KEY(a,b) REFERENCES parent(x,y) ON DELETE RESTRICT ON UPDATE NO ACTION NOT DEFERRABLE INITIALLY IMMEDIATE)");
    const sqlparser_node *a = node(s->as.create_table.elements.first);
    text_is(node(a->as.column.type)->as.type.name, "DOUBLE PRECISION");
    text_is(node(a->as.column.constraints.first)->as.constraint.name, "positive");
    const sqlparser_node *b = node(a->next);
    const sqlparser_node *reference = node(b->as.column.constraints.first);
    check_equal(reference->as.constraint.reference.on_update, SQLPARSER_REFERENCE_CASCADE);
    check_equal(reference->as.constraint.reference.on_delete, SQLPARSER_REFERENCE_SET_NULL);
    text_is(reference->as.constraint.reference.match, "simple");
    check_equal(node(reference->next)->as.constraint.kind, SQLPARSER_NOT_NULL);
    const sqlparser_node *defer = node(b->as.column.constraints.last);
    check_equal(defer->as.constraint.kind, SQLPARSER_DEFERRABILITY);
    check_true(defer->as.constraint.reference.deferrable);
    check_true(defer->as.constraint.reference.initially_deferred);
    const sqlparser_node *fk = node(s->as.create_table.elements.last);
    check_equal(fk->as.constraint.columns.count, 2u);
    check_equal(fk->as.constraint.referenced_columns.count, 2u);
    check_equal(fk->as.constraint.reference.on_delete, SQLPARSER_REFERENCE_RESTRICT);
    check_equal(fk->as.constraint.reference.on_update, SQLPARSER_REFERENCE_NO_ACTION);
    check_true(fk->as.constraint.reference.has_deferrable);
    check_false(fk->as.constraint.reference.deferrable);
  }

  it("retains query table functions index hints grouped joins and IN table references") {
    const sqlparser_node *s = parse("SELECT 1 IN t, 2 NOT IN fn(3) FROM (t NOT INDEXED NATURAL LEFT OUTER JOIN u INDEXED BY ix) AS g JOIN json_each('[1,2]') AS j");
    const sqlparser_node *in = node(node(s->as.select.columns.first)->as.projection.expression);
    text_is(node(in->as.in.table)->as.table.name, "t");
    in = node(node(s->as.select.columns.last)->as.projection.expression);
    check_true(in->as.in.negated);
    check_true(node(in->as.in.table)->as.table.table_function);
    const sqlparser_node *join = node(s->as.select.from);
    const sqlparser_node *group = node(join->as.join.left);
    text_is(group->as.table.alias, "g");
    const sqlparser_node *natural = node(group->as.table.group);
    check_true(natural->as.join.natural);
    check_equal(natural->as.join.kind, SQLPARSER_JOIN_LEFT);
    check_true(node(natural->as.join.left)->as.table.not_indexed);
    text_is(node(natural->as.join.right)->as.table.indexed_by, "ix");
    check_true(node(join->as.join.right)->as.table.table_function);
    check_equal(node(join->as.join.right)->as.table.arguments.count, 1u);
    s = parse("UPDATE OR IGNORE t INDEXED BY ix SET x=1; DELETE FROM t NOT INDEXED WHERE x=0");
    text_is(s->as.update.indexed_by, "ix");
    check_true(node(s->next)->as.delete_stmt.not_indexed);
  }

  it("wraps EXPLAIN and CTE writes and preserves ALTER and maintenance operands") {
    const sqlparser_node *s = parse("EXPLAIN QUERY PLAN WITH q AS (SELECT 1) DELETE FROM t WHERE x IN q; ALTER TABLE t RENAME COLUMN x TO y; ALTER TABLE t ADD COLUMN z DOUBLE PRECISION; ALTER TABLE t DROP COLUMN z; ALTER TABLE t RENAME TO u; ANALYZE main.t; REINDEX ix; VACUUM main INTO 'backup.db'");
    check_equal(sqlparser_statements(document).count, 8u);
    check_equal(s->kind, SQLPARSER_EXPLAIN); check_true(s->as.explain.query_plan);
    const sqlparser_node *with = node(s->as.explain.statement);
    check_equal(with->kind, SQLPARSER_WITH);
    check_equal(node(with->as.with.body)->kind, SQLPARSER_DELETE);
    s = node(s->next); check_equal(s->as.alter.action, SQLPARSER_RENAME_COLUMN);
    text_is(s->as.alter.column, "x"); text_is(s->as.alter.new_name, "y");
    s = node(s->next); check_equal(s->as.alter.action, SQLPARSER_ADD_COLUMN);
    check_equal(node(s->as.alter.column)->kind, SQLPARSER_COLUMN);
    s = node(s->next); check_equal(s->as.alter.action, SQLPARSER_DROP_COLUMN);
    s = node(s->next); check_equal(s->as.alter.action, SQLPARSER_RENAME_TABLE);
    s = node(s->next); check_equal(s->kind, SQLPARSER_ANALYZE); text_is(s->as.maintenance.target, "main.t");
    s = node(s->next); check_equal(s->kind, SQLPARSER_REINDEX);
    s = node(s->next); check_equal(s->kind, SQLPARSER_VACUUM); text_is(s->as.maintenance.into, "'backup.db'");
    rejected("ALTER TABLE t ADD PRIMARY KEY(x)", SQLPARSER_SQLITE);
    rejected("EXPLAIN EXPLAIN SELECT 1", SQLPARSER_SQLITE);
  }

  it("executes representative extension batches in the installed SQLite engine") {
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    const char *batches[] = {
      "PRAGMA foreign_keys=ON; CREATE TABLE parent(id INTEGER PRIMARY KEY); CREATE TABLE child(id INT REFERENCES parent(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED, value DOUBLE PRECISION CONSTRAINT positive CHECK(value>0));",
      "BEGIN; INSERT INTO child VALUES(1,2.5); INSERT INTO parent VALUES(1); COMMIT; DELETE FROM parent;",
      "CREATE TABLE t(x INT AUTO_INCREMENT); CREATE TABLE audit(x); CREATE TRIGGER log AFTER INSERT ON t BEGIN INSERT INTO audit VALUES(NEW.x); SELECT RAISE(IGNORE); END; INSERT INTO t VALUES(1); DROP TRIGGER log;",
      "WITH q(x) AS (VALUES(2)) INSERT INTO t SELECT x FROM q; WITH q(x) AS (VALUES(2)) UPDATE OR IGNORE t SET x=3 WHERE x IN q; WITH q(x) AS (VALUES(3)) DELETE FROM t WHERE x IN q;",
      "CREATE INDEX ix ON t(x); CREATE INDEX indexed ON t(x); SELECT * FROM (t INDEXED BY ix NATURAL LEFT JOIN audit NOT INDEXED); SELECT * FROM t INDEXED BY indexed; SELECT 1 IN audit; REINDEX ix; ANALYZE t; EXPLAIN SELECT * FROM t; EXPLAIN QUERY PLAN SELECT * FROM t; EXPLAIN UPDATE t INDEXED BY ix SET x=1; EXPLAIN DELETE FROM t NOT INDEXED;",
      "SELECT j.value FROM json_each('[1,2]') AS j; SELECT 1 IN (SELECT value FROM json_each('[1]'));",
      "ALTER TABLE t ADD COLUMN y TEXT DEFAULT 'a'; ALTER TABLE t RENAME COLUMN y TO z; ALTER TABLE t DROP z; ALTER TABLE t RENAME TO renamed; VACUUM;",
      "CREATE TABLE 'quoted table'('quoted column' TEXT); CREATE INDEX 'quoted index' ON 'quoted table'('quoted column'); INSERT INTO 'quoted table' VALUES('x'); DELETE FROM 'quoted table'; DROP TABLE 'quoted table'; ATTACH ':memory:' AS 'aux'; DETACH 'aux';"
    };
    for (size_t i = 0; i < sizeof(batches)/sizeof(batches[0]); ++i) execute(batches[i]);
  }

  it("retains table-key ordering and collation without changing the column-name list") {
    const char *sql = "CREATE TABLE keyed(a TEXT,b INTEGER,CONSTRAINT pk PRIMARY KEY(a COLLATE 'nocase' DESC,b ASC) ON CONFLICT REPLACE,UNIQUE('b' DESC))";
    const sqlparser_node *s = parse(sql);
    const sqlparser_node *a = node(s->as.create_table.elements.first);
    const sqlparser_node *pk = node(node(a->next)->next);
    check_equal(pk->as.constraint.kind, SQLPARSER_PRIMARY_KEY);
    check_equal(pk->as.constraint.conflict, SQLPARSER_CONFLICT_REPLACE);
    check_equal(pk->as.constraint.columns.count, 2u);
    check_equal(pk->as.constraint.key_terms.count, 2u);
    text_is(pk->as.constraint.name, "pk");
    check_equal(node(pk->as.constraint.columns.first)->kind, SQLPARSER_NAME);
    text_is(pk->as.constraint.columns.first, "a");
    text_is(pk->as.constraint.columns.last, "b");
    check_equal(node(pk->as.constraint.columns.first)->next, pk->as.constraint.columns.last);
    const sqlparser_node *first_term = node(pk->as.constraint.key_terms.first);
    check_equal(first_term->kind, SQLPARSER_ORDER);
    check_true(first_term->as.order.descending);
    const sqlparser_node *collate = node(first_term->as.order.expression);
    check_equal(collate->kind, SQLPARSER_COLLATE);
    check_equal(collate->as.collate.expression, pk->as.constraint.columns.first);
    text_is(collate->as.collate.name, "'nocase'");
    check_false(node(pk->as.constraint.key_terms.last)->as.order.descending);
    const sqlparser_node *unique = node(pk->next);
    check_equal(unique->as.constraint.kind, SQLPARSER_UNIQUE);
    check_equal(node(unique->as.constraint.columns.first)->kind, SQLPARSER_NAME);
    text_is(unique->as.constraint.columns.first, "'b'");
    check_true(node(unique->as.constraint.key_terms.first)->as.order.descending);
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    execute(sql);
    s = parse("CREATE TABLE incremented(a INTEGER,PRIMARY KEY(a AUTOINCREMENT))");
    check_true(node(s->as.create_table.elements.last)->as.constraint.autoincrement);
    execute("CREATE TABLE incremented(a INTEGER,PRIMARY KEY(a AUTOINCREMENT)); INSERT INTO incremented DEFAULT VALUES");
    const char *bad = "CREATE TABLE bad_key(a,PRIMARY KEY(a+1))";
    rejected(bad, SQLPARSER_SQLITE);
    check_equal(sqlite3_exec(database, bad, NULL, NULL, NULL), SQLITE_ERROR);
    rejected("CREATE TABLE bad_key(a INT,PRIMARY KEY(a COLLATE 'nocase' DESC))", SQLPARSER_MYSQL);
  }

  it("preserves conditions on comma joins and the type of natural cross joins") {
    const sqlparser_node *s = parse("SELECT * FROM a,b ON a.x=b.x NATURAL CROSS JOIN c");
    const sqlparser_node *natural = node(s->as.select.from);
    check_equal(natural->as.join.kind, SQLPARSER_JOIN_CROSS);
    check_true(natural->as.join.natural);
    const sqlparser_node *comma = node(natural->as.join.left);
    check_equal(comma->as.join.kind, SQLPARSER_JOIN_CROSS);
    check_equal(node(comma->as.join.condition)->as.binary.op, SQLPARSER_OP_EQ);
    s = parse("SELECT * FROM a,b USING(x) NATURAL INNER JOIN c");
    natural = node(s->as.select.from);
    check_equal(natural->as.join.kind, SQLPARSER_JOIN_NATURAL);
    check_true(natural->as.join.natural);
    check_equal(node(natural->as.join.left)->as.join.using_columns.count, 1u);
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    execute("CREATE TABLE a(x); CREATE TABLE b(x); CREATE TABLE c(y); INSERT INTO a VALUES(1); INSERT INTO b VALUES(1); INSERT INTO c VALUES(2); SELECT * FROM a,b ON a.x=b.x NATURAL CROSS JOIN c; SELECT * FROM a,b USING(x) NATURAL INNER JOIN c;");
    const char *invalid[] = {"SELECT * FROM a NATURAL JOIN b ON a.x=b.x", "SELECT * FROM a NATURAL CROSS JOIN b USING(x)", "SELECT * FROM a,b USING(x) ON a.x=b.x"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
      rejected(invalid[i], SQLPARSER_SQLITE);
      check_equal(sqlite3_exec(database, invalid[i], NULL, NULL, NULL), SQLITE_ERROR);
    }
    rejected("SELECT * FROM a,b ON a.x=b.x", SQLPARSER_MYSQL);
  }

  it("distinguishes quoted qualifiers and contextual keyword names from string values") {
    const sqlparser_node *s = parse("SELECT 'alias'.*, 'alias'.x, 'alias'.'x', 'plain', glob('a*','abc') FROM cross AS 'alias'");
    sqlparser_id projection = s->as.select.columns.first;
    check_equal(node(node(projection)->as.projection.expression)->kind, SQLPARSER_STAR);
    projection = node(projection)->next;
    const sqlparser_node *qualified = node(node(projection)->as.projection.expression);
    check_equal(qualified->kind, SQLPARSER_NAME);
    check_equal(qualified->as.name.parts, 2u);
    projection = node(projection)->next;
    check_equal(node(node(projection)->as.projection.expression)->as.name.parts, 2u);
    projection = node(projection)->next;
    check_equal(node(node(projection)->as.projection.expression)->kind, SQLPARSER_STRING);
    check_equal(node(node(s->as.select.columns.last)->as.projection.expression)->kind, SQLPARSER_CALL);
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    execute("CREATE TABLE cross(x); INSERT INTO cross VALUES(1); SELECT 'alias'.*, 'alias'.x, 'alias'.'x', 'plain', glob('a*','abc') FROM cross AS 'alias'; CREATE TABLE left(right,full,inner,natural,outer); SELECT right,full,inner,natural,outer FROM left");
    rejected("SELECT 'alias'.* FROM cross AS 'alias'", SQLPARSER_MYSQL);
  }

  it("retains quoted collation and type names and signed string defaults") {
    const sqlparser_node *s = parse("CREATE TABLE quoted(x 'custom type' COLLATE 'nocase' DEFAULT -'hello',y TEXT DEFAULT +'world')");
    const sqlparser_node *x = node(s->as.create_table.elements.first);
    text_is(node(x->as.column.type)->as.type.name, "'custom type'");
    text_is(node(x->as.column.constraints.first)->as.constraint.name, "'nocase'");
    const sqlparser_node *value = node(node(x->as.column.constraints.last)->as.constraint.expression);
    check_equal(value->as.unary.op, SQLPARSER_OP_NEGATE);
    check_equal(node(value->as.unary.operand)->kind, SQLPARSER_STRING);
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    execute("CREATE TABLE quoted(x 'custom type' COLLATE 'nocase' DEFAULT -'hello',y TEXT DEFAULT +'world'); INSERT INTO quoted DEFAULT VALUES; SELECT x,y COLLATE 'nocase' FROM quoted");
    rejected("CREATE TABLE quoted(x INT DEFAULT -'hello')", SQLPARSER_MYSQL);
  }

  it("keeps every new syntax prefix within its input bounds") {
    const char *inputs[] = {
      "CREATE TABLE t(a CONSTRAINT n1 CONSTRAINT n2 CHECK(a>0) CONSTRAINT tail, CONSTRAINT n3 UNIQUE(a) CHECK(a<10) CONSTRAINT n4);",
      "UPDATE t SET a=1 ORDER BY b DESC LIMIT 2 OFFSET 1; DELETE FROM t LIMIT 3,4;",
      "CREATE VIRTUAL TABLE main.v USING custom(a,(b,c),'x,y');",
      "CREATE TRIGGER tr AFTER INSERT ON t BEGIN UPDATE t SET x=NEW.x; SELECT RAISE(ABORT,'bad'); END;",
      "CREATE TABLE c(x DOUBLE PRECISION CONSTRAINT nn NOT NULL REFERENCES p ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED);",
      "SELECT * FROM (t NATURAL LEFT JOIN json_each('[1]')) AS g;",
      "CREATE TABLE t(a TEXT,b,PRIMARY KEY(a COLLATE 'nocase' DESC,b),UNIQUE('b' ASC));",
      "SELECT 'alias'.* FROM cross AS 'alias',left ON 'alias'.x=left.x NATURAL INNER JOIN right;"
    };
    for (size_t i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
      for (size_t length = 0; length <= strlen(inputs[i]); ++length) {
        clear_document();
        sqlparser_error error;
        sqlparser_status status = sqlparser_parse_dialect(inputs[i], length, SQLPARSER_SQLITE, NULL, &document, &error);
        if (status == SQLPARSER_OK) {
          for (size_t id = 1; id <= sqlparser_node_count(document); ++id) {
            const sqlparser_node *n = node((sqlparser_id)id);
            check_true(n->span.offset <= length);
            check_true(n->span.length <= length - n->span.offset);
          }
        } else {
          check_equal(status, SQLPARSER_SYNTAX_ERROR);
          check_null(document); check_true(error.offset <= length);
        }
      }
    }
  }
}
