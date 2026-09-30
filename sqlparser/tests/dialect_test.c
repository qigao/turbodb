#include <sqlparser/sqlparser.h>
#include <sqlite3.h>
#include <tinytest.h>
#include <stdio.h>
#include <string.h>

static sqlparser_document *document;
static sqlite3 *database;
static sqlite3_stmt *reference_statement;
static const sqlparser_node *node(sqlparser_id id) {
  const sqlparser_node *n = sqlparser_get_node(document, id);
  check_not_null(n);
  return n;
}
static void clear_document(void) {
  sqlparser_document_destroy(document);
  document = NULL;
}
static const sqlparser_node *parse(const char *sql, sqlparser_dialect dialect) {
  clear_document();
  sqlparser_error error;
  sqlparser_status status = sqlparser_parse_dialect(sql, strlen(sql), dialect, NULL, &document, &error);
  if (status != SQLPARSER_OK) info("SQL: %s; byte %zu: %s", sql, error.offset, error.message);
  check_equal(status, SQLPARSER_OK);
  return node(sqlparser_statements(document).first);
}
static const sqlparser_node *expression(const char *sql, sqlparser_dialect dialect) {
  const sqlparser_node *s = parse(sql, dialect);
  return node(node(s->as.select.columns.first)->as.projection.expression);
}
static void rejected(const char *sql, sqlparser_dialect dialect) {
  clear_document();
  sqlparser_error error;
  info("reject dialect %d: %s", (int)dialect, sql);
  check_equal(sqlparser_parse_dialect(sql, strlen(sql), dialect, NULL, &document, &error), SQLPARSER_SYNTAX_ERROR);
  check_null(document);
  check_true(error.offset <= strlen(sql));
}
static void text_is(const sqlparser_node *n, const char *expected) {
  check_equal(n->span.length, strlen(expected));
  check_equal(memcmp(sqlparser_text(document, n->span), expected, n->span.length), 0);
}

