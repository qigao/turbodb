#include <sqlparser/sqlparser.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static sqlparser_document *document;
static sqlparser_error error;

static const sqlparser_node *node(sqlparser_id id) {
  const sqlparser_node *result = sqlparser_get_node(document, id);
  check_not_null(result);
  return result;
}
static const sqlparser_node *parse(const char *sql) {
  sqlparser_status status = sqlparser_parse(sql, strlen(sql), NULL, &document, &error);
  if (status != SQLPARSER_OK) info("at byte %zu: %s; SQL: %s", error.offset, error.message, sql);
  check_equal(status, SQLPARSER_OK);
  return node(sqlparser_statements(document).first);
}
static void text_is(sqlparser_id id, const char *expected) {
  const sqlparser_node *n = node(id);
  check_equal(n->span.length, strlen(expected));
  check_equal(memcmp(sqlparser_text(document, n->span), expected, n->span.length), 0);
}

spec("standalone re2c and Lemon SQL parser") {
  before_each() { document = NULL; error = (sqlparser_error){0}; }
  after_each() { sqlparser_document_destroy(document); document = NULL; }

  it("owns an exact-length copy without reading beyond the caller slice") {
    char input[] = {'S','E','L','E','C','T',' ','1'};
    check_equal(sqlparser_parse(input, sizeof(input), NULL, &document, &error), SQLPARSER_OK);
    memset(input, 'x', sizeof(input));
    const sqlparser_node *select = node(sqlparser_statements(document).first);
    text_is(node(select->as.select.columns.first)->as.projection.expression, "1");
    sqlparser_document_destroy(document); document = NULL;
    check_equal(sqlparser_parse("SELECT 123", sizeof(input), NULL, &document, &error), SQLPARSER_OK);
    select = node(sqlparser_statements(document).first);
    text_is(node(select->as.select.columns.first)->as.projection.expression, "1");
  }

  it("preserves SELECT projections joins grouping sorting and pagination") {
    const sqlparser_node *s = parse("SeLeCt DISTINCT t.id AS ident, count(*) n FROM app.t AS t LEFT JOIN u ON t.id=u.id WHERE t.id>=? GROUP BY t.id HAVING count(*)>1 ORDER BY t.id DESC LIMIT 2,10;");
    check_equal(s->kind, SQLPARSER_SELECT);
    check_true(s->as.select.distinct);
    check_equal(s->as.select.columns.count, 2u);
    check_equal(node(s->as.select.from)->as.join.kind, SQLPARSER_JOIN_LEFT);
    check_equal(node(s->as.select.where)->as.binary.op, SQLPARSER_OP_GE);
    check_equal(s->as.select.group_by.count, 1u);
    check_true(node(s->as.select.order_by.first)->as.order.descending);
    text_is(node(s->as.select.limit)->as.limit.count, "10");
    text_is(node(s->as.select.limit)->as.limit.offset, "2");
  }

  it("uses arithmetic comparison NOT AND OR precedence") {
    const sqlparser_node *s = parse("SELECT 1+2*3 FROM t WHERE NOT a=1 AND b=2 OR c=3");
    const sqlparser_node *add = node(node(s->as.select.columns.first)->as.projection.expression);
    check_equal(add->as.binary.op, SQLPARSER_OP_ADD);
    check_equal(node(add->as.binary.right)->as.binary.op, SQLPARSER_OP_MULTIPLY);
    const sqlparser_node *disjunction = node(s->as.select.where);
    check_equal(disjunction->as.binary.op, SQLPARSER_OP_OR);
    const sqlparser_node *conjunction = node(disjunction->as.binary.left);
    check_equal(conjunction->as.binary.op, SQLPARSER_OP_AND);
    const sqlparser_node *negation = node(conjunction->as.binary.left);
    check_equal(negation->as.unary.op, SQLPARSER_OP_NOT);
    check_equal(node(negation->as.unary.operand)->as.binary.op, SQLPARSER_OP_EQ);
  }

  it("binds comparisons more tightly than BETWEEN") {
    const sqlparser_node *s = parse("SELECT a BETWEEN b AND c=d FROM t");
    const sqlparser_node *between = node(node(s->as.select.columns.first)->as.projection.expression);
    check_equal(between->kind, SQLPARSER_BETWEEN);
    check_equal(node(between->as.between.upper)->as.binary.op, SQLPARSER_OP_EQ);
  }

  it("binds explicit JOIN before comma-separated tables") {
    const sqlparser_node *s = parse("SELECT * FROM a,b JOIN c ON b.id=c.id");
    const sqlparser_node *cross = node(s->as.select.from);
    check_equal(cross->as.join.kind, SQLPARSER_JOIN_CROSS);
    check_equal(node(cross->as.join.left)->kind, SQLPARSER_TABLE);
    check_equal(node(cross->as.join.right)->as.join.kind, SQLPARSER_JOIN_INNER);
  }

  it("preserves BETWEEN IN LIKE CASE and subqueries") {
    const sqlparser_node *s = parse("SELECT CASE WHEN a BETWEEN -1 AND 2.5 THEN 'ok' ELSE 'no' END FROM t WHERE id NOT IN (SELECT id FROM u) AND name NOT LIKE 'a%' ESCAPE '!'");
    const sqlparser_node *c = node(node(s->as.select.columns.first)->as.projection.expression);
    check_equal(c->kind, SQLPARSER_CASE);
    check_equal(node(node(c->as.case_expr.branches.first)->as.when.condition)->kind, SQLPARSER_BETWEEN);
    const sqlparser_node *w = node(s->as.select.where);
    check_true(node(w->as.binary.left)->as.in.negated);
    check_equal(node(w->as.binary.right)->as.binary.op, SQLPARSER_OP_NOT_LIKE);
    check_not_equal(node(w->as.binary.right)->as.binary.escape, SQLPARSER_NONE);
  }

  it("preserves UNION ALL and derived tables") {
    const sqlparser_node *s = parse("SELECT q.* FROM (SELECT id FROM t UNION ALL SELECT id FROM u) q");
    const sqlparser_node *query = node(node(s->as.select.from)->as.table.query);
    check_equal(query->kind, SQLPARSER_UNION);
    check_true(query->as.compound.all);
    text_is(node(s->as.select.from)->as.table.alias, "q");
  }

  it("attaches UNION ordering and limits to the compound query") {
    const sqlparser_node *s = parse("SELECT id FROM a UNION ALL SELECT id FROM b ORDER BY id LIMIT 4");
    check_equal(s->kind, SQLPARSER_UNION);
    check_equal(s->as.compound.order_by.count, 1u);
    text_is(node(s->as.compound.limit)->as.limit.count, "4");
    check_equal(node(s->as.compound.right)->as.select.limit, SQLPARSER_NONE);
    check_equal(node(s->as.compound.left)->as.select.order_by.count, 0u);
  }

  it("preserves multi-row INSERT REPLACE INSERT SET and INSERT SELECT") {
    const sqlparser_node *s = parse("INSERT INTO t(id,name) VALUES(1,'a'),(2,'b'); REPLACE INTO t VALUES(3,'c'); INSERT t SET id=4; INSERT INTO t SELECT id FROM u;");
    check_equal(sqlparser_statements(document).count, 4u);
    check_equal(s->as.insert.columns.count, 2u);
    check_equal(s->as.insert.rows.count, 2u);
    check_equal(node(s->as.insert.rows.first)->as.row.values.count, 2u);
    s = node(s->next); check_true(s->as.insert.replace);
    s = node(s->next); check_equal(s->as.insert.assignments.count, 1u);
    s = node(s->next); check_equal(node(s->as.insert.query)->kind, SQLPARSER_SELECT);
  }

  it("preserves UPDATE and DELETE predicates and limits") {
    const sqlparser_node *s = parse("UPDATE app.t SET n=n+1, name=? WHERE id IN(1,2) ORDER BY id LIMIT 2; DELETE FROM t WHERE id IS NOT NULL LIMIT 1 OFFSET 2;");
    check_equal(s->kind, SQLPARSER_UPDATE);
    check_equal(s->as.update.assignments.count, 2u);
    check_equal(node(s->as.update.where)->kind, SQLPARSER_IN);
    s = node(s->next);
    check_equal(s->kind, SQLPARSER_DELETE);
    check_equal(node(s->as.delete_stmt.where)->as.unary.op, SQLPARSER_OP_IS_NOT_NULL);
    text_is(node(s->as.delete_stmt.limit)->as.limit.offset, "2");
  }

  it("keeps transaction and SET scope distinctions") {
    const sqlparser_node *s = parse("BEGIN WORK; COMMIT; START TRANSACTION; ROLLBACK WORK; SET @@SESSION.autocommit=0, GLOBAL max_connections=10, @x=1; SET NAMES utf8mb4; SET CHARACTER SET 'utf8';");
    check_equal(sqlparser_statements(document).count, 7u);
    check_equal(s->as.transaction.kind, SQLPARSER_BEGIN);
    s = node(s->next); check_equal(s->as.transaction.kind, SQLPARSER_COMMIT);
    s = node(s->next); check_equal(s->as.transaction.kind, SQLPARSER_START_TRANSACTION);
    s = node(s->next); check_equal(s->as.transaction.kind, SQLPARSER_ROLLBACK);
    s = node(s->next);
    const sqlparser_node *a = node(s->as.set.assignments.first);
    check_equal(a->as.assignment.scope, SQLPARSER_SCOPE_SESSION);
    check_equal(node(a->next)->as.assignment.scope, SQLPARSER_SCOPE_GLOBAL);
    s = node(s->next); check_equal(s->as.set.kind, SQLPARSER_SET_NAMES);
    s = node(s->next); check_equal(s->as.set.kind, SQLPARSER_SET_CHARACTER_SET);
  }

  it("retains SHOW database scope and filter") {
    const sqlparser_node *s = parse("SHOW FULL TABLES FROM db LIKE 't%'; SHOW GLOBAL VARIABLES WHERE x=1; SHOW DATABASES; SHOW TABLE STATUS IN db; SHOW COLLATION;");
    check_equal(sqlparser_statements(document).count, 5u);
    check_true(s->as.show.full);
    text_is(s->as.show.database, "db"); text_is(s->as.show.pattern, "'t%'");
    s = node(s->next); check_equal(s->as.show.scope, SQLPARSER_SCOPE_GLOBAL);
    check_not_equal(s->as.show.where, SQLPARSER_NONE);
  }

  it("retains basic CREATE and DROP TABLE definitions") {
    const sqlparser_node *s = parse("CREATE TEMPORARY TABLE IF NOT EXISTS db.t (id BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT, name VARCHAR(40) NOT NULL DEFAULT 'x', n INT DEFAULT -1, UNIQUE KEY uq(name), CHECK(n>=-1)); DROP TEMPORARY TABLE IF EXISTS db.t,u;");
    check_true(s->as.create_table.temporary); check_true(s->as.create_table.if_not_exists);
    check_equal(s->as.create_table.elements.count, 5u);
    const sqlparser_node *column = node(s->as.create_table.elements.first);
    check_equal(column->kind, SQLPARSER_COLUMN);
    check_true(node(column->as.column.type)->as.type.is_unsigned);
    check_equal(column->as.column.constraints.count, 2u);
    s = node(s->next); check_equal(s->kind, SQLPARSER_DROP_TABLE);
    check_true(s->as.drop_table.if_exists); check_equal(s->as.drop_table.tables.count, 2u);
  }

  it("handles quoted identifiers string escapes comments and empty rows") {
    const sqlparser_node *s = parse("/* prefix */ SELECT `a``b`, 'O''Brien', 'a\\\'b' FROM `t`; -- comment\n INSERT INTO t () VALUES (); # end");
    check_equal(s->as.select.columns.count, 3u);
    text_is(node(s->as.select.columns.first)->as.projection.expression, "`a``b`");
    s = node(s->next); check_equal(node(s->as.insert.rows.first)->as.row.values.count, 0u);
  }

  it("retains foreign keys and table options without executing DDL") {
    const sqlparser_node *s = parse("CREATE TABLE t(id INT, created TIMESTAMP DEFAULT CURRENT_TIMESTAMP(), CONSTRAINT fk FOREIGN KEY(id) REFERENCES u(id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE utf8mb4_bin");
    check_equal(s->as.create_table.options.count, 3u);
    const sqlparser_node *fk = node(s->as.create_table.elements.last);
    check_equal(fk->as.constraint.kind, SQLPARSER_FOREIGN_KEY);
    text_is(fk->as.constraint.table, "u");
    check_equal(fk->as.constraint.columns.count, 1u);
    check_equal(fk->as.constraint.referenced_columns.count, 1u);
  }

  it("ends newline comments before the following SQL line") {
    const sqlparser_node *s = parse("--\nSELECT 1;-- \nSELECT 2;# comment\nSELECT 3");
    check_equal(sqlparser_statements(document).count, 3u);
    check_equal(s->kind, SQLPARSER_SELECT);
  }

  it("rejects malformed SQL and discards all earlier statements") {
    const char *invalid[] = {"!", "SELECT 1!", "SELECT #1", "SELECT 'x", "SELECT `x",
      "SELECT 1 /*", "SELECT 1; SELECT", "SELECT * FROM", "SELECT (1", "SELECT 1e+",
      "SELECT 1 2", "SET @@sess.autocommit=1", "SET @@error.x=1", "CREATE TABLE t()",
      "SELECT a IN ()", "INSERT INTO t VALUES(1),", "UPDATE t SET", "DELETE FROM t WHERE",
      "SELECT /*! SQL_NO_CACHE */ 1", "SELECT /*+ hint */ 1",
      "SELECT * FROM a LEFT JOIN b", "SELECT * FROM a NATURAL JOIN b ON a.id=b.id"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      info("SQL: %s", invalid[i]);
      check_equal(sqlparser_parse(invalid[i], strlen(invalid[i]), NULL, &document, &error), SQLPARSER_SYNTAX_ERROR);
      check_null(document); check_true(error.offset <= strlen(invalid[i]));
    }
    const char embedded[] = "SELECT 1\0;SELECT 2";
    check_equal(sqlparser_parse(embedded, sizeof(embedded)-1u, NULL, &document, &error), SQLPARSER_SYNTAX_ERROR);
    check_equal(error.offset, 8u);
  }

  it("reports EOF offsets and permits empty batches") {
    check_equal(sqlparser_parse("SELECT", 6, NULL, &document, &error), SQLPARSER_SYNTAX_ERROR);
    check_equal(error.offset, 6u);
    check_equal(sqlparser_parse(NULL, 0, NULL, &document, &error), SQLPARSER_OK);
    check_equal(sqlparser_statements(document).count, 0u);
  }

  it("enforces input node statement and parser-stack budgets") {
    sqlparser_limits limits = sqlparser_default_limits();
    limits.max_input_bytes = 7;
    check_equal(sqlparser_parse("SELECT 1", 8, &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    limits = sqlparser_default_limits(); limits.max_nodes = 1;
    check_equal(sqlparser_parse("SELECT 1", 8, &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    limits = sqlparser_default_limits(); limits.max_statements = 1;
    const char *batch = "SELECT 1;SELECT 2";
    check_equal(sqlparser_parse(batch, strlen(batch), &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    limits = sqlparser_default_limits(); limits.max_stack_entries = 8;
    const char *deep = "SELECT (((((((((((1)))))))))))";
    check_equal(sqlparser_parse(deep, strlen(deep), &limits, &document, &error), SQLPARSER_LIMIT_EXCEEDED);
    check_null(document);
    limits = sqlparser_default_limits(); limits.max_input_bytes = 8;
    check_equal(sqlparser_parse("SELECT 1", 8, &limits, &document, &error), SQLPARSER_OK);
  }

  it("validates API arguments and accessor bounds") {
    check_equal(sqlparser_parse(NULL, 1, NULL, &document, &error), SQLPARSER_INVALID_ARGUMENT);
    check_equal(sqlparser_parse("", 0, NULL, NULL, &error), SQLPARSER_INVALID_ARGUMENT);
    sqlparser_limits limits = sqlparser_default_limits(); limits.max_nodes = 0;
    check_equal(sqlparser_parse("", 0, &limits, &document, &error), SQLPARSER_INVALID_ARGUMENT);
    parse("SELECT 1");
    sqlparser_document *original = document;
    check_equal(sqlparser_parse("SELECT 2", 8, NULL, &document, &error), SQLPARSER_INVALID_ARGUMENT);
    check_true(document == original);
    check_null(sqlparser_get_node(document, UINT32_MAX));
    check_null(sqlparser_text(document, (sqlparser_span){SIZE_MAX,1}));
    check_null(sqlparser_text(document, (sqlparser_span){0,SIZE_MAX}));
  }

  it("rejects every truncated prefix without publishing invalid node spans") {
    const char *sql = "SELECT CASE WHEN x BETWEEN 1 AND 3 THEN 'a\\\'b' ELSE 'z' END FROM `t``x` WHERE x IN (1,2)";
    for (size_t length = 0; length <= strlen(sql); ++length) {
      sqlparser_status status = sqlparser_parse(sql, length, NULL, &document, &error);
      if (status == SQLPARSER_OK) {
        for (sqlparser_id i = 1; i <= sqlparser_node_count(document); ++i) {
          const sqlparser_node *n = node(i);
          check_true(n->span.offset <= length);
          check_true(n->span.length <= length - n->span.offset);
          check_not_null(sqlparser_text(document, n->span));
        }
        sqlparser_document_destroy(document); document = NULL;
      } else {
        check_equal(status, SQLPARSER_SYNTAX_ERROR);
        check_null(document); check_true(error.offset <= length);
      }
    }
  }

  it("keeps stable node IDs while growing a large expression batch") {
    enum { terms = 2000, prefix_bytes = 7, bytes_per_term = 2 };
    const size_t length = prefix_bytes + terms * bytes_per_term - 1;
    char *sql = malloc(length);
    check_not_null(sql);
    memcpy(sql, "SELECT ", prefix_bytes);
    for (size_t i = prefix_bytes; i < length; ++i)
      sql[i] = ((i-prefix_bytes) % bytes_per_term == 0) ? '1' : '+';
    sqlparser_status status = sqlparser_parse(sql, length, NULL, &document, &error);
    free(sql);
    check_equal(status, SQLPARSER_OK);
    const sqlparser_node *s = node(sqlparser_statements(document).first);
    sqlparser_id expression = node(s->as.select.columns.first)->as.projection.expression;
    size_t count = 1;
    while (node(expression)->kind == SQLPARSER_BINARY) {
      const sqlparser_node *binary = node(expression);
      check_equal(binary->as.binary.op, SQLPARSER_OP_ADD);
      text_is(binary->as.binary.right, "1");
      expression = binary->as.binary.left;
      ++count;
    }
    check_equal(count, terms);
  }
}