spec("explicit MySQL and SQLite dialects") {
  after_each() {
    clear_document();
    if (reference_statement) { check_equal(sqlite3_finalize(reference_statement), SQLITE_OK); reference_statement = NULL; }
    if (database) { check_equal(sqlite3_close(database), SQLITE_OK); database = NULL; }
  }

  it("distinguishes concatenation from logical OR and their precedence") {
    const sqlparser_node *n = expression("SELECT 1+2||3*4", SQLPARSER_MYSQL);
    check_equal(n->as.binary.op, SQLPARSER_OP_OR);
    check_equal(node(n->as.binary.left)->as.binary.op, SQLPARSER_OP_ADD);
    n = expression("SELECT 1+2||3*4", SQLPARSER_SQLITE);
    check_equal(n->as.binary.op, SQLPARSER_OP_ADD);
    const sqlparser_node *multiply = node(n->as.binary.right);
    check_equal(multiply->as.binary.op, SQLPARSER_OP_MULTIPLY);
    check_equal(node(multiply->as.binary.left)->as.binary.op, SQLPARSER_OP_CONCAT);
  }

  it("uses dialect-specific bitwise comparison and BETWEEN precedence") {
    const sqlparser_node *n = expression("SELECT 1|2&3", SQLPARSER_MYSQL);
    check_equal(n->as.binary.op, SQLPARSER_OP_BIT_OR);
    check_equal(node(n->as.binary.right)->as.binary.op, SQLPARSER_OP_BIT_AND);
    n = expression("SELECT 1|2&3", SQLPARSER_SQLITE);
    check_equal(n->as.binary.op, SQLPARSER_OP_BIT_AND);
    check_equal(node(n->as.binary.left)->as.binary.op, SQLPARSER_OP_BIT_OR);
    n = expression("SELECT 1=2<3", SQLPARSER_MYSQL);
    check_equal(n->as.binary.op, SQLPARSER_OP_LT);
    n = expression("SELECT 1=2<3", SQLPARSER_SQLITE);
    check_equal(n->as.binary.op, SQLPARSER_OP_EQ);
    check_equal(node(n->as.binary.right)->as.binary.op, SQLPARSER_OP_LT);
    n = expression("SELECT 1 BETWEEN 2 AND 3 = 4", SQLPARSER_SQLITE);
    check_equal(n->as.binary.op, SQLPARSER_OP_EQ);
    check_equal(node(n->as.binary.left)->kind, SQLPARSER_BETWEEN);
  }

  it("preserves quoted names and dialect-specific backslash handling") {
    check_equal(expression("SELECT \"column\"", SQLPARSER_MYSQL)->kind, SQLPARSER_STRING);
    check_equal(expression("SELECT \"column\"", SQLPARSER_SQLITE)->kind, SQLPARSER_NAME);
    check_equal(expression("SELECT [column]", SQLPARSER_SQLITE)->kind, SQLPARSER_NAME);
    rejected("SELECT [column]", SQLPARSER_MYSQL);
    check_equal(expression("SELECT 'a\\'", SQLPARSER_SQLITE)->kind, SQLPARSER_STRING);
    rejected("SELECT 'a\\'", SQLPARSER_MYSQL);
    check_equal(expression("SELECT 'a\\\'b'", SQLPARSER_MYSQL)->kind, SQLPARSER_STRING);
    rejected("SELECT 'a\\\'b'", SQLPARSER_SQLITE);
  }

  it("keeps parameters variables numeric literals and blobs distinct") {
    check_equal(expression("SELECT @value", SQLPARSER_MYSQL)->kind, SQLPARSER_VARIABLE);
    const sqlparser_node *s = parse("SELECT @value,:value,$ns::value(key),?12,?,0xCA_FE,1_000.2_5e+1,X'00ff'", SQLPARSER_SQLITE);
    check_equal(s->as.select.columns.count, 8u);
    sqlparser_id id = s->as.select.columns.first;
    for (size_t i = 0; i < 5; ++i) {
      check_equal(node(node(id)->as.projection.expression)->kind, SQLPARSER_PARAMETER);
      id = node(id)->next;
    }
    check_equal(node(node(s->as.select.columns.last)->as.projection.expression)->kind, SQLPARSER_BLOB);
    rejected("SELECT X'123'", SQLPARSER_SQLITE);
    rejected("SELECT 123abc", SQLPARSER_SQLITE);
    rejected("SELECT 0xG", SQLPARSER_SQLITE);
    rejected("SELECT 1__2", SQLPARSER_SQLITE);
    rejected("SELECT :value", SQLPARSER_MYSQL);
    rejected("SELECT @@version", SQLPARSER_SQLITE);
  }

  it("supports SQLite comparison forms without leaking them into MySQL") {
    check_equal(expression("SELECT 1==2", SQLPARSER_SQLITE)->as.binary.op, SQLPARSER_OP_EQ);
    check_equal(expression("SELECT a IS b", SQLPARSER_SQLITE)->as.binary.op, SQLPARSER_OP_IS);
    check_equal(expression("SELECT a IS NOT DISTINCT FROM b", SQLPARSER_SQLITE)->as.binary.op, SQLPARSER_OP_IS);
    check_equal(expression("SELECT a IS DISTINCT FROM b", SQLPARSER_SQLITE)->as.binary.op, SQLPARSER_OP_IS_NOT);
    check_equal(expression("SELECT a NOT GLOB 'x*'", SQLPARSER_SQLITE)->as.binary.op, SQLPARSER_OP_NOT_GLOB);
    check_equal(expression("SELECT a NOTNULL", SQLPARSER_SQLITE)->as.unary.op, SQLPARSER_OP_IS_NOT_NULL);
    check_equal(expression("SELECT a IN ()", SQLPARSER_SQLITE)->as.in.items.count, 0u);
    rejected("SELECT 1==2", SQLPARSER_MYSQL);
    rejected("SELECT 1<=>2", SQLPARSER_SQLITE);
    rejected("SELECT 1&&2", SQLPARSER_SQLITE);
  }

  it("preserves digit-leading MySQL names instead of splitting numbers and aliases") {
    const sqlparser_node *s = parse("CREATE TABLE 1ea10 (1a20 INT,1e INT);"
        "INSERT INTO 1ea10 VALUES(1,1);"
        "SELECT 1ea10.1a20,1e+ 1e+10 FROM 1ea10; DROP TABLE 1ea10", SQLPARSER_MYSQL);
    check_equal(sqlparser_statements(document).count, 4u);
    text_is(node(s->as.create_table.table), "1ea10");
    const sqlparser_node *column = node(s->as.create_table.elements.first);
    text_is(node(column->as.column.name), "1a20");
    s = node(s->next);
    text_is(node(s->as.insert.table), "1ea10");
    s = node(s->next);
    const sqlparser_node *p = node(s->as.select.columns.first);
    const sqlparser_node *n = node(p->as.projection.expression);
    check_equal(n->kind, SQLPARSER_NAME);
    check_equal(n->as.name.parts, 2u);
    text_is(n, "1ea10.1a20");
    check_equal(p->as.projection.alias, SQLPARSER_NONE);
    n = node(node(p->next)->as.projection.expression);
    check_equal(n->as.binary.op, SQLPARSER_OP_ADD);
    check_equal(node(n->as.binary.left)->kind, SQLPARSER_NAME);
    text_is(node(n->as.binary.left), "1e");
    check_equal(node(n->as.binary.right)->kind, SQLPARSER_NUMBER);
    text_is(node(n->as.binary.right), "1e+10");
    n = expression("SELECT 123abc", SQLPARSER_MYSQL);
    check_equal(n->kind, SQLPARSER_NAME); text_is(n, "123abc");
    text_is(expression("SELECT 1$col", SQLPARSER_MYSQL), "1$col");
    text_is(expression("SELECT 123_abc", SQLPARSER_MYSQL), "123_abc");
    rejected("CREATE TABLE 123(a INT)", SQLPARSER_MYSQL);
    rejected("CREATE TABLE 1e10(a INT)", SQLPARSER_MYSQL);
    parse("CREATE TABLE `123`(`1e10` INT)", SQLPARSER_MYSQL);
    rejected("SELECT 1ea10.1a20", SQLPARSER_SQLITE);
    rejected("CREATE TABLE 123abc(a INT)", SQLPARSER_SQLITE);
  }

  it("treats MySQL qualified components as names without consuming decimal literals") {
    const sqlparser_node *s = parse("SELECT t.select,t.from,t.1e10,t.0xFF,t.count(1),"
        "`t` . /* gap */ 1a20, .5, 1.5 FROM t; SELECT .25", SQLPARSER_MYSQL);
    check_equal(sqlparser_statements(document).count, 2u);
    sqlparser_id id = s->as.select.columns.first;
    const char *names[] = {"t.select", "t.from", "t.1e10", "t.0xFF"};
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
      const sqlparser_node *n = node(node(id)->as.projection.expression);
      check_equal(n->kind, SQLPARSER_NAME); text_is(n, names[i]);
      id = node(id)->next;
    }
    const sqlparser_node *call = node(node(id)->as.projection.expression);
    check_equal(call->kind, SQLPARSER_CALL);
    text_is(node(call->as.call.name), "t.count");
    id = node(id)->next;
    check_equal(node(node(id)->as.projection.expression)->as.name.parts, 2u);
    id = node(id)->next;
    check_equal(node(node(id)->as.projection.expression)->kind, SQLPARSER_NUMBER);
    text_is(node(node(id)->as.projection.expression), ".5");
    s = node(s->next);
    text_is(node(node(s->as.select.columns.first)->as.projection.expression), ".25");
    rejected("SELECT t..1a20", SQLPARSER_MYSQL);
    rejected("SELECT t.", SQLPARSER_MYSQL);
    rejected("SELECT .123abc", SQLPARSER_SQLITE);
  }

  it("retains MySQL exponent boundaries before an unquoted alias") {
    const sqlparser_node *s = parse("SELECT 1e10tail,1E-2,1e,1ea10,.1e2", SQLPARSER_MYSQL);
    const sqlparser_node *p = node(s->as.select.columns.first);
    check_equal(node(p->as.projection.expression)->kind, SQLPARSER_NUMBER);
    text_is(node(p->as.projection.expression), "1e10");
    text_is(node(p->as.projection.alias), "tail");
    const sqlparser_kind kinds[] = {SQLPARSER_NUMBER, SQLPARSER_NAME, SQLPARSER_NAME, SQLPARSER_NUMBER};
    for (size_t i = 0; i < sizeof(kinds)/sizeof(kinds[0]); ++i) {
      p = node(p->next);
      check_equal(node(p->as.projection.expression)->kind, kinds[i]);
      check_equal(p->as.projection.alias, SQLPARSER_NONE);
    }
    rejected("SELECT 1e+", SQLPARSER_MYSQL);
    rejected("SELECT 1e10tail", SQLPARSER_SQLITE);
  }

  it("distinguishes MySQL binary literals from similarly spelled identifiers") {
    const sqlparser_node *s = parse("SELECT 0xF,0b101,X'00ff',x'',B'101',b''", SQLPARSER_MYSQL);
    const char *raw[] = {"0xF", "0b101", "X'00ff'", "x''", "B'101'", "b''"};
    sqlparser_id id = s->as.select.columns.first;
    for (size_t i = 0; i < sizeof(raw)/sizeof(raw[0]); ++i) {
      const sqlparser_node *n = node(node(id)->as.projection.expression);
      check_equal(n->kind, SQLPARSER_BLOB); text_is(n, raw[i]);
      id = node(id)->next;
    }
    check_equal(id, SQLPARSER_NONE);
    s = parse("SELECT 0xG,0b102,0XFF,0B10,0x,0b", SQLPARSER_MYSQL);
    id = s->as.select.columns.first;
    for (size_t i = 0; i < s->as.select.columns.count; ++i) {
      check_equal(node(node(id)->as.projection.expression)->kind, SQLPARSER_NAME);
      id = node(id)->next;
    }
    const sqlparser_node *n = expression("SELECT 0xFF+1", SQLPARSER_MYSQL);
    check_equal(n->as.binary.op, SQLPARSER_OP_ADD);
    check_equal(node(n->as.binary.left)->kind, SQLPARSER_BLOB);
    const char *invalid[] = {"SELECT X'F'", "SELECT X'GG'", "SELECT B'102'",
      "SELECT X'00", "SELECT B'1", "CREATE TABLE 0xFF(a INT)", "CREATE TABLE 0b10(a INT)"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
      rejected(invalid[i], SQLPARSER_MYSQL);
    check_equal(expression("SELECT 0xFF", SQLPARSER_SQLITE)->kind, SQLPARSER_NUMBER);
    check_equal(expression("SELECT X'00ff'", SQLPARSER_SQLITE)->kind, SQLPARSER_BLOB);
    rejected("SELECT 0b101", SQLPARSER_SQLITE);
  }

  it("bounds MySQL numeric name and binary literal lookahead at every input length") {
    const char *inputs[] = {"SELECT t.1e10,1e+10tail FROM 1ea10",
      "SELECT 0xFF", "SELECT 0b101", "SELECT X'0011'", "SELECT B'101'"};
    for (size_t i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
      for (size_t length = 0; length <= strlen(inputs[i]); ++length) {
        clear_document();
        sqlparser_error error;
        sqlparser_status status = sqlparser_parse_dialect(inputs[i], length,
            SQLPARSER_MYSQL, NULL, &document, &error);
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

  it("distinguishes line comments and rejects unterminated quoted inputs") {
    parse("SELECT 1--x\n;SELECT 2", SQLPARSER_SQLITE);
    check_equal(sqlparser_statements(document).count, 2u);
    parse("SELECT 1 # comment", SQLPARSER_MYSQL);
    rejected("SELECT 1 # comment", SQLPARSER_SQLITE);
    parse("SELECT /*! ordinary SQLite comment */ 1", SQLPARSER_SQLITE);
    parse("SELECT 1 /* comment to EOF", SQLPARSER_SQLITE);
    rejected("SELECT 1 /* comment to EOF", SQLPARSER_MYSQL);
    const char *inputs[] = {"SELECT '", "SELECT \"", "SELECT [", "SELECT `", "SELECT /*", "SELECT X'"};
    for (size_t i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) rejected(inputs[i], SQLPARSER_SQLITE);
  }

  it("shares CAST and quoted aliases while allowing status as a column name") {
    for (int d = SQLPARSER_MYSQL; d <= SQLPARSER_SQLITE; ++d) {
      const sqlparser_node *s = parse("SELECT CAST(status AS CHAR(20)) AS \"label\" FROM t", (sqlparser_dialect)d);
      const sqlparser_node *p = node(s->as.select.columns.first);
      check_equal(node(p->as.projection.expression)->kind, SQLPARSER_CAST);
      check_equal(node(p->as.projection.alias)->kind, SQLPARSER_NAME);
      parse("DELETE FROM t WHERE status=1", (sqlparser_dialect)d);
    }
    const sqlparser_node *s = parse("SELECT key, pragma, immediate, names, global, show, start, rowid FROM t", SQLPARSER_SQLITE);
    check_equal(s->as.select.columns.count, 8u);
  }

  // MySQL 8.4 function-resolution.html and mysql-test/t/parser.test, sql_mode=''.
  it("distinguishes special function tokens from table identifiers") {
    const char *names[] = {
      "BIT_AND", "BIT_OR", "BIT_XOR", "CAST", "COUNT", "CURDATE", "CURTIME",
      "DATE_ADD", "DATE_SUB", "EXTRACT", "GROUP_CONCAT", "MAX", "MID", "MIN",
      "NOW", "POSITION", "STD", "STDDEV", "STDDEV_POP", "STDDEV_SAMP",
      "SUBSTR", "SUBSTRING", "SUM", "SYSDATE", "TRIM", "VARIANCE", "VAR_POP", "VAR_SAMP"
    };
    const char *formats[] = {
      "CREATE TABLE %s (a INT)", "CREATE TABLE %s\t(a INT)",
      "CREATE TABLE %s\n(a INT)", "CREATE TABLE %s/* gap */(a INT)",
      "CREATE TABLE `%s`(a INT)", "CREATE TABLE db.%s(a INT)",
      "CREATE TABLE db. /* gap */ %s(a INT)", "DROP TABLE %s"
    };
    enum { SQL_CAPACITY = 160 };
    char sql[SQL_CAPACITY];
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
      int length = snprintf(sql, sizeof(sql), "CREATE TABLE %s(a INT)", names[i]);
      check_true(length > 0 && (size_t)length < sizeof(sql));
      rejected(sql, SQLPARSER_MYSQL);
      parse(sql, SQLPARSER_SQLITE);
      for (size_t j = 0; j < sizeof(formats)/sizeof(formats[0]); ++j) {
        length = snprintf(sql, sizeof(sql), formats[j], names[i]);
        check_true(length > 0 && (size_t)length < sizeof(sql));
        parse(sql, SQLPARSER_MYSQL);
      }
    }
    rejected("CREATE TABLE cOuNt(a INT)", SQLPARSER_MYSQL);
    parse("CREATE TABLE ADDDATE(a INT); CREATE TABLE SUBDATE(a INT);"
          "CREATE TABLE SESSION_USER(a INT); CREATE TABLE SYSTEM_USER(a INT);"
          "CREATE TABLE ASCII(a INT); CREATE TABLE count_suffix(a INT)", SQLPARSER_MYSQL);
    check_equal(sqlparser_statements(document).count, 6u);
  }

  it("retains function names arguments and aliases in the AST") {
    const sqlparser_node *s = parse("SELECT CoUnT(*) AS n, SUM(DISTINCT amount),"
        "CURDATE(), db.count(1), count FROM t", SQLPARSER_MYSQL);
    check_equal(s->as.select.columns.count, 5u);
    const sqlparser_node *p = node(s->as.select.columns.first);
    const sqlparser_node *call = node(p->as.projection.expression);
    check_equal(call->kind, SQLPARSER_CALL);
    text_is(node(call->as.call.name), "CoUnT");
    check_equal(call->as.call.arguments.count, 1u);
    check_equal(node(call->as.call.arguments.first)->kind, SQLPARSER_STAR);
    text_is(node(p->as.projection.alias), "n");
    p = node(p->next); call = node(p->as.projection.expression);
    check_equal(call->kind, SQLPARSER_CALL);
    check_true(call->as.call.distinct);
    text_is(node(call->as.call.arguments.first), "amount");
    p = node(p->next); call = node(p->as.projection.expression);
    check_equal(call->kind, SQLPARSER_CALL);
    check_equal(call->as.call.arguments.count, 0u);
    p = node(p->next); call = node(p->as.projection.expression);
    text_is(node(call->as.call.name), "db.count");
    p = node(p->next);
    check_equal(node(p->as.projection.expression)->kind, SQLPARSER_NAME);
    text_is(node(p->as.projection.expression), "count");
    check_equal(expression("SELECT COUNT (*)", SQLPARSER_SQLITE)->kind, SQLPARSER_CALL);
  }

  it("recognizes CAST only as a MySQL builtin when its parenthesis is adjacent") {
    check_equal(expression("SELECT CAST(1 AS CHAR)", SQLPARSER_MYSQL)->kind, SQLPARSER_CAST);
    text_is(expression("SELECT CAST", SQLPARSER_MYSQL), "CAST");
    const sqlparser_node *call = expression("SELECT db.CAST(1)", SQLPARSER_MYSQL);
    check_equal(call->kind, SQLPARSER_CALL);
    text_is(node(call->as.call.name), "db.CAST");
    rejected("SELECT CAST (1 AS CHAR)", SQLPARSER_MYSQL);
    check_equal(expression("SELECT CAST (1 AS CHAR)", SQLPARSER_SQLITE)->kind, SQLPARSER_CAST);
  }

  it("models SQL_CALC_FOUND_ROWS without mistaking it for a column name") {
    const sqlparser_node *s = parse("SELECT SQL_CALC_FOUND_ROWS DISTINCT a,b FROM t LIMIT 2", SQLPARSER_MYSQL);
    check_true(s->as.select.calc_found_rows); check_true(s->as.select.distinct);
    check_equal(s->as.select.columns.count, 2u);
    text_is(node(node(s->as.select.columns.first)->as.projection.expression), "a");
    check_equal(node(s->as.select.columns.first)->as.projection.alias, SQLPARSER_NONE);
    s = parse("SELECT DISTINCT SQL_CALC_FOUND_ROWS a FROM t", SQLPARSER_MYSQL);
    check_true(s->as.select.calc_found_rows);
    rejected("SELECT SQL_CALC_FOUND_ROWS", SQLPARSER_MYSQL);
    rejected("SELECT ALL DISTINCT a", SQLPARSER_MYSQL);
    s = parse("SELECT `SQL_CALC_FOUND_ROWS` a FROM t", SQLPARSER_MYSQL);
    check_false(s->as.select.calc_found_rows);
    const sqlparser_node *p = node(s->as.select.columns.first);
    text_is(node(p->as.projection.expression), "`SQL_CALC_FOUND_ROWS`");
    text_is(node(p->as.projection.alias), "a");
    text_is(expression("SELECT t.SQL_CALC_FOUND_ROWS FROM t", SQLPARSER_MYSQL), "t.SQL_CALC_FOUND_ROWS");
    s = parse("SELECT SQL_CALC_FOUND_ROWS a FROM t", SQLPARSER_SQLITE);
    p = node(s->as.select.columns.first);
    text_is(node(p->as.projection.expression), "SQL_CALC_FOUND_ROWS");
    text_is(node(p->as.projection.alias), "a");
  }

  it("retains MySQL CREATE TABLE query columns options and query clauses") {
    const sqlparser_node *s = parse("CREATE TEMPORARY TABLE IF NOT EXISTS db.copy "
        "(id INT PRIMARY KEY) ENGINE=InnoDB AS SELECT id FROM source "
        "UNION ALL SELECT id FROM archive ORDER BY id LIMIT 5", SQLPARSER_MYSQL);
    check_equal(s->kind, SQLPARSER_CREATE_TABLE);
    check_true(s->as.create_table.temporary);
    check_true(s->as.create_table.if_not_exists);
    text_is(node(s->as.create_table.table), "db.copy");
    check_equal(s->as.create_table.elements.count, 1u);
    const sqlparser_node *column = node(s->as.create_table.elements.first);
    text_is(node(column->as.column.name), "id");
    check_equal(node(column->as.column.constraints.first)->as.constraint.kind, SQLPARSER_PRIMARY_KEY);
    check_equal(s->as.create_table.options.count, 1u);
    const sqlparser_node *option = node(s->as.create_table.options.first);
    text_is(node(option->as.assignment.value), "InnoDB");
    const sqlparser_node *q = node(s->as.create_table.query);
    check_equal(q->kind, SQLPARSER_UNION);
    check_true(q->as.compound.all);
    check_equal(q->as.compound.order_by.count, 1u);
    text_is(node(node(q->as.compound.limit)->as.limit.count), "5");
    check_equal(sqlparser_statements(document).count, 1u);
    const char *inputs[] = {
      "CREATE TABLE t SELECT 1", "CREATE TABLE t AS SELECT 1",
      "CREATE TABLE t ENGINE=heap SELECT * FROM source",
      "CREATE TABLE t (extra INT) SELECT x FROM source",
      "CREATE TABLE t AS WITH q AS (SELECT 1) SELECT * FROM q"
    };
    for (size_t i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
      s = parse(inputs[i], SQLPARSER_MYSQL);
      check_not_equal(s->as.create_table.query, SQLPARSER_NONE);
    }
    s = parse("CREATE TABLE t (a INT)", SQLPARSER_MYSQL);
    check_equal(s->as.create_table.query, SQLPARSER_NONE);
    rejected("CREATE TABLE t ENGINE=heap", SQLPARSER_MYSQL);
    rejected("CREATE TABLE t () SELECT 1", SQLPARSER_MYSQL);
    rejected("CREATE TABLE t SELECT 1 ENGINE=heap", SQLPARSER_MYSQL);
    rejected("CREATE TABLE t AS", SQLPARSER_MYSQL);
    rejected("CREATE TABLE t SELECT 1", SQLPARSER_SQLITE);
    rejected("CREATE TABLE t (a INT) AS SELECT 1", SQLPARSER_SQLITE);
    check_equal(parse("CREATE TABLE t AS SELECT 1", SQLPARSER_SQLITE)->kind, SQLPARSER_CREATE_TABLE);
  }

  it("wraps only MySQL explainable queries and writes in EXPLAIN") {
    const struct { const char *sql; sqlparser_kind kind; } inputs[] = {
      {"EXPLAIN SELECT 1", SQLPARSER_SELECT},
      {"DESCRIBE SELECT 1 UNION SELECT 2", SQLPARSER_UNION},
      {"DESC UPDATE t SET a=2 WHERE a=1 LIMIT 1", SQLPARSER_UPDATE},
      {"EXPLAIN DELETE FROM t WHERE a=1", SQLPARSER_DELETE},
      {"EXPLAIN INSERT INTO t VALUES(1)", SQLPARSER_INSERT},
      {"EXPLAIN REPLACE INTO t SELECT a FROM source", SQLPARSER_INSERT},
      {"EXPLAIN WITH q AS (SELECT 1) SELECT * FROM q", SQLPARSER_WITH},
      {"EXPLAIN WITH q AS (SELECT 1) UPDATE t SET a=(SELECT * FROM q)", SQLPARSER_WITH},
      {"EXPLAIN WITH q AS (SELECT 1) DELETE FROM t WHERE a IN (SELECT * FROM q)", SQLPARSER_WITH}
    };
    for (size_t i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
      const sqlparser_node *s = parse(inputs[i].sql, SQLPARSER_MYSQL);
      check_equal(s->kind, SQLPARSER_EXPLAIN);
      check_false(s->as.explain.query_plan);
      check_equal(node(s->as.explain.statement)->kind, inputs[i].kind);
      check_equal(sqlparser_statements(document).count, 1u);
    }
    const sqlparser_node *s = parse("EXPLAIN UPDATE t SET a=a+100 LIMIT 0", SQLPARSER_MYSQL);
    const sqlparser_node *update = node(s->as.explain.statement);
    text_is(node(node(update->as.update.limit)->as.limit.count), "0");
    const sqlparser_node *assignment = node(update->as.update.assignments.first);
    check_equal(node(assignment->as.assignment.value)->as.binary.op, SQLPARSER_OP_ADD);
    const char *invalid[] = {"EXPLAIN", "EXPLAIN EXPLAIN SELECT 1",
      "EXPLAIN CREATE TABLE t(a INT)", "EXPLAIN SET a=1", "EXPLAIN BEGIN",
      "EXPLAIN QUERY PLAN SELECT 1", "EXPLAIN FORMAT=UNKNOWN SELECT 1",
      "EXPLAIN ANALYZE SELECT 1", "EXPLAIN t"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
      rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("DESCRIBE SELECT 1", SQLPARSER_SQLITE);
    rejected("DESC SELECT 1", SQLPARSER_SQLITE);
    check_true(parse("EXPLAIN QUERY PLAN SELECT 1", SQLPARSER_SQLITE)->as.explain.query_plan);
  }

  it("preserves every ORDER BY and LIMIT layer of parenthesized MySQL queries") {
    const char *sql = "((SELECT a FROM t ORDER BY a DESC LIMIT 7 OFFSET 1) "
        "ORDER BY a LIMIT 4 OFFSET 2) LIMIT 3";
    const sqlparser_node *outer = parse(sql, SQLPARSER_MYSQL);
    check_equal(outer->kind, SQLPARSER_QUERY_GROUP);
    text_is(outer, sql);
    check_equal(outer->as.query_group.order_by.count, 0u);
    text_is(node(node(outer->as.query_group.limit)->as.limit.count), "3");
    const sqlparser_node *middle = node(outer->as.query_group.query);
    check_equal(middle->kind, SQLPARSER_QUERY_GROUP);
    check_equal(middle->as.query_group.order_by.count, 1u);
    check_false(node(middle->as.query_group.order_by.first)->as.order.descending);
    const sqlparser_node *limit = node(middle->as.query_group.limit);
    text_is(node(limit->as.limit.count), "4");
    text_is(node(limit->as.limit.offset), "2");
    const sqlparser_node *inner = node(middle->as.query_group.query);
    check_equal(inner->kind, SQLPARSER_SELECT);
    check_true(node(inner->as.select.order_by.first)->as.order.descending);
    limit = node(inner->as.select.limit);
    text_is(node(limit->as.limit.count), "7");
    text_is(node(limit->as.limit.offset), "1");
    outer = parse("((SELECT 1 LIMIT 2))", SQLPARSER_MYSQL);
    check_equal(outer->as.query_group.limit, SQLPARSER_NONE);
    middle = node(outer->as.query_group.query);
    check_equal(middle->as.query_group.limit, SQLPARSER_NONE);
    inner = node(middle->as.query_group.query);
    text_is(node(node(inner->as.select.limit)->as.limit.count), "2");
  }

  it("keeps UNION operand tails separate from its global tail") {
    const sqlparser_node *s = parse("(SELECT a FROM t LIMIT 2) UNION ALL "
        "(SELECT b FROM u ORDER BY b LIMIT 1) ORDER BY a DESC LIMIT 4", SQLPARSER_MYSQL);
    check_equal(s->kind, SQLPARSER_UNION);
    check_true(s->as.compound.all);
    check_true(node(s->as.compound.order_by.first)->as.order.descending);
    text_is(node(node(s->as.compound.limit)->as.limit.count), "4");
    const sqlparser_node *left = node(s->as.compound.left);
    const sqlparser_node *right = node(s->as.compound.right);
    check_equal(left->kind, SQLPARSER_QUERY_GROUP);
    check_equal(right->kind, SQLPARSER_QUERY_GROUP);
    left = node(left->as.query_group.query);
    right = node(right->as.query_group.query);
    text_is(node(node(left->as.select.limit)->as.limit.count), "2");
    text_is(node(node(right->as.select.limit)->as.limit.count), "1");
    check_equal(right->as.select.order_by.count, 1u);
    s = parse("SELECT 1 UNION (SELECT 2 UNION SELECT 3)", SQLPARSER_MYSQL);
    right = node(s->as.compound.right);
    check_equal(right->kind, SQLPARSER_QUERY_GROUP);
    check_equal(node(right->as.query_group.query)->kind, SQLPARSER_UNION);
  }

  it("distinguishes nested query operands from scalar expressions and IN lists") {
    const sqlparser_node *n = expression("SELECT ((SELECT 1))+2", SQLPARSER_MYSQL);
    check_equal(n->as.binary.op, SQLPARSER_OP_ADD);
    const sqlparser_node *scalar = node(n->as.binary.left);
    check_equal(scalar->kind, SQLPARSER_SUBQUERY);
    check_equal(node(scalar->as.subquery.query)->kind, SQLPARSER_QUERY_GROUP);
    n = expression("SELECT (1+2)*3", SQLPARSER_MYSQL);
    check_equal(n->as.binary.op, SQLPARSER_OP_MULTIPLY);
    check_equal(node(n->as.binary.left)->as.binary.op, SQLPARSER_OP_ADD);
    n = expression("SELECT 1 IN ((SELECT a FROM t))", SQLPARSER_MYSQL);
    check_equal(n->kind, SQLPARSER_IN);
    check_equal(n->as.in.items.count, 0u);
    check_equal(node(n->as.in.query)->kind, SQLPARSER_QUERY_GROUP);
    n = expression("SELECT 1 IN ((SELECT a FROM t),2)", SQLPARSER_MYSQL);
    check_equal(n->as.in.query, SQLPARSER_NONE);
    check_equal(n->as.in.items.count, 2u);
    check_equal(node(n->as.in.items.first)->kind, SQLPARSER_SUBQUERY);
    n = expression("SELECT 1 IN ((SELECT 1)+2)", SQLPARSER_MYSQL);
    check_equal(n->as.in.query, SQLPARSER_NONE);
    check_equal(node(n->as.in.items.first)->as.binary.op, SQLPARSER_OP_ADD);
    n = expression("SELECT EXISTS ((SELECT 1 UNION SELECT 2))", SQLPARSER_MYSQL);
    check_equal(n->as.unary.op, SQLPARSER_OP_EXISTS);
    check_equal(node(n->as.unary.operand)->kind, SQLPARSER_QUERY_GROUP);
    n = expression("SELECT ((SELECT 1))+2", SQLPARSER_SQLITE);
    check_equal(node(n->as.binary.left)->kind, SQLPARSER_SUBQUERY);
    check_equal(node(node(n->as.binary.left)->as.subquery.query)->kind, SQLPARSER_SELECT);
  }

  it("accepts parenthesized MySQL queries in CTEs derived tables writes and DDL") {
    const sqlparser_node *s = parse("WITH q AS ((SELECT 1)) (SELECT * FROM q)", SQLPARSER_MYSQL);
    check_equal(node(s->as.with.body)->kind, SQLPARSER_QUERY_GROUP);
    const sqlparser_node *cte = node(s->as.with.bindings.first);
    check_equal(node(cte->as.cte.query)->kind, SQLPARSER_QUERY_GROUP);
    s = parse("SELECT * FROM ((SELECT 1) UNION (SELECT 2)) AS q", SQLPARSER_MYSQL);
    const sqlparser_node *table = node(s->as.select.from);
    text_is(node(table->as.table.alias), "q");
    check_equal(node(table->as.table.query)->kind, SQLPARSER_UNION);
    const char *writes[] = {"INSERT INTO t (SELECT 1)",
      "INSERT INTO t(a) (SELECT 1)", "INSERT INTO t () (SELECT 1)",
      "REPLACE INTO t ((SELECT 1) UNION SELECT 2)"};
    for (size_t i = 0; i < sizeof(writes)/sizeof(writes[0]); ++i) {
      s = parse(writes[i], SQLPARSER_MYSQL);
      check_equal(s->kind, SQLPARSER_INSERT);
      check_equal(node(s->as.insert.query)->kind, SQLPARSER_QUERY_GROUP);
    }
    check_true(s->as.insert.replace);
    const char *creates[] = {"CREATE TABLE t (SELECT 1)",
      "CREATE TABLE t AS (SELECT 1)", "CREATE TABLE t (a INT) (SELECT 1)",
      "CREATE TABLE t ENGINE=heap (SELECT 1)", "CREATE TABLE t (a INT) ENGINE=heap AS (SELECT 1)"};
    for (size_t i = 0; i < sizeof(creates)/sizeof(creates[0]); ++i) {
      s = parse(creates[i], SQLPARSER_MYSQL);
      check_equal(node(s->as.create_table.query)->kind, SQLPARSER_QUERY_GROUP);
    }
    s = parse("EXPLAIN (SELECT 1) UNION SELECT 2", SQLPARSER_MYSQL);
    check_equal(node(s->as.explain.statement)->kind, SQLPARSER_UNION);
  }

  it("rejects malformed query groups and keeps the SQLite query grammar isolated") {
    const char *invalid[] = {"()", "(UPDATE t SET a=1)", "(SELECT 1",
      "(SELECT 1) WHERE a=1", "(SELECT 1) LIMIT 1 LIMIT 2",
      "(SELECT 1) ORDER BY a ORDER BY b", "SELECT 1 LIMIT 1 UNION SELECT 2",
      "(SELECT 1;) UNION SELECT 2", "SELECT 1; (SELECT 2) WHERE a=1"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
      rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("(SELECT 1)", SQLPARSER_SQLITE);
    rejected("SELECT 1 UNION (SELECT 2)", SQLPARSER_SQLITE);
    rejected("INSERT INTO t (SELECT 1)", SQLPARSER_SQLITE);
    const sqlparser_node *s = parse("(SELECT SQL_CALC_FOUND_ROWS a FROM t)", SQLPARSER_MYSQL);
    check_true(node(s->as.query_group.query)->as.select.calc_found_rows);
  }

  it("bounds nested MySQL query groups and preserves batch failure atomicity") {
    const char *sql = "((SELECT 1 LIMIT 7) ORDER BY 1 LIMIT 4) LIMIT 2";
    for (size_t length = 0; length <= strlen(sql); ++length) {
      clear_document();
      sqlparser_error error;
      sqlparser_status status = sqlparser_parse_dialect(sql, length, SQLPARSER_MYSQL,
          NULL, &document, &error);
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
    clear_document();
    sqlparser_limits limits = sqlparser_default_limits();
    sqlparser_error error;
    limits.max_nodes = 4;
    check_equal(sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_MYSQL,
        &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
    limits = sqlparser_default_limits(); limits.max_stack_entries = 8;
    check_equal(sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_MYSQL,
        &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
    limits = sqlparser_default_limits(); limits.max_statements = 1;
    check_equal(sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_MYSQL,
        &limits, &document, &error), SQLPARSER_OK);
    clear_document();
    const char *batch = "(SELECT 1); (SELECT 2)";
    check_equal(sqlparser_parse_dialect(batch, strlen(batch), SQLPARSER_MYSQL,
        &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
  }

  it("requires TABLE and preserves the single MySQL ANALYZE target") {
    const sqlparser_node *s = parse("ANALYZE TABLE db.t", SQLPARSER_MYSQL);
    check_equal(s->kind, SQLPARSER_ANALYZE);
    text_is(node(s->as.maintenance.target), "db.t");
    check_equal(s->as.maintenance.into, SQLPARSER_NONE);
    rejected("ANALYZE", SQLPARSER_MYSQL);
    rejected("ANALYZE t", SQLPARSER_MYSQL);
    rejected("ANALYZE TABLE", SQLPARSER_MYSQL);
    rejected("ANALYZE TABLE t,u", SQLPARSER_MYSQL);
    rejected("ANALYZE LOCAL TABLE t", SQLPARSER_MYSQL);
    rejected("ANALYZE TABLE t UPDATE HISTOGRAM ON a", SQLPARSER_MYSQL);
    rejected("ANALYZE TABLE t", SQLPARSER_SQLITE);
    check_equal(parse("ANALYZE t", SQLPARSER_SQLITE)->kind, SQLPARSER_ANALYZE);
  }

  it("retains basic MySQL view definitions and rejects unmodeled view options") {
    const sqlparser_node *s = parse("CREATE VIEW db.v(x,y) AS "
        "SELECT a,b FROM t WHERE a>0; DROP VIEW IF EXISTS db.v", SQLPARSER_MYSQL);
    check_equal(sqlparser_statements(document).count, 2u);
    check_equal(s->kind, SQLPARSER_CREATE_VIEW);
    check_false(s->as.create_view.temporary);
    check_false(s->as.create_view.if_not_exists);
    text_is(node(s->as.create_view.name), "db.v");
    check_equal(s->as.create_view.columns.count, 2u);
    text_is(node(s->as.create_view.columns.first), "x");
    text_is(node(s->as.create_view.columns.last), "y");
    const sqlparser_node *q = node(s->as.create_view.query);
    check_equal(q->kind, SQLPARSER_SELECT);
    check_equal(q->as.select.columns.count, 2u);
    check_equal(node(q->as.select.where)->as.binary.op, SQLPARSER_OP_GT);
    s = node(s->next);
    check_equal(s->kind, SQLPARSER_DROP_VIEW);
    check_true(s->as.drop_object.if_exists);
    text_is(node(s->as.drop_object.name), "db.v");
    s = parse("CREATE VIEW v AS WITH q AS (SELECT 1) SELECT * FROM q", SQLPARSER_MYSQL);
    check_equal(node(s->as.create_view.query)->kind, SQLPARSER_WITH);
    parse("SELECT view FROM view; SELECT t.explain,t.analyze FROM t", SQLPARSER_MYSQL);
    const char *invalid[] = {"CREATE TEMPORARY VIEW v AS SELECT 1",
      "CREATE VIEW IF NOT EXISTS v AS SELECT 1", "CREATE VIEW v() AS SELECT 1",
      "CREATE VIEW v(t.x) AS SELECT 1", "CREATE VIEW v AS SELECT 1 WITH CHECK",
      "CREATE OR REPLACE VIEW v AS SELECT 1", "DROP VIEW v,w"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
      rejected(invalid[i], SQLPARSER_MYSQL);
    check_equal(parse("CREATE TEMP VIEW IF NOT EXISTS v AS SELECT 1", SQLPARSER_SQLITE)->kind,
        SQLPARSER_CREATE_VIEW);
  }

  it("preserves MySQL DEFAULT atoms in INSERT and REPLACE rows") {
    const sqlparser_node *s = parse("INSERT INTO t(a,b,c) VALUES(DEFAULT,1,'DEFAULT'),(NULL,DEFAULT,2); REPLACE INTO t VALUES(63,default)", SQLPARSER_MYSQL);
    check_equal(s->as.insert.rows.count, 2u);
    const sqlparser_node *row = node(s->as.insert.rows.first);
    check_equal(row->as.row.values.count, 3u);
    const sqlparser_node *value = node(row->as.row.values.first);
    check_equal(value->kind, SQLPARSER_DEFAULT_VALUE);
    text_is(value, "DEFAULT");
    check_equal(node(value->next)->kind, SQLPARSER_NUMBER);
    check_equal(node(row->as.row.values.last)->kind, SQLPARSER_STRING);
    row = node(row->next);
    value = node(row->as.row.values.first);
    check_equal(value->kind, SQLPARSER_NULL);
    check_equal(node(value->next)->kind, SQLPARSER_DEFAULT_VALUE);
    s = node(s->next);
    check_true(s->as.insert.replace);
    value = node(node(s->as.insert.rows.first)->as.row.values.last);
    check_equal(value->kind, SQLPARSER_DEFAULT_VALUE);
    text_is(value, "default");
    s = parse("INSERT INTO t() VALUES()", SQLPARSER_MYSQL);
    check_equal(node(s->as.insert.rows.first)->as.row.values.count, 0u);
  }

  it("distinguishes MySQL DEFAULT assignment values from DEFAULT column calls") {
    const sqlparser_node *s = parse("INSERT INTO t SET a=DEFAULT,b=DEFAULT(t.b)+1; REPLACE INTO t SET a=DEFAULT; UPDATE t SET a=DEFAULT,b=DEFAULT(b)+1 WHERE a>0", SQLPARSER_MYSQL);
    const sqlparser_node *assignment = node(s->as.insert.assignments.first);
    check_equal(node(assignment->as.assignment.value)->kind, SQLPARSER_DEFAULT_VALUE);
    const sqlparser_node *sum = node(node(assignment->next)->as.assignment.value);
    check_equal(sum->as.binary.op, SQLPARSER_OP_ADD);
    const sqlparser_node *call = node(sum->as.binary.left);
    check_equal(call->kind, SQLPARSER_CALL);
    text_is(node(call->as.call.name), "DEFAULT");
    check_equal(call->as.call.arguments.count, 1u);
    check_false(call->as.call.distinct);
    check_equal(node(call->as.call.arguments.first)->as.name.parts, 2u);
    text_is(call, "DEFAULT(t.b)");
    s = node(s->next);
    check_true(s->as.insert.replace);
    check_equal(node(node(s->as.insert.assignments.first)->as.assignment.value)->kind, SQLPARSER_DEFAULT_VALUE);
    s = node(s->next);
    check_equal(s->kind, SQLPARSER_UPDATE);
    check_equal(node(node(s->as.update.assignments.first)->as.assignment.value)->kind, SQLPARSER_DEFAULT_VALUE);
    check_equal(node(s->as.update.where)->kind, SQLPARSER_BINARY);
    call = expression("SELECT DEFAULT /* comment */ (db.t.`value`)", SQLPARSER_MYSQL);
    check_equal(call->kind, SQLPARSER_CALL);
    check_equal(node(call->as.call.arguments.first)->as.name.parts, 3u);
    parse("SELECT a FROM t WHERE a=DEFAULT(a); SET @value=DEFAULT(t.a)", SQLPARSER_MYSQL);
  }

  it("retains DEFAULT and scopes for MySQL system variables and character sets") {
    const struct { const char *sql; sqlparser_scope scope; sqlparser_kind name_kind; } samples[] = {
      {"SET sql_mode=DEFAULT", SQLPARSER_SCOPE_DEFAULT, SQLPARSER_NAME},
      {"SET SESSION sql_mode=DEFAULT", SQLPARSER_SCOPE_SESSION, SQLPARSER_NAME},
      {"SET GLOBAL sql_mode=DEFAULT", SQLPARSER_SCOPE_GLOBAL, SQLPARSER_NAME},
      {"SET LOCAL sql_mode=DEFAULT", SQLPARSER_SCOPE_LOCAL, SQLPARSER_NAME},
      {"SET @@sql_select_limit=DEFAULT", SQLPARSER_SCOPE_DEFAULT, SQLPARSER_VARIABLE},
      {"SET @@global.sql_mode=DEFAULT", SQLPARSER_SCOPE_GLOBAL, SQLPARSER_NAME},
      {"SET @@session.sql_mode=DEFAULT", SQLPARSER_SCOPE_SESSION, SQLPARSER_NAME},
      {"SET @@local.sql_mode=DEFAULT", SQLPARSER_SCOPE_LOCAL, SQLPARSER_NAME}
    };
    for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) {
      const sqlparser_node *s = parse(samples[i].sql, SQLPARSER_MYSQL);
      check_equal(s->as.set.kind, SQLPARSER_SET_ASSIGNMENTS);
      const sqlparser_node *assignment = node(s->as.set.assignments.first);
      check_equal(assignment->as.assignment.scope, samples[i].scope);
      check_equal(node(assignment->as.assignment.name)->kind, samples[i].name_kind);
      check_equal(node(assignment->as.assignment.value)->kind, SQLPARSER_DEFAULT_VALUE);
    }
    const sqlparser_node *s = parse("SET NAMES DEFAULT; SET CHARACTER SET DEFAULT; SET NAMES 'DEFAULT'", SQLPARSER_MYSQL);
    check_equal(s->as.set.kind, SQLPARSER_SET_NAMES);
    check_equal(node(s->as.set.value)->kind, SQLPARSER_DEFAULT_VALUE);
    s = node(s->next);
    check_equal(s->as.set.kind, SQLPARSER_SET_CHARACTER_SET);
    check_equal(node(s->as.set.value)->kind, SQLPARSER_DEFAULT_VALUE);
    s = node(s->next);
    check_equal(node(s->as.set.value)->kind, SQLPARSER_STRING);
    s = parse("SET sql_mode=DEFAULT,@@sql_select_limit=DEFAULT,@value=1", SQLPARSER_MYSQL);
    check_equal(s->as.set.assignments.count, 3u);
  }

  it("rejects DEFAULT expression operands and invalid DEFAULT column arguments") {
    const char *invalid[] = {
      "SELECT DEFAULT", "SELECT DEFAULT+1", "SELECT f(DEFAULT)",
      "SELECT 1 IN (DEFAULT)", "SELECT DEFAULT()", "SELECT DEFAULT(*)",
      "SELECT DEFAULT(1)", "SELECT DEFAULT(a,b)", "SELECT DEFAULT(a+1)",
      "SELECT DEFAULT((a))", "SELECT DEFAULT(DISTINCT a)", "SELECT DEFAULT('a')",
      "INSERT INTO t VALUES(DEFAULT+1)", "INSERT INTO t VALUES((DEFAULT))",
      "UPDATE t SET a=1+DEFAULT", "UPDATE t SET a=DEFAULT WHERE DEFAULT",
      "SET sql_mode=DEFAULT+1", "SET @@sql_mode=(DEFAULT)",
      "SET NAMES DEFAULT COLLATE utf8mb4_bin", "SET CHARACTER SET DEFAULT+1",
      "SET @value=DEFAULT", "SET @value.name=DEFAULT"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
      rejected(invalid[i], SQLPARSER_MYSQL);
    const char *sql = "SELECT 1; SET @value=DEFAULT; SELECT 2";
    clear_document();
    sqlparser_error error;
    check_equal(sqlparser_parse(sql, strlen(sql), NULL, &document, &error), SQLPARSER_SYNTAX_ERROR);
    check_null(document);
    check_equal(error.offset, (size_t)(strstr(sql, "DEFAULT") - sql));
    check_not_null(strstr(error.message, "user variable"));
  }

  it("keeps MySQL DEFAULT write syntax out of SQLite without changing DDL defaults") {
    const char *invalid[] = {"INSERT INTO t VALUES(DEFAULT)", "UPDATE t SET a=DEFAULT",
      "SELECT DEFAULT(a)", "SET sql_mode=DEFAULT", "SET NAMES DEFAULT"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
      rejected(invalid[i], SQLPARSER_SQLITE);
    const sqlparser_node *s = parse("INSERT INTO t DEFAULT VALUES", SQLPARSER_SQLITE);
    check_true(s->as.insert.default_values);
    for (int d = SQLPARSER_MYSQL; d <= SQLPARSER_SQLITE; ++d) {
      s = parse("CREATE TABLE t(a INT DEFAULT 7)", (sqlparser_dialect)d);
      const sqlparser_node *constraint = node(node(s->as.create_table.elements.first)->as.column.constraints.first);
      check_equal(constraint->as.constraint.kind, SQLPARSER_DEFAULT);
      check_equal(node(constraint->as.constraint.expression)->kind, SQLPARSER_NUMBER);
      check_equal(expression("SELECT `DEFAULT` FROM t", (sqlparser_dialect)d)->kind, SQLPARSER_NAME);
    }
  }

  it("bounds DEFAULT nodes and unwinds partial write batches") {
    const char *sql = "INSERT INTO t VALUES(DEFAULT,DEFAULT(a)); UPDATE t SET a=DEFAULT; SET @@sql_mode=DEFAULT";
    for (size_t length = 0; length <= strlen(sql); ++length) {
      clear_document();
      sqlparser_error error;
      sqlparser_status status = sqlparser_parse(sql, length, NULL, &document, &error);
      if (status == SQLPARSER_OK) {
        for (size_t id = 1; id <= sqlparser_node_count(document); ++id) {
          const sqlparser_node *n = node((sqlparser_id)id);
          check_true(n->span.offset <= length);
          check_true(n->span.length <= length - n->span.offset);
        }
      } else {
        check_equal(status, SQLPARSER_SYNTAX_ERROR);
        check_null(document);
        check_true(error.offset <= length);
      }
    }
    clear_document();
    sqlparser_limits limits = sqlparser_default_limits();
    limits.max_nodes = 2;
    sqlparser_error error;
    check_equal(sqlparser_parse(sql, strlen(sql), &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
    parse(sql, SQLPARSER_MYSQL);
    check_equal(sqlparser_statements(document).count, 3u);
  }

  it("retains database and filters for MySQL SHOW TRIGGERS EVENTS and OPEN TABLES") {
    const struct { const char *sql; sqlparser_show_kind kind; } samples[] = {
      {"SHOW TRIGGERS FROM app LIKE 'audit%'", SQLPARSER_SHOW_TRIGGERS},
      {"SHOW EVENTS IN app LIKE 'audit%'", SQLPARSER_SHOW_EVENTS},
      {"SHOW OPEN TABLES FROM app LIKE 'audit%'", SQLPARSER_SHOW_OPEN_TABLES}
    };
    for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) {
      const sqlparser_node *s = parse(samples[i].sql, SQLPARSER_MYSQL);
      check_equal(s->kind, SQLPARSER_SHOW);
      check_equal(s->as.show.kind, samples[i].kind);
      text_is(node(s->as.show.database), "app");
      text_is(node(s->as.show.pattern), "'audit%'");
      check_equal(s->as.show.where, SQLPARSER_NONE);
      check_equal(s->as.show.table, SQLPARSER_NONE);
      text_is(s, samples[i].sql);
      rejected(samples[i].sql, SQLPARSER_SQLITE);
    }
    const sqlparser_node *s = parse("SHOW EVENTS WHERE Db='app'", SQLPARSER_MYSQL);
    check_equal(s->as.show.database, SQLPARSER_NONE);
    check_equal(s->as.show.pattern, SQLPARSER_NONE);
    check_equal(node(s->as.show.where)->as.binary.op, SQLPARSER_OP_EQ);
  }

  it("retains SHOW COLUMNS and INDEX aliases modifiers and separate table/database names") {
    const sqlparser_node *s = parse("SHOW EXTENDED FULL FIELDS IN t FROM app LIKE 'a%'; SHOW EXTENDED KEYS FROM app.t WHERE Key_name='PRIMARY'; SHOW CREATE TABLE app.t", SQLPARSER_MYSQL);
    check_equal(s->as.show.kind, SQLPARSER_SHOW_COLUMNS);
    check_true(s->as.show.full); check_true(s->as.show.extended);
    text_is(node(s->as.show.table), "t");
    text_is(node(s->as.show.database), "app");
    text_is(node(s->as.show.pattern), "'a%'");
    s = node(s->next);
    check_equal(s->as.show.kind, SQLPARSER_SHOW_INDEX);
    check_false(s->as.show.full); check_true(s->as.show.extended);
    text_is(node(s->as.show.table), "app.t");
    check_equal(s->as.show.database, SQLPARSER_NONE);
    check_equal(node(s->as.show.where)->as.binary.op, SQLPARSER_OP_EQ);
    s = node(s->next);
    check_equal(s->as.show.kind, SQLPARSER_SHOW_CREATE_TABLE);
    text_is(node(s->as.show.table), "app.t");
    check_equal(s->as.show.where, SQLPARSER_NONE);
    const char *aliases[] = {"SHOW COLUMNS FROM t", "SHOW FULL COLUMNS FROM t",
      "SHOW EXTENDED COLUMNS FROM t", "SHOW FIELDS IN t", "SHOW INDEX FROM t",
      "SHOW INDEXES IN t IN app", "SHOW KEYS FROM t FROM app"};
    for (size_t i = 0; i < sizeof(aliases)/sizeof(aliases[0]); ++i)
      parse(aliases[i], SQLPARSER_MYSQL);
  }

  it("keeps MySQL SHOW STATUS scopes and routine or charset filters distinct") {
    const struct { const char *sql; sqlparser_show_kind kind; sqlparser_scope scope; } samples[] = {
      {"SHOW STATUS WHERE 0", SQLPARSER_SHOW_STATUS, SQLPARSER_SCOPE_DEFAULT},
      {"SHOW GLOBAL STATUS WHERE 0", SQLPARSER_SHOW_STATUS, SQLPARSER_SCOPE_GLOBAL},
      {"SHOW SESSION STATUS WHERE 0", SQLPARSER_SHOW_STATUS, SQLPARSER_SCOPE_SESSION},
      {"SHOW LOCAL STATUS WHERE 0", SQLPARSER_SHOW_STATUS, SQLPARSER_SCOPE_LOCAL},
      {"SHOW CHARACTER SET WHERE 0", SQLPARSER_SHOW_CHARACTER_SET, SQLPARSER_SCOPE_DEFAULT},
      {"SHOW CHARSET WHERE 0", SQLPARSER_SHOW_CHARACTER_SET, SQLPARSER_SCOPE_DEFAULT},
      {"SHOW PROCEDURE STATUS WHERE 0", SQLPARSER_SHOW_PROCEDURE_STATUS, SQLPARSER_SCOPE_DEFAULT},
      {"SHOW FUNCTION STATUS WHERE 0", SQLPARSER_SHOW_FUNCTION_STATUS, SQLPARSER_SCOPE_DEFAULT}
    };
    for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) {
      const sqlparser_node *s = parse(samples[i].sql, SQLPARSER_MYSQL);
      check_equal(s->as.show.kind, samples[i].kind);
      check_equal(s->as.show.scope, samples[i].scope);
      text_is(node(s->as.show.where), "0");
      check_equal(s->as.show.pattern, SQLPARSER_NONE);
    }
    const sqlparser_node *s = parse("SHOW FUNCTION STATUS LIKE 'f%'", SQLPARSER_MYSQL);
    text_is(node(s->as.show.pattern), "'f%'");
    check_equal(s->as.show.where, SQLPARSER_NONE);
  }

  it("rejects misplaced SHOW modifiers filters and missing targets") {
    const char *invalid[] = {"SHOW FULL EVENTS", "SHOW EXTENDED TABLES", "SHOW FULL EXTENDED COLUMNS FROM t",
      "SHOW COLUMNS", "SHOW COLUMNS t", "SHOW INDEX FROM t LIKE 'a%'", "SHOW FULL KEYS FROM t",
      "SHOW GLOBAL EVENTS", "SHOW STATUS FROM app", "SHOW PROCEDURE STATUS FROM app",
      "SHOW EVENTS LIKE 'a%' WHERE 1", "SHOW CREATE TABLE", "SHOW CREATE TABLE t WHERE 1",
      "SHOW CREATE TABLE t FROM app", "SHOW CHARACTER SET FROM app", "SHOW OPEN TABLES FULL"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("SHOW COLUMNS FROM t", SQLPARSER_SQLITE);
    rejected("SHOW CREATE TABLE t", SQLPARSER_SQLITE);
    rejected("SHOW STATUS", SQLPARSER_SQLITE);
  }

  it("preserves unspecified yes and no transaction completion options") {
    const struct { const char *sql; sqlparser_choice chain, release; } samples[] = {
      {"COMMIT", SQLPARSER_CHOICE_UNSPECIFIED, SQLPARSER_CHOICE_UNSPECIFIED},
      {"COMMIT WORK AND NO CHAIN", SQLPARSER_CHOICE_NO, SQLPARSER_CHOICE_UNSPECIFIED},
      {"ROLLBACK AND CHAIN", SQLPARSER_CHOICE_YES, SQLPARSER_CHOICE_UNSPECIFIED},
      {"COMMIT AND CHAIN NO RELEASE", SQLPARSER_CHOICE_YES, SQLPARSER_CHOICE_NO},
      {"ROLLBACK WORK AND NO CHAIN RELEASE", SQLPARSER_CHOICE_NO, SQLPARSER_CHOICE_YES},
      {"COMMIT RELEASE", SQLPARSER_CHOICE_UNSPECIFIED, SQLPARSER_CHOICE_YES},
      {"ROLLBACK NO RELEASE", SQLPARSER_CHOICE_UNSPECIFIED, SQLPARSER_CHOICE_NO}
    };
    for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) {
      const sqlparser_node *s = parse(samples[i].sql, SQLPARSER_MYSQL);
      check_equal(s->kind, SQLPARSER_TRANSACTION);
      check_equal(s->as.transaction.kind, samples[i].sql[0]=='C' ? SQLPARSER_COMMIT : SQLPARSER_ROLLBACK);
      check_equal(s->as.transaction.chain, samples[i].chain);
      check_equal(s->as.transaction.release, samples[i].release);
      text_is(s, samples[i].sql);
    }
    const sqlparser_node *s = parse("ROLLBACK WORK TO SAVEPOINT checkpoint", SQLPARSER_MYSQL);
    check_equal(s->as.transaction.kind, SQLPARSER_ROLLBACK_TO);
    text_is(node(s->as.transaction.name), "checkpoint");
    const char *invalid[] = {"COMMIT AND CHAIN RELEASE", "ROLLBACK AND CHAIN RELEASE",
      "COMMIT RELEASE AND NO CHAIN", "COMMIT NO CHAIN", "COMMIT AND CHAIN AND CHAIN",
      "BEGIN AND CHAIN", "ROLLBACK TO s AND CHAIN"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("COMMIT AND NO CHAIN", SQLPARSER_SQLITE);
    rejected("ROLLBACK RELEASE", SQLPARSER_SQLITE);
  }

  it("preserves MySQL transaction access modes and consistent snapshots") {
    const sqlparser_node *s = parse("START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY", SQLPARSER_MYSQL);
    check_equal(s->as.transaction.kind, SQLPARSER_START_TRANSACTION);
    check_equal(s->as.transaction.access, SQLPARSER_READ_ONLY);
    check_true(s->as.transaction.consistent_snapshot);
    s = parse("START TRANSACTION READ WRITE, WITH CONSISTENT SNAPSHOT", SQLPARSER_MYSQL);
    check_equal(s->as.transaction.access, SQLPARSER_READ_WRITE);
    check_true(s->as.transaction.consistent_snapshot);
    s = parse("START TRANSACTION", SQLPARSER_MYSQL);
    check_equal(s->as.transaction.access, SQLPARSER_ACCESS_DEFAULT);
    check_false(s->as.transaction.consistent_snapshot);
    const char *invalid[] = {"START TRANSACTION READ ONLY, READ WRITE", "START TRANSACTION READ WRITE, READ ONLY",
      "START TRANSACTION READ", "START TRANSACTION READ ONLY,", "START TRANSACTION WITH SNAPSHOT",
      "START TRANSACTION ISOLATION LEVEL SERIALIZABLE", "BEGIN READ ONLY"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("START TRANSACTION READ ONLY", SQLPARSER_SQLITE);
  }

  it("retains SET TRANSACTION scopes isolation levels and access modes") {
    const struct { const char *sql; sqlparser_scope scope; sqlparser_isolation isolation; sqlparser_transaction_access access; } samples[] = {
      {"SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED", SQLPARSER_SCOPE_DEFAULT, SQLPARSER_READ_UNCOMMITTED, SQLPARSER_ACCESS_DEFAULT},
      {"SET GLOBAL TRANSACTION ISOLATION LEVEL READ COMMITTED", SQLPARSER_SCOPE_GLOBAL, SQLPARSER_READ_COMMITTED, SQLPARSER_ACCESS_DEFAULT},
      {"SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY", SQLPARSER_SCOPE_SESSION, SQLPARSER_REPEATABLE_READ, SQLPARSER_READ_ONLY},
      {"SET TRANSACTION READ WRITE, ISOLATION LEVEL SERIALIZABLE", SQLPARSER_SCOPE_DEFAULT, SQLPARSER_SERIALIZABLE, SQLPARSER_READ_WRITE},
      {"SET LOCAL TRANSACTION READ ONLY", SQLPARSER_SCOPE_LOCAL, SQLPARSER_ISOLATION_DEFAULT, SQLPARSER_READ_ONLY}
    };
    for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) {
      const sqlparser_node *s = parse(samples[i].sql, SQLPARSER_MYSQL);
      check_equal(s->kind, SQLPARSER_TRANSACTION);
      check_equal(s->as.transaction.kind, SQLPARSER_SET_TRANSACTION);
      check_equal(s->as.transaction.scope, samples[i].scope);
      check_equal(s->as.transaction.isolation, samples[i].isolation);
      check_equal(s->as.transaction.access, samples[i].access);
      text_is(s, samples[i].sql);
    }
    const char *invalid[] = {"SET TRANSACTION", "SET TRANSACTION ISOLATION LEVEL",
      "SET TRANSACTION READ ONLY, READ ONLY", "SET TRANSACTION READ ONLY, READ WRITE",
      "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE, ISOLATION LEVEL SERIALIZABLE",
      "SET TRANSACTION WITH CONSISTENT SNAPSHOT", "SET TRANSACTION ISOLATION LEVEL READ ONLY"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("SET TRANSACTION READ ONLY", SQLPARSER_SQLITE);
  }

  it("keeps new nonreserved MySQL keywords usable as identifiers and qualifiers") {
    const char *sql = "SELECT triggers,events,open,columns,fields,indexes,function,extended,chain,no,only,consistent,snapshot,isolation,level,repeatable,committed,uncommitted,serializable FROM events";
    for (int d = SQLPARSER_MYSQL; d <= SQLPARSER_SQLITE; ++d)
      check_equal(parse(sql, (sqlparser_dialect)d)->as.select.columns.count, 19u);
    const sqlparser_node *n = expression("SELECT events.1e10 FROM events", SQLPARSER_MYSQL);
    check_equal(n->kind, SQLPARSER_NAME);
    check_equal(n->as.name.parts, 2u);
    text_is(n, "events.1e10");
    parse("SELECT t.read,t.write,t.keys,t.procedure FROM t", SQLPARSER_MYSQL);
    parse("SELECT read,write,keys,procedure FROM t", SQLPARSER_SQLITE);
  }

  it("bounds SHOW and transaction batches and discards partial results on error") {
    const char *sql = "SHOW EXTENDED FULL COLUMNS FROM app.t WHERE Field='id'; SET SESSION TRANSACTION READ ONLY, ISOLATION LEVEL SERIALIZABLE; COMMIT AND NO CHAIN NO RELEASE";
    for (size_t length = 0; length <= strlen(sql); ++length) {
      clear_document();
      sqlparser_error error;
      sqlparser_status status = sqlparser_parse(sql, length, NULL, &document, &error);
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
    rejected("SHOW COLUMNS FROM t; COMMIT AND CHAIN RELEASE", SQLPARSER_MYSQL);
    rejected("SELECT 1; START TRANSACTION READ ONLY, READ WRITE", SQLPARSER_MYSQL);
    clear_document();
    sqlparser_limits limits = sqlparser_default_limits();
    limits.max_statements = 2;
    check_equal(sqlparser_parse(sql, strlen(sql), &limits, &document, NULL), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
    limits = sqlparser_default_limits(); limits.max_nodes = 2;
    check_equal(sqlparser_parse(sql, strlen(sql), &limits, &document, NULL), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
    parse(sql, SQLPARSER_MYSQL);
    check_equal(sqlparser_statements(document).count, 3u);
  }

  it("preserves CREATE TABLE LIKE sources and TRUNCATE as distinct DDL") {
    const sqlparser_node *s = parse("CREATE TEMPORARY TABLE IF NOT EXISTS app.copy (LIKE source); TRUNCATE TABLE app.copy; TRUNCATE copy", SQLPARSER_MYSQL);
    check_equal(s->kind, SQLPARSER_CREATE_TABLE);
    text_is(node(s->as.create_table.table), "app.copy");
    text_is(node(s->as.create_table.like_table), "source");
    check_true(s->as.create_table.temporary); check_true(s->as.create_table.if_not_exists);
    check_equal(s->as.create_table.query, SQLPARSER_NONE);
    check_equal(s->as.create_table.elements.count, 0u);
    s = node(s->next); check_equal(s->kind, SQLPARSER_TRUNCATE_TABLE);
    text_is(node(s->as.maintenance.target), "app.copy");
    s = node(s->next); check_equal(s->kind, SQLPARSER_TRUNCATE_TABLE);
    text_is(node(s->as.maintenance.target), "copy");
    s = parse("CREATE TABLE copy LIKE app.source", SQLPARSER_MYSQL);
    text_is(node(s->as.create_table.like_table), "app.source");
    const char *invalid[] = {"CREATE TABLE t LIKE", "CREATE TABLE t(a INT) LIKE src",
      "CREATE TABLE t LIKE src AS SELECT 1", "TRUNCATE t WHERE 1", "TRUNCATE t,u", "TRUNCATE IF EXISTS t"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("CREATE TABLE t LIKE src", SQLPARSER_SQLITE);
    rejected("TRUNCATE TABLE t", SQLPARSER_SQLITE);
  }

  it("retains MySQL ALTER engine and column default actions") {
    const sqlparser_node *s = parse("ALTER TABLE app.t ENGINE=heap; ALTER TABLE t ALTER COLUMN a SET DEFAULT 'value'; ALTER TABLE t ALTER b DROP DEFAULT", SQLPARSER_MYSQL);
    check_equal(s->kind, SQLPARSER_ALTER_TABLE);
    check_equal(s->as.alter.action, SQLPARSER_SET_ENGINE);
    text_is(node(s->as.alter.table), "app.t"); text_is(node(s->as.alter.value), "heap");
    check_equal(s->as.alter.column, SQLPARSER_NONE);
    s = node(s->next); check_equal(s->as.alter.action, SQLPARSER_SET_COLUMN_DEFAULT);
    text_is(node(s->as.alter.column), "a"); text_is(node(s->as.alter.value), "'value'");
    s = node(s->next); check_equal(s->as.alter.action, SQLPARSER_DROP_COLUMN_DEFAULT);
    text_is(node(s->as.alter.column), "b"); check_equal(s->as.alter.value, SQLPARSER_NONE);
    s = parse("ALTER TABLE t ALTER a SET DEFAULT (1+2)", SQLPARSER_MYSQL);
    check_equal(node(s->as.alter.value)->as.binary.op, SQLPARSER_OP_ADD);
    parse("ALTER TABLE t ENGINE InnoDB; ALTER TABLE t ALTER a SET DEFAULT -1; ALTER TABLE t ALTER a SET DEFAULT NULL", SQLPARSER_MYSQL);
    const char *invalid[] = {"ALTER TABLE t ALTER a SET DEFAULT", "ALTER TABLE t ALTER a SET DEFAULT other_column",
      "ALTER TABLE t ALTER a SET DEFAULT 1+2", "ALTER TABLE t ALTER a.b DROP DEFAULT", "ALTER TABLE t ENGINE=",
      "ALTER TABLE t ENGINE=heap, ALTER a DROP DEFAULT"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("ALTER TABLE t ENGINE=heap", SQLPARSER_SQLITE);
    rejected("ALTER TABLE t ALTER a DROP DEFAULT", SQLPARSER_SQLITE);
  }

  it("preserves explicit numeric ZEROFILL independently of UNSIGNED") {
    const sqlparser_node *s = parse("CREATE TABLE t(a INT(4) UNSIGNED ZEROFILL,b INT ZEROFILL,c INT ZEROFILL UNSIGNED)", SQLPARSER_MYSQL);
    const sqlparser_node *column = node(s->as.create_table.elements.first);
    const sqlparser_node *type = node(column->as.column.type);
    check_true(type->as.type.is_unsigned); check_true(type->as.type.zerofill);
    text_is(type, "INT(4) UNSIGNED ZEROFILL");
    column = node(column->next); type = node(column->as.column.type);
    check_false(type->as.type.is_unsigned); check_true(type->as.type.zerofill);
    column = node(column->next); type = node(column->as.column.type);
    check_true(type->as.type.is_unsigned); check_true(type->as.type.zerofill);
    s = parse("CREATE TABLE t(a INT ZEROFILL)", SQLPARSER_SQLITE);
    type = node(node(s->as.create_table.elements.first)->as.column.type);
    check_false(type->as.type.zerofill);
    text_is(node(type->as.type.name), "INT ZEROFILL");
  }

  it("retains ON UPDATE timestamps and default expressions separately") {
    const sqlparser_node *s = parse("CREATE TABLE t(ts TIMESTAMP(6) DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6))", SQLPARSER_MYSQL);
    const sqlparser_node *column = node(s->as.create_table.elements.first);
    check_equal(column->as.column.constraints.count, 2u);
    const sqlparser_node *constraint = node(column->as.column.constraints.first);
    check_equal(constraint->as.constraint.kind, SQLPARSER_DEFAULT);
    constraint = node(constraint->next);
    check_equal(constraint->as.constraint.kind, SQLPARSER_ON_UPDATE);
    const sqlparser_node *call = node(constraint->as.constraint.expression);
    check_equal(call->kind, SQLPARSER_CALL);
    text_is(node(call->as.call.name), "CURRENT_TIMESTAMP");
    text_is(node(call->as.call.arguments.first), "6");
    const char *forms[] = {"CREATE TABLE t(ts TIMESTAMP ON UPDATE NOW())",
      "CREATE TABLE t(ts TIMESTAMP ON UPDATE LOCALTIME DEFAULT 0)",
      "CREATE TABLE t(ts TIMESTAMP ON UPDATE LOCALTIMESTAMP())"};
    for (size_t i = 0; i < sizeof(forms)/sizeof(forms[0]); ++i) parse(forms[i], SQLPARSER_MYSQL);
    check_equal(expression("SELECT CURRENT_TIMESTAMP", SQLPARSER_MYSQL)->kind, SQLPARSER_CALL);
    const char *invalid[] = {"CREATE TABLE t(ts TIMESTAMP ON UPDATE 1)", "CREATE TABLE t(ts TIMESTAMP ON UPDATE arbitrary())",
      "CREATE TABLE t(ts TIMESTAMP ON UPDATE CURRENT_TIMESTAMP(1.5))", "SELECT CURRENT_TIMESTAMP(1e1)",
      "SELECT CURRENT_TIMESTAMP(-1)", "SELECT CURRENT_TIMESTAMP(1,2)"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("CREATE TABLE t(ts TIMESTAMP ON UPDATE CURRENT_TIMESTAMP)", SQLPARSER_SQLITE);
  }

  it("retains SQL prepared statement names sources and ordered variable bindings") {
    const sqlparser_node *s = parse("PREPARE stmt FROM 'SELECT ? + ?'; EXECUTE stmt USING @first,@second; DEALLOCATE PREPARE stmt", SQLPARSER_MYSQL);
    check_equal(s->kind, SQLPARSER_PREPARE);
    text_is(node(s->as.prepared.name), "stmt");
    text_is(node(s->as.prepared.source), "'SELECT ? + ?'");
    check_equal(node(s->as.prepared.source)->kind, SQLPARSER_STRING);
    s = node(s->next); check_equal(s->kind, SQLPARSER_EXECUTE);
    check_equal(s->as.prepared.parameters.count, 2u);
    check_equal(node(s->as.prepared.parameters.first)->kind, SQLPARSER_VARIABLE);
    text_is(node(s->as.prepared.parameters.first), "@first");
    text_is(node(s->as.prepared.parameters.last), "@second");
    s = node(s->next); check_equal(s->kind, SQLPARSER_DEALLOCATE);
    text_is(node(s->as.prepared.name), "stmt");
    s = parse("PREPARE `execute` FROM @sql; EXECUTE `execute`; DROP PREPARE `execute`", SQLPARSER_MYSQL);
    check_equal(node(s->as.prepared.source)->kind, SQLPARSER_VARIABLE);
    s = node(s->next); check_equal(s->as.prepared.parameters.count, 0u);
    s = node(s->next); check_equal(s->kind, SQLPARSER_DEALLOCATE);
    parse("SELECT prepare,execute,deallocate,prepare.1e10 FROM prepare", SQLPARSER_MYSQL);
  }

  it("rejects nonvariable EXECUTE bindings and malformed PREPARE commands") {
    const char *invalid[] = {"PREPARE stmt FROM SELECT 1", "PREPARE stmt FROM @@sql_mode",
      "PREPARE stmt FROM concat('SELECT ',1)", "PREPARE app.stmt FROM 'SELECT 1'",
      "EXECUTE stmt USING 1", "EXECUTE stmt USING ?", "EXECUTE stmt USING @@sql_mode",
      "EXECUTE stmt USING @a+1", "EXECUTE stmt USING", "EXECUTE stmt USING @a,",
      "DEALLOCATE stmt", "DROP PREPARE", "DEALLOCATE PREPARE stmt USING @a"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    const char *mysql_only[] = {"PREPARE stmt FROM 'SELECT 1'", "EXECUTE stmt", "DEALLOCATE PREPARE stmt"};
    for (size_t i = 0; i < sizeof(mysql_only)/sizeof(mysql_only[0]); ++i) rejected(mysql_only[i], SQLPARSER_SQLITE);
    rejected("SELECT 1; EXECUTE stmt USING @@sql_mode", SQLPARSER_MYSQL);
  }

  it("uses MySQL XOR between AND and OR and left associativity") {
    const sqlparser_node *n = expression("SELECT a OR b XOR c AND d", SQLPARSER_MYSQL);
    check_equal(n->as.binary.op, SQLPARSER_OP_OR);
    n = node(n->as.binary.right); check_equal(n->as.binary.op, SQLPARSER_OP_XOR);
    check_equal(node(n->as.binary.right)->as.binary.op, SQLPARSER_OP_AND);
    n = expression("SELECT a XOR b XOR c", SQLPARSER_MYSQL);
    check_equal(n->as.binary.op, SQLPARSER_OP_XOR);
    check_equal(node(n->as.binary.left)->as.binary.op, SQLPARSER_OP_XOR);
    rejected("SELECT a XOR b", SQLPARSER_SQLITE);
    rejected("SELECT a XOR", SQLPARSER_MYSQL);
  }

  it("retains right-associated MySQL user variable assignment expressions") {
    const sqlparser_node *n = expression("SELECT @a:=@b:=1 OR 0", SQLPARSER_MYSQL);
    check_equal(n->kind, SQLPARSER_ASSIGNMENT);
    text_is(node(n->as.assignment.name), "@a");
    n = node(n->as.assignment.value); check_equal(n->kind, SQLPARSER_ASSIGNMENT);
    text_is(node(n->as.assignment.name), "@b");
    check_equal(node(n->as.assignment.value)->as.binary.op, SQLPARSER_OP_OR);
    const sqlparser_node *s = parse("UPDATE t SET b=(@tmp:=@tmp+1) ORDER BY a", SQLPARSER_MYSQL);
    n = node(node(s->as.update.assignments.first)->as.assignment.value);
    check_equal(n->kind, SQLPARSER_ASSIGNMENT);
    check_equal(node(n->as.assignment.value)->as.binary.op, SQLPARSER_OP_ADD);
    rejected("SELECT @@sql_mode:=1", SQLPARSER_MYSQL);
    rejected("SELECT a:=1", SQLPARSER_MYSQL);
    rejected("SELECT @a:=DEFAULT", SQLPARSER_MYSQL);
    rejected("SELECT @a:=1", SQLPARSER_SQLITE);
  }

  it("retains LOW_PRIORITY across each supported MySQL write form") {
    const char *inserts[] = {"INSERT LOW_PRIORITY INTO t VALUES(1)",
      "REPLACE LOW_PRIORITY INTO t SET a=DEFAULT", "INSERT LOW_PRIORITY INTO t SELECT 1",
      "INSERT LOW_PRIORITY INTO t(a) SELECT 1", "INSERT LOW_PRIORITY INTO t() SELECT 1"};
    for (size_t i = 0; i < sizeof(inserts)/sizeof(inserts[0]); ++i) {
      check_true(parse(inserts[i], SQLPARSER_MYSQL)->as.insert.low_priority);
      rejected(inserts[i], SQLPARSER_SQLITE);
    }
    check_true(parse("UPDATE LOW_PRIORITY t SET a=1", SQLPARSER_MYSQL)->as.update.low_priority);
    check_true(parse("DELETE LOW_PRIORITY FROM t WHERE a=1", SQLPARSER_MYSQL)->as.delete_stmt.low_priority);
    check_false(parse("INSERT INTO t VALUES(1)", SQLPARSER_MYSQL)->as.insert.low_priority);
    rejected("INSERT INTO LOW_PRIORITY t VALUES(1)", SQLPARSER_MYSQL);
    rejected("DELETE FROM t LOW_PRIORITY", SQLPARSER_MYSQL);
  }

  it("separates multi-table DELETE targets from its joined source tables") {
    const sqlparser_node *s = parse("DELETE LOW_PRIORITY a.*,b FROM t AS a JOIN u AS b ON a.id=b.id WHERE b.id>0", SQLPARSER_MYSQL);
    check_true(s->as.delete_stmt.low_priority);
    check_equal(s->as.delete_stmt.table, SQLPARSER_NONE);
    check_equal(s->as.delete_stmt.targets.count, 2u);
    const sqlparser_node *target = node(s->as.delete_stmt.targets.first);
    check_equal(target->kind, SQLPARSER_STAR); text_is(target, "a.*");
    text_is(node(target->next), "b");
    check_equal(node(s->as.delete_stmt.from)->kind, SQLPARSER_JOIN);
    check_equal(node(s->as.delete_stmt.where)->as.binary.op, SQLPARSER_OP_GT);
    s = parse("DELETE FROM a USING t a,t b WHERE a.id=b.id", SQLPARSER_MYSQL);
    check_equal(s->as.delete_stmt.targets.count, 1u);
    check_equal(node(s->as.delete_stmt.from)->as.join.kind, SQLPARSER_JOIN_CROSS);
    const char *invalid[] = {"DELETE a FROM t a LIMIT 1", "DELETE FROM a USING t a ORDER BY a.id",
      "DELETE FROM a USING", "DELETE a AS b FROM t a", "DELETE * FROM t"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("DELETE FROM a USING t a", SQLPARSER_SQLITE);
    s = parse("DELETE FROM t", SQLPARSER_MYSQL);
    check_equal(s->as.delete_stmt.targets.count, 0u);
    check_equal(s->as.delete_stmt.from, SQLPARSER_NONE);
  }

  it("retains MySQL table lock aliases and per-table modes") {
    const sqlparser_node *s = parse("LOCK TABLES app.t AS a READ LOCAL,u b WRITE,v READ; UNLOCK TABLES", SQLPARSER_MYSQL);
    check_equal(s->kind, SQLPARSER_LOCK_TABLES);
    check_equal(s->as.lock_tables.targets.count, 3u);
    const sqlparser_node *entry = node(s->as.lock_tables.targets.first);
    check_equal(entry->kind, SQLPARSER_LOCK_TARGET);
    text_is(node(entry->as.lock_target.table), "app.t");
    text_is(node(entry->as.lock_target.alias), "a");
    check_equal(entry->as.lock_target.mode, SQLPARSER_LOCK_READ_LOCAL);
    entry = node(entry->next); check_equal(entry->as.lock_target.mode, SQLPARSER_LOCK_WRITE);
    text_is(node(entry->as.lock_target.alias), "b");
    entry = node(entry->next); check_equal(entry->as.lock_target.mode, SQLPARSER_LOCK_READ);
    check_equal(entry->as.lock_target.alias, SQLPARSER_NONE);
    s = node(s->next); check_equal(s->kind, SQLPARSER_UNLOCK_TABLES); text_is(s, "UNLOCK TABLES");
    parse("LOCK TABLE t WRITE; UNLOCK TABLE", SQLPARSER_MYSQL);
    const char *invalid[] = {"LOCK TABLES", "LOCK TABLES t", "LOCK TABLES t LOW_PRIORITY WRITE",
      "LOCK TABLES t WRITE LOCAL", "UNLOCK TABLES t"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("LOCK TABLES t WRITE", SQLPARSER_SQLITE);
    rejected("UNLOCK TABLES", SQLPARSER_SQLITE);
  }

  it("retains explicit EXPLAIN formats and view CHECK OPTION modes") {
    const sqlparser_node *s = parse("EXPLAIN FORMAT=TREE SELECT 1; DESC FORMAT=JSON UPDATE t SET a=1; DESCRIBE FORMAT=TRADITIONAL DELETE FROM t", SQLPARSER_MYSQL);
    check_equal(s->as.explain.format, SQLPARSER_EXPLAIN_TREE);
    check_equal(node(s->as.explain.statement)->kind, SQLPARSER_SELECT);
    s = node(s->next); check_equal(s->as.explain.format, SQLPARSER_EXPLAIN_JSON);
    s = node(s->next); check_equal(s->as.explain.format, SQLPARSER_EXPLAIN_TRADITIONAL);
    s = parse("CREATE VIEW v AS SELECT a FROM t WHERE a>0 WITH LOCAL CHECK OPTION; CREATE VIEW v2 AS SELECT 1 WITH CASCADED CHECK OPTION; CREATE VIEW v3 AS SELECT 1 WITH CHECK OPTION", SQLPARSER_MYSQL);
    check_equal(s->as.create_view.check, SQLPARSER_VIEW_CHECK_LOCAL);
    check_equal(node(s->as.create_view.query)->kind, SQLPARSER_SELECT);
    s = node(s->next); check_equal(s->as.create_view.check, SQLPARSER_VIEW_CHECK_CASCADED);
    s = node(s->next); check_equal(s->as.create_view.check, SQLPARSER_VIEW_CHECK_DEFAULT);
    rejected("EXPLAIN FORMAT TREE SELECT 1", SQLPARSER_MYSQL);
    rejected("EXPLAIN FORMAT=JSON SET a=1", SQLPARSER_MYSQL);
    rejected("CREATE VIEW v AS SELECT 1 WITH LOCAL CASCADED CHECK OPTION", SQLPARSER_MYSQL);
    rejected("EXPLAIN FORMAT=TREE SELECT 1", SQLPARSER_SQLITE);
    rejected("CREATE VIEW v AS SELECT 1 WITH CHECK OPTION", SQLPARSER_SQLITE);
  }

  it("separates MySQL window partition and ordering from query clauses") {
    const sqlparser_node *s = parse("SELECT SUM(x) OVER (PARTITION BY department ORDER BY created DESC) AS total FROM t ORDER BY total LIMIT 2", SQLPARSER_MYSQL);
    const sqlparser_node *window = node(node(s->as.select.columns.first)->as.projection.expression);
    check_equal(window->kind, SQLPARSER_WINDOW);
    check_equal(node(window->as.window.call)->kind, SQLPARSER_CALL);
    text_is(node(window->as.window.partition_by.first), "department");
    const sqlparser_node *order = node(window->as.window.order_by.first);
    check_true(order->as.order.descending);
    text_is(node(order->as.order.expression), "created");
    text_is(node(node(s->as.select.order_by.first)->as.order.expression), "total");
    window = expression("SELECT RANK() OVER ()", SQLPARSER_MYSQL);
    check_equal(window->kind, SQLPARSER_WINDOW);
    check_equal(window->as.window.partition_by.count, 0u);
    check_equal(window->as.window.order_by.count, 0u);
    window = expression("SELECT SUM(x) OVER w", SQLPARSER_MYSQL);
    text_is(node(window->as.window.name), "w");
    parse("WITH RECURSIVE q AS (SELECT rank() OVER (ORDER BY b) AS a FROM t UNION SELECT a FROM q) SELECT * FROM q", SQLPARSER_MYSQL);
    const char *invalid[] = {"SELECT 1 OVER ()", "SELECT RANK() OVER (ORDER BY)",
      "SELECT RANK() OVER (ORDER BY a PARTITION BY b)", "SELECT RANK() OVER () OVER ()",
      "SELECT abs(x) OVER ()", "SELECT db.sum(x) OVER ()", "SELECT `sum`(x) OVER ()",
      "SELECT SUM(x) OVER (ORDER BY a ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], SQLPARSER_MYSQL);
    rejected("SELECT RANK() OVER ()", SQLPARSER_SQLITE);
  }

  it("bounds all new MySQL statement families under input truncation") {
    const char *samples[] = {
      "CREATE TABLE t(n INT ZEROFILL, ts TIMESTAMP ON UPDATE CURRENT_TIMESTAMP(6)); CREATE TABLE u LIKE t; ALTER TABLE u ALTER n SET DEFAULT (1+2); TRUNCATE u;",
      "PREPARE stmt FROM @sql; EXECUTE stmt USING @a,@b; DEALLOCATE PREPARE stmt;",
      "SELECT @a:=@b:=1 XOR 0; INSERT LOW_PRIORITY INTO t VALUES(1); DELETE a.*,b FROM t a JOIN u b ON a.id=b.id;",
      "LOCK TABLES t AS a READ LOCAL,u WRITE; UNLOCK TABLES; EXPLAIN FORMAT=TREE SELECT SQL_CALC_FOUND_ROWS SUM(x) OVER (PARTITION BY a ORDER BY b) FROM t; CREATE VIEW v AS SELECT 1 WITH LOCAL CHECK OPTION;"
    };
    for (size_t sample = 0; sample < sizeof(samples)/sizeof(samples[0]); ++sample) {
      const char *sql = samples[sample];
      for (size_t length = 0; length <= strlen(sql); ++length) {
        clear_document();
        sqlparser_error error;
        sqlparser_status status = sqlparser_parse(sql, length, NULL, &document, &error);
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

  it("uses SQLite left-to-right joins and permits outer joins without ON") {
    const sqlparser_node *s = parse("SELECT * FROM a,b JOIN c ON b.id=c.id", SQLPARSER_SQLITE);
    const sqlparser_node *join = node(s->as.select.from);
    check_equal(join->as.join.kind, SQLPARSER_JOIN_INNER);
    check_equal(node(join->as.join.left)->as.join.kind, SQLPARSER_JOIN_CROSS);
    s = parse("SELECT * FROM a,b JOIN c ON b.id=c.id", SQLPARSER_MYSQL);
    join = node(s->as.select.from);
    check_equal(join->as.join.kind, SQLPARSER_JOIN_CROSS);
    check_equal(node(join->as.join.right)->as.join.kind, SQLPARSER_JOIN_INNER);
    parse("SELECT * FROM a FULL OUTER JOIN b", SQLPARSER_SQLITE);
    rejected("SELECT * FROM a FULL OUTER JOIN b", SQLPARSER_MYSQL);
  }

  it("records SQLite transaction modes and savepoint names") {
    const sqlparser_node *s = parse("BEGIN IMMEDIATE TRANSACTION; SAVEPOINT s; ROLLBACK TO SAVEPOINT s; RELEASE s; END TRANSACTION", SQLPARSER_SQLITE);
    check_equal(sqlparser_statements(document).count, 5u);
    check_equal(s->as.transaction.mode, SQLPARSER_IMMEDIATE);
    s = node(s->next); check_equal(s->as.transaction.kind, SQLPARSER_SAVEPOINT);
    check_equal(node(s->as.transaction.name)->span.length, 1u);
    s = node(s->next); check_equal(s->as.transaction.kind, SQLPARSER_ROLLBACK_TO);
    rejected("BEGIN IMMEDIATE", SQLPARSER_MYSQL);
    rejected("BEGIN WORK", SQLPARSER_SQLITE);
  }

  it("retains SQLite typeless columns conflict policies and references") {
    const sqlparser_node *s = parse("CREATE TABLE t(id INTEGER PRIMARY KEY AUTOINCREMENT, value, label TEXT COLLATE nocase UNIQUE ON CONFLICT IGNORE, FOREIGN KEY(id) REFERENCES parent)", SQLPARSER_SQLITE);
    const sqlparser_node *id = node(s->as.create_table.elements.first);
    check_true(node(id->as.column.constraints.last)->as.constraint.autoincrement);
    const sqlparser_node *value = node(id->next);
    check_equal(value->as.column.type, SQLPARSER_NONE);
    const sqlparser_node *label = node(value->next);
    check_equal(node(label->as.column.constraints.last)->as.constraint.conflict, SQLPARSER_CONFLICT_IGNORE);
    rejected("CREATE TABLE t(value)", SQLPARSER_MYSQL);
    rejected("CREATE TABLE t(id INT) ENGINE=InnoDB", SQLPARSER_SQLITE);
    s = parse("CREATE TABLE t(id INT AUTO_INCREMENT)", SQLPARSER_SQLITE);
    check_equal(node(s->as.create_table.elements.first)->as.column.constraints.count, 0u);
    rejected("CREATE TABLE t(id INT AUTOINCREMENT)", SQLPARSER_SQLITE);
    rejected("DROP TABLE t,u", SQLPARSER_SQLITE);
    rejected("DROP TEMPORARY TABLE t", SQLPARSER_SQLITE);
    if (!sqlparser_sqlite_update_delete_limit_enabled())
      rejected("DELETE FROM t LIMIT 1", SQLPARSER_SQLITE);
    s = parse("CREATE TABLE t(id INTEGER PRIMARY KEY) WITHOUT ROWID, STRICT", SQLPARSER_SQLITE);
    check_equal(s->as.create_table.options.count, 2u);
    check_equal(node(s->as.create_table.options.first)->as.table_option.kind, SQLPARSER_WITHOUT_ROWID);
  }

  it("retains pragma index view and attach arguments") {
    const sqlparser_node *s = parse("PRAGMA main.cache_size=-2000; CREATE UNIQUE INDEX IF NOT EXISTS ix ON t(a DESC) WHERE a>0; CREATE TEMP VIEW v(x) AS SELECT a FROM t; ATTACH DATABASE ':memory:' AS aux; DETACH DATABASE aux; DROP INDEX ix; DROP VIEW v", SQLPARSER_SQLITE);
    check_equal(sqlparser_statements(document).count, 7u);
    check_equal(s->kind, SQLPARSER_PRAGMA);
    check_equal(node(s->as.pragma.value)->as.unary.op, SQLPARSER_OP_NEGATE);
    s = node(s->next); check_equal(s->kind, SQLPARSER_CREATE_INDEX);
    check_true(s->as.create_index.unique); check_true(s->as.create_index.if_not_exists);
    check_not_equal(s->as.create_index.where, SQLPARSER_NONE);
    s = node(s->next); check_equal(s->kind, SQLPARSER_CREATE_VIEW);
    check_true(s->as.create_view.temporary);
    check_equal(s->as.create_view.columns.count, 1u);
    rejected("PRAGMA cache_size=1", SQLPARSER_MYSQL);
    rejected("SHOW TABLES", SQLPARSER_SQLITE);
    rejected("SET NAMES utf8mb4", SQLPARSER_SQLITE);
    rejected("INSERT INTO t SET a=1", SQLPARSER_SQLITE);
    parse("PRAGMA foreign_keys=true; PRAGMA locking_mode=exclusive", SQLPARSER_SQLITE);
  }

  it("retains recursive CTEs VALUES and compound operations") {
    const sqlparser_node *s = parse("WITH RECURSIVE q(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM q WHERE x<3) SELECT x FROM q EXCEPT SELECT 2 ORDER BY x", SQLPARSER_SQLITE);
    check_equal(s->kind, SQLPARSER_WITH); check_true(s->as.with.recursive);
    check_equal(node(s->as.with.body)->as.compound.kind, SQLPARSER_COMPOUND_EXCEPT);
    check_equal(node(s->as.with.body)->as.compound.order_by.count, 1u);
    const sqlparser_node *cte = node(s->as.with.bindings.first);
    check_equal(node(node(cte->as.cte.query)->as.compound.left)->kind, SQLPARSER_VALUES);
    s = parse("INSERT OR IGNORE INTO t VALUES(1),(2); INSERT INTO t DEFAULT VALUES", SQLPARSER_SQLITE);
    check_equal(s->as.insert.conflict, SQLPARSER_CONFLICT_IGNORE);
    check_true(node(s->next)->as.insert.default_values);
    rejected("VALUES(1) ORDER BY 1", SQLPARSER_SQLITE);
    rejected("SELECT 1 UNION VALUES(2) LIMIT 1", SQLPARSER_SQLITE);
    rejected("INSERT INTO t () VALUES()", SQLPARSER_SQLITE);
    check_equal(parse("CREATE VIRTUAL TABLE t USING fts5(body)", SQLPARSER_SQLITE)->kind, SQLPARSER_CREATE_VIRTUAL_TABLE);
    parse("SELECT 1; CREATE TRIGGER tr AFTER INSERT ON t BEGIN SELECT 1; END", SQLPARSER_SQLITE);
    check_equal(sqlparser_statements(document).count, 2u);
  }

  // https://dev.mysql.com/doc/refman/8.4/en/with.html
  // https://www.sqlite.org/lang_with.html
  it("preserves recursive and nested CTE structure in both dialects") {
    for (int d = SQLPARSER_MYSQL; d <= SQLPARSER_SQLITE; ++d) {
      const sqlparser_node *s = parse("WITH RECURSIVE numbers(n) AS (SELECT 7 UNION ALL SELECT n+1 FROM numbers WHERE n<9), doubled(v) AS (SELECT n*2 FROM numbers) SELECT v FROM doubled ORDER BY v DESC LIMIT 2", (sqlparser_dialect)d);
      check_equal(s->kind, SQLPARSER_WITH);
      check_true(s->as.with.recursive);
      check_equal(s->as.with.bindings.count, 2u);
      const sqlparser_node *binding = node(s->as.with.bindings.first);
      check_equal(binding->kind, SQLPARSER_CTE);
      check_equal(binding->as.cte.columns.count, 1u);
      check_equal(node(binding->as.cte.query)->kind, SQLPARSER_UNION);
      const sqlparser_node *body = node(s->as.with.body);
      check_equal(body->as.select.order_by.count, 1u);
      check_not_equal(body->as.select.limit, SQLPARSER_NONE);
      parse("WITH outer_q AS (SELECT 7) SELECT * FROM (WITH inner_q AS (SELECT 9) SELECT * FROM inner_q) AS nested_q", (sqlparser_dialect)d);
      const char *invalid[] = {
        "WITH a AS (SELECT 7) WITH b AS (SELECT 9) SELECT * FROM b",
        "WITH a() AS (SELECT 7) SELECT * FROM a",
        "WITH a AS SELECT 7 SELECT * FROM a",
        "WITH a AS (SELECT 7)"
      };
      for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) rejected(invalid[i], (sqlparser_dialect)d);
    }
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    check_equal(sqlite3_exec(database, "WITH a AS (SELECT 7) WITH b AS (SELECT 9) SELECT * FROM b", NULL, NULL, NULL), SQLITE_ERROR);
  }

  it("uses each dialect's documented WITH placement for writes") {
    const sqlparser_node *s = parse("INSERT INTO target(n) WITH source(n) AS (SELECT 7) SELECT n FROM source", SQLPARSER_MYSQL);
    check_equal(s->kind, SQLPARSER_INSERT);
    check_equal(node(s->as.insert.query)->kind, SQLPARSER_WITH);
    s = parse("REPLACE INTO target(n) WITH source(n) AS (SELECT 7) SELECT n FROM source", SQLPARSER_MYSQL);
    check_true(s->as.insert.replace);
    check_equal(node(s->as.insert.query)->kind, SQLPARSER_WITH);
    const char *prefix_insert = "WITH source(n) AS (SELECT 7) INSERT INTO target SELECT n FROM source";
    rejected(prefix_insert, SQLPARSER_MYSQL);
    s = parse(prefix_insert, SQLPARSER_SQLITE);
    check_equal(node(s->as.with.body)->kind, SQLPARSER_INSERT);
    s = parse("WITH source(n) AS (SELECT 7) UPDATE target SET n=8 WHERE n IN (SELECT n FROM source); WITH source(n) AS (SELECT 8) DELETE FROM target WHERE n IN (SELECT n FROM source)", SQLPARSER_MYSQL);
    check_equal(sqlparser_statements(document).count, 2u);
    check_equal(node(s->as.with.body)->kind, SQLPARSER_UPDATE);
    check_equal(node(node(s->next)->as.with.body)->kind, SQLPARSER_DELETE);
    rejected("WITH source(n) AS (SELECT 7) REPLACE INTO target SELECT n FROM source", SQLPARSER_MYSQL);
    rejected("WITH source(n) AS (SELECT 7) SET @value=1", SQLPARSER_MYSQL);
  }

  // https://dev.mysql.com/doc/refman/8.4/en/savepoint.html
  it("supports MySQL savepoints with mandatory RELEASE SAVEPOINT spelling") {
    const sqlparser_node *s = parse("SAVEPOINT checkpoint; ROLLBACK WORK TO SAVEPOINT checkpoint; ROLLBACK TO checkpoint; RELEASE SAVEPOINT checkpoint", SQLPARSER_MYSQL);
    check_equal(sqlparser_statements(document).count, 4u);
    check_equal(s->as.transaction.kind, SQLPARSER_SAVEPOINT);
    check_equal(node(s->as.transaction.name)->span.length, strlen("checkpoint"));
    s = node(s->next); check_equal(s->as.transaction.kind, SQLPARSER_ROLLBACK_TO);
    s = node(s->next); check_equal(s->as.transaction.kind, SQLPARSER_ROLLBACK_TO);
    s = node(s->next); check_equal(s->as.transaction.kind, SQLPARSER_RELEASE);
    parse("SELECT savepoint, `with`, `recursive`, `release`, `to` FROM t; SAVEPOINT savepoint; ROLLBACK TO SAVEPOINT savepoint", SQLPARSER_MYSQL);
    parse("SELECT t.with, t.recursive, t.release, t.to FROM schema.with AS t", SQLPARSER_MYSQL);
    rejected("RELEASE checkpoint", SQLPARSER_MYSQL);
    parse("RELEASE checkpoint", SQLPARSER_SQLITE);
    rejected("ROLLBACK WORK TO checkpoint", SQLPARSER_SQLITE);
    rejected("SAVEPOINT schema.checkpoint", SQLPARSER_MYSQL);
    rejected("SAVEPOINT 'checkpoint'", SQLPARSER_MYSQL);
    rejected("SELECT with", SQLPARSER_MYSQL);
    rejected("SELECT recursive", SQLPARSER_MYSQL);
  }

  it("bounds truncated MySQL CTEs and savepoints and counts their outer statements") {
    const char *sql = "WITH RECURSIVE q(n) AS (SELECT 7 UNION ALL SELECT n+1 FROM q WHERE n<9) SELECT n FROM q; SAVEPOINT checkpoint; ROLLBACK TO SAVEPOINT checkpoint; RELEASE SAVEPOINT checkpoint";
    for (size_t length = 0; length <= strlen(sql); ++length) {
      clear_document();
      sqlparser_error error;
      sqlparser_status status = sqlparser_parse_dialect(sql, length, SQLPARSER_MYSQL, NULL, &document, &error);
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
    clear_document();
    sqlparser_limits limits = sqlparser_default_limits(); limits.max_statements = 3;
    sqlparser_error error;
    check_equal(sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_MYSQL, &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
    limits.max_statements = 4;
    check_equal(sqlparser_parse_dialect(sql, strlen(sql), SQLPARSER_MYSQL, &limits, &document, &error), SQLPARSER_OK);
    check_equal(sqlparser_statements(document).count, 4u);
  }

  it("matches the installed SQLite engine on representative executable batches") {
    check_equal(sqlite3_open(":memory:", &database), SQLITE_OK);
    const char *batches[] = {
      "PRAGMA foreign_keys=ON; CREATE TABLE t(id INTEGER PRIMARY KEY AUTOINCREMENT, value, label TEXT COLLATE nocase UNIQUE ON CONFLICT IGNORE); INSERT INTO t(value,label) VALUES(1,'a'),(2,'b');",
      "BEGIN IMMEDIATE; SAVEPOINT s; INSERT OR IGNORE INTO t(label) VALUES('a'); ROLLBACK TO s; RELEASE s; COMMIT;",
      "CREATE INDEX ix ON t(value DESC) WHERE value>0; CREATE VIEW v AS SELECT * FROM t; SELECT CAST(value AS TEXT)||label FROM v; DROP VIEW v; DROP INDEX ix;",
      "WITH RECURSIVE q(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM q WHERE x<3) SELECT x FROM q EXCEPT SELECT 2;",
      "SELECT 1=2<3, 1|2&3, 'a'||'b', 1 BETWEEN 2 AND 3 = 4, 1 IS NOT DISTINCT FROM 1, X'00ff', :value, ?12;"
    };
    for (size_t i = 0; i < sizeof(batches)/sizeof(batches[0]); ++i) {
      parse(batches[i], SQLPARSER_SQLITE);
      const int rc = sqlite3_exec(database, batches[i], NULL, NULL, NULL);
      if (rc != SQLITE_OK) info("SQLite: %s", sqlite3_errmsg(database));
      check_equal(rc, SQLITE_OK);
    }
    check_equal(sqlite3_prepare_v2(database, "SELECT 1|2&3,1=2<3,1+2||3*4,'a'||'b'", -1, &reference_statement, NULL), SQLITE_OK);
    check_equal(sqlite3_step(reference_statement), SQLITE_ROW);
    check_equal(sqlite3_column_int(reference_statement, 0), 3);
    check_equal(sqlite3_column_int(reference_statement, 1), 1);
    check_equal(sqlite3_column_int(reference_statement, 2), 93);
    check_equal(strcmp((const char *)sqlite3_column_text(reference_statement, 3), "ab"), 0);
    check_equal(sqlite3_step(reference_statement), SQLITE_DONE);
  }

  it("keeps every truncated SQLite input within its byte and node bounds") {
    const char *inputs[] = {
      "SELECT [a],\"b\",'c\\',X'00ff',0xF_F,1_2.3_4e-5,$ns::key(value),?12 FROM t--tail",
      "WITH q(x) AS (VALUES(1),(2)) SELECT CAST(x AS TEXT)||'a' FROM q /*tail*/",
      "CREATE TABLE t(x TEXT UNIQUE ON CONFLICT IGNORE); PRAGMA cache_size=-2000;"
    };
    for (size_t sample = 0; sample < sizeof(inputs)/sizeof(inputs[0]); ++sample) {
      for (size_t length = 0; length <= strlen(inputs[sample]); ++length) {
        sqlparser_error error;
        const sqlparser_status status = sqlparser_parse_dialect(inputs[sample], length, SQLPARSER_SQLITE, NULL, &document, &error);
        if (status == SQLPARSER_OK) {
          for (size_t i = 1; i <= sqlparser_node_count(document); ++i) {
            const sqlparser_node *n = node((sqlparser_id)i);
            check_true(n->span.offset <= length);
            check_true(n->span.length <= length - n->span.offset);
          }
          clear_document();
        } else {
          check_equal(status, SQLPARSER_SYNTAX_ERROR);
          check_null(document); check_true(error.offset <= length);
        }
      }
    }
  }

  it("rejects unknown dialects and preserves failure atomicity and limits") {
    sqlparser_error error;
    check_equal(sqlparser_parse_dialect("", 0, (sqlparser_dialect)-1, NULL, &document, &error), SQLPARSER_INVALID_ARGUMENT);
    check_null(document);
    check_equal(sqlparser_get_dialect(document), SQLPARSER_DIALECT_UNKNOWN);
    parse("SELECT 1", SQLPARSER_SQLITE);
    check_equal(sqlparser_get_dialect(document), SQLPARSER_SQLITE);
    clear_document();
    sqlparser_limits limits = sqlparser_default_limits(); limits.max_statements = 1;
    const char *batch = "SELECT 1; PRAGMA cache_size";
    check_equal(sqlparser_parse_dialect(batch, strlen(batch), SQLPARSER_SQLITE, &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
    rejected("SELECT 1; SHOW TABLES", SQLPARSER_SQLITE);
    limits = sqlparser_default_limits(); limits.max_nodes = 1;
    check_equal(sqlparser_parse_dialect("SELECT 1", 8, SQLPARSER_SQLITE, &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
    limits = sqlparser_default_limits(); limits.max_input_bytes = 1;
    check_equal(sqlparser_parse_dialect("SELECT 1", 8, SQLPARSER_SQLITE, &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    limits = sqlparser_default_limits(); limits.max_stack_entries = 8;
    const char *deep = "SELECT (((((((((((1)))))))))))";
    check_equal(sqlparser_parse_dialect(deep, strlen(deep), SQLPARSER_SQLITE, &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
  }
}
