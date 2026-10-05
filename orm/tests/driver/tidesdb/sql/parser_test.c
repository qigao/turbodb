#include "sql.h"
#include <tinymock.h>
#include <string.h>

TINYMOCk_MOCK(tstr, literal_allocate, const void *, size_t)
static bool fail_literal;
static bool fail_work;
static tstr allocate_literal(const void *input, size_t size) {
  return fail_literal ? literal_allocate(input, size) : tstr_new_len(input, size);
}
static stl_status push_work(vec_t *work, const void *element) {
  return fail_work ? STL_OUT_OF_MEMORY : vec_push(work, element);
}
#define tstr_new_len allocate_literal
#define vec_push push_work
#include "../../../../../drivers/tidesdb/sql/parser.c"
#undef tstr_new_len
#undef vec_push

static orm_query_plan raw, parsed;
static orm_limits limits;
static orm_error_t error;

static void input_sql(const char *text) {
  orm_plan_destroy(&raw);
  orm_plan_destroy(&parsed);
  check_equal(orm_plan_init(&raw, ORM_QUERY_RAW, orm_view(text), &limits, &error), ORM_STATUS_OK);
}
static orm_status_t parse_input(void) {
  return orm_tidesdb_sql_parse(&raw, &limits, &parsed, &error);
}
static void bind_parameter(orm_value_t input) {
  check_equal(orm_plan_add_bind(&raw, input, &limits, &error), ORM_STATUS_OK);
}

spec("TidesDB driver SQL AST lowering") {
  before_each() {
    limits = (orm_limits){16, 16, 16, 16, 1024, 256, 100, 4096};
    raw = (orm_query_plan){0};
    parsed = (orm_query_plan){0};
    orm_error_init(&error);
    fail_literal = false;
    fail_work = false;
    mock_literal_allocate_reset();
  }
  after_each() {
    orm_plan_destroy(&parsed);
    orm_plan_destroy(&raw);
    mock_literal_allocate_verify();
  }
  it("lowers mixed-case SELECT comparisons and positional pagination") {
    input_sql("SeLeCt id, score FROM people WHERE id >= ? AND score <> 9 LIMIT ? OFFSET 2;");
    bind_parameter(orm_i64(3)); bind_parameter(orm_u64(5));
    check_equal(parse_input(), ORM_STATUS_OK);
    check_equal(parsed.kind, ORM_QUERY_SELECT);
    check_equal(parsed.table, "people");
    check_equal(vec_size(&parsed.columns), 2u);
    check_equal(vec_size(&parsed.predicates), 2u);
    const orm_predicate *first = vec_at_const(&parsed.predicates, 0);
    check_equal(first->comparison, ORM_COMPARE_GREATER_EQUAL);
    check_equal(first->value.data.int64_value, 3);
    check_equal(parsed.limit, 5u); check_equal(parsed.offset, 2u);
    check_true(parsed.has_limit); check_true(parsed.has_offset);
  }
  it("owns bound text and binary values independently of the raw plan") {
    const unsigned char bytes[] = {0, 255, 7};
    input_sql("INSERT INTO people (id, name, payload) VALUES (?, ?, ?)");
    bind_parameter(orm_i64(1)); bind_parameter(orm_text("a'); DELETE FROM people; --"));
    bind_parameter(orm_blob(bytes, sizeof(bytes)));
    check_equal(parse_input(), ORM_STATUS_OK);
    orm_plan_destroy(&raw);
    const orm_assignment *name = vec_at_const(&parsed.assignments, 1);
    const orm_assignment *payload = vec_at_const(&parsed.assignments, 2);
    check_equal(name->value.bytes, "a'); DELETE FROM people; --");
    check_equal(tstr_len(payload->value.bytes), sizeof(bytes));
    check_equal(memcmp(payload->value.bytes, bytes, sizeof(bytes)), 0);
  }
  it("preserves UTF-8 literal bytes and rejects unterminated quotes") {
    const char utf8[] = "\xe4\xb8\xad\xe6\x96\x87";
    input_sql("INSERT INTO people (name) VALUES ('\xe4\xb8\xad\xe6\x96\x87')");
    check_equal(parse_input(), ORM_STATUS_OK);
    const orm_assignment *name = vec_at_const(&parsed.assignments, 0);
    check_equal(name->value.bytes, utf8);
    input_sql("INSERT INTO people (name) VALUES ('unfinished)");
    check_equal(parse_input(), ORM_STATUS_SQL_ERROR);
  }
  it("decodes escaped strings booleans NULL and integer boundaries") {
    input_sql("INSERT INTO people (id, n, name, active, missing) VALUES (-9223372036854775808, 18446744073709551615, 'O''Brien', true, NULL)");
    check_equal(parse_input(), ORM_STATUS_OK);
    const orm_assignment *a = vec_at_const(&parsed.assignments, 0);
    check_equal(a->value.data.int64_value, INT64_MIN);
    a = vec_at_const(&parsed.assignments, 1);
    check_equal(a->value.data.uint64_value, UINT64_MAX);
    a = vec_at_const(&parsed.assignments, 2); check_equal(a->value.bytes, "O'Brien");
    a = vec_at_const(&parsed.assignments, 3); check_equal(a->value.data.boolean_value, 1);
    a = vec_at_const(&parsed.assignments, 4); check_equal(a->value.kind, ORM_VALUE_NULL);
  }
  it("lowers UPDATE and DELETE without rewriting the caller plan") {
    input_sql("UPDATE people SET score = ?, name = '' WHERE id = ?");
    bind_parameter(orm_f64(2.5)); bind_parameter(orm_i64(9));
    check_equal(parse_input(), ORM_STATUS_OK);
    check_equal(parsed.kind, ORM_QUERY_UPDATE);
    check_equal(vec_size(&parsed.assignments), 2u);
    check_equal(raw.kind, ORM_QUERY_RAW);
    check_equal(vec_size(&raw.raw_parameters), 2u);
    input_sql("DELETE FROM people WHERE id = 9 AND score > 2");
    check_equal(parse_input(), ORM_STATUS_OK);
    check_equal(parsed.kind, ORM_QUERY_DELETE);
    check_equal(vec_size(&parsed.predicates), 2u);
  }
  it("distinguishes IS NULL from unsupported ordinary NULL comparisons") {
    input_sql("SELECT id FROM people WHERE name IS NULL AND score IS NOT NULL");
    check_equal(parse_input(), ORM_STATUS_OK);
    const orm_predicate *p = vec_at_const(&parsed.predicates, 1);
    check_equal(p->comparison, ORM_COMPARE_NOT_EQUAL);
    check_equal(p->value.kind, ORM_VALUE_NULL);
    input_sql("SELECT id FROM people WHERE score = ?"); bind_parameter(orm_null());
    check_equal(parse_input(), ORM_STATUS_UNSUPPORTED);
    check_null(parsed.table);
  }
  it("requires exactly one binding for each anonymous placeholder") {
    input_sql("SELECT id FROM people WHERE id = ?");
    check_equal(parse_input(), ORM_STATUS_INVALID_ARGUMENT);
    input_sql("SELECT id FROM people"); bind_parameter(orm_i64(1));
    check_equal(parse_input(), ORM_STATUS_INVALID_ARGUMENT);
    check_null(parsed.table);
  }
  it("rejects unsupported syntax before publishing a partial plan") {
    const char *invalid[] = {
      "SELECT * FROM people", "SELECT id FROM people ORDER BY id",
      "SELECT id FROM people WHERE id = 1 OR id = 2", "SELECT count(id) FROM people",
      "SELECT NULL FROM people", "SELECT id FROM people JOIN other ON people.id = other.id",
      "INSERT INTO people (id) VALUES (1),(2)", "INSERT INTO people (id) VALUES (1); DELETE FROM people",
      "UPDATE people SET score = score + 1 WHERE id = 1", "CREATE TABLE people (id INT)",
      "SELECT id FROM people LIMIT 1 OFFSET -1", "INSERT INTO people (id) VALUES ('unterminated)",
      "INSERT INTO people (id, id) VALUES (1, 2)", "UPDATE people SET score=1, score=2 WHERE id=1",
      "SELECT id FROM people WHERE id = $1"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      input_sql(invalid[i]);
      check_not_equal(parse_input(), ORM_STATUS_OK);
      check_null(parsed.table);
      check_equal(vec_size(&parsed.assignments), 0u);
    }
  }
  it("rejects every truncated prefix of a single-row INSERT") {
    const char *text = "INSERT INTO people (id, name) VALUES (1, 'a''b')";
    for (size_t length = 1; length < strlen(text); ++length) {
      orm_plan_destroy(&raw); orm_plan_destroy(&parsed);
      const vstr prefix = {text, length};
      check_equal(orm_plan_init(&raw, ORM_QUERY_RAW, prefix, &limits, &error), ORM_STATUS_OK);
      check_not_equal(parse_input(), ORM_STATUS_OK);
      check_null(parsed.table);
    }
  }
  it("enforces query column predicate assignment and payload budgets") {
    input_sql("SELECT id FROM people"); limits.max_query_bytes = 4;
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
    limits.max_query_bytes = 1024;
    input_sql("SELECT id, score FROM people"); limits.max_columns = 1;
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
    input_sql("SELECT id FROM people WHERE id=1 AND score=2"); limits.max_predicates = 1;
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
    input_sql("UPDATE people SET id=1, score=2"); limits.max_assignments = 1;
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
    input_sql("INSERT INTO people (id) VALUES ('abcd')"); limits.max_parameter_bytes = 3;
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
  }
  it("rejects integer overflow and pagination outside the configured row budget") {
    input_sql("SELECT id FROM people LIMIT 101");
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
    input_sql("INSERT INTO people (id) VALUES (18446744073709551616)");
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
    input_sql("INSERT INTO people (id) VALUES (-9223372036854775809)");
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
  }
  it("rejects embedded NUL and non-finite bound numbers") {
    input_sql("SELECT id FROM people");
    raw.raw_sql[6] = '\0';
    check_equal(parse_input(), ORM_STATUS_SQL_ERROR);
    input_sql("INSERT INTO people (id) VALUES (?)"); bind_parameter(orm_f64(INFINITY));
    check_equal(parse_input(), ORM_STATUS_INVALID_ARGUMENT);
    input_sql("SELECT id FROM people LIMIT ?"); bind_parameter(orm_i64(-1));
    check_equal(parse_input(), ORM_STATUS_INVALID_ARGUMENT);
  }
  it("cleans the partial plan when TinyMock rejects literal allocation") {
    input_sql("INSERT INTO people (id, name) VALUES (1, 'text')");
    fail_literal = true;
    mock_literal_allocate_expect(TINYMOCk_ARG((const void *)NULL), TINYMOCk_ARG((size_t)4), TINYMOCk_RETURN((tstr)NULL));
    check_equal(parse_input(), ORM_STATUS_OUT_OF_MEMORY);
    check_null(parsed.table);
    check_equal(vec_size(&parsed.assignments), 0u);
    tinymock_mock_verify_times(&tinymock_literal_allocate, 1u);
  }

  it("preserves backslashes and doubled quotes after releasing the AST and raw SQL") {
    input_sql("INSERT INTO people (id, note, name) VALUES (1, 'C:\\', 'a\\n\\''b')");
    check_equal(parse_input(), ORM_STATUS_OK);
    orm_plan_destroy(&raw);
    const orm_assignment *a = vec_at_const(&parsed.assignments, 1);
    check_equal(a->value.bytes, "C:\\");
    a = vec_at_const(&parsed.assignments, 2);
    check_equal(a->value.bytes, "a\\n\\'b");
    input_sql("INSERT INTO people (id, note) VALUES (2, \"a\"\"b\\\")");
    check_equal(parse_input(), ORM_STATUS_OK);
    a = vec_at_const(&parsed.assignments, 1);
    check_equal(a->value.bytes, "a\"b\\");
  }

  it("lowers comments quoted ASCII names and grouped AND without discarding semantics") {
    input_sql("SELECT `id`, `score` FROM `people` /* scan */ WHERE (id >= ? AND (score < ? AND note IS NOT NULL)) -- end\n");
    bind_parameter(orm_i64(3)); bind_parameter(orm_i64(9));
    check_equal(parse_input(), ORM_STATUS_OK);
    check_equal(parsed.table, "people");
    check_equal(vec_size(&parsed.predicates), 3u);
    const orm_predicate *a = vec_at_const(&parsed.predicates, 0);
    check_equal(a->value.data.int64_value, 3);
    a = vec_at_const(&parsed.predicates, 1);
    check_equal(a->value.data.int64_value, 9);
    input_sql("INSERT INTO people (`select`) VALUES (1)");
    check_equal(parse_input(), ORM_STATUS_OK);
    const orm_assignment *assignment = vec_at_const(&parsed.assignments, 0);
    check_equal(assignment->column, "select");
  }

  it("binds comma LIMIT in source order and keeps zero count explicit") {
    input_sql("SELECT id FROM people WHERE id >= ? LIMIT ?, ?");
    bind_parameter(orm_i64(7)); bind_parameter(orm_i64(3)); bind_parameter(orm_i64(0));
    check_equal(parse_input(), ORM_STATUS_OK);
    check_equal(parsed.limit, 0u);
    check_equal(parsed.offset, 3u);
    check_true(parsed.has_limit); check_true(parsed.has_offset);
    const orm_predicate *a = vec_at_const(&parsed.predicates, 0);
    check_equal(a->value.data.int64_value, 7);
    input_sql("UPDATE people SET score=?, note=? WHERE id=? AND active=?");
    bind_parameter(orm_i64(11)); bind_parameter(orm_text("text"));
    bind_parameter(orm_i64(5)); bind_parameter(orm_bool(true));
    check_equal(parse_input(), ORM_STATUS_OK);
    const orm_assignment *assignment = vec_at_const(&parsed.assignments, 0);
    check_equal(assignment->value.data.int64_value, 11);
    a = vec_at_const(&parsed.predicates, 0);
    check_equal(a->value.data.int64_value, 5);
    a = vec_at_const(&parsed.predicates, 1);
    check_equal(a->value.data.boolean_value, 1);
  }

  it("rejects parsed clauses and modifiers that the execution plan cannot represent") {
    const char *unsupported[] = {
      "SELECT DISTINCT id FROM people", "SELECT SQL_CALC_FOUND_ROWS id FROM people",
      "SELECT id AS other FROM people", "SELECT id FROM people p",
      "SELECT id FROM people GROUP BY id", "SELECT id FROM people HAVING id=1",
      "SELECT id FROM people WINDOW w AS ()",
      "SELECT RANK() OVER w FROM people WINDOW w AS (ORDER BY id)",
      "SELECT id FROM people WHERE id <=> 1", "SELECT id FROM people WHERE id IN (1,2)",
      "SELECT id FROM people WHERE id BETWEEN 1 AND 2", "SELECT id FROM people WHERE NOT id=1",
      "SELECT id FROM people WHERE id=1 XOR id=2", "SELECT id FROM people WHERE note LIKE 'a%'",
      "SELECT id FROM people WHERE id=other", "SELECT people.id FROM people",
      "SELECT id FROM app.people", "SELECT id FROM people UNION SELECT id FROM people",
      "WITH q AS (SELECT id FROM people) SELECT id FROM q",
      "REPLACE INTO people (id) VALUES(1)", "INSERT LOW_PRIORITY INTO people (id) VALUES(1)",
      "INSERT INTO people SET id=1", "INSERT INTO people (id) SELECT id FROM people",
      "INSERT INTO people (id) VALUES(DEFAULT)", "INSERT INTO people (id) VALUES(1.5)",
      "INSERT INTO people (id) VALUES(0x41)", "INSERT INTO people (id) VALUES(@id)",
      "UPDATE LOW_PRIORITY people SET score=1 WHERE id=1",
      "UPDATE people SET score=1 WHERE id=1 LIMIT 1", "UPDATE people SET score=1 ORDER BY id",
      "DELETE LOW_PRIORITY FROM people WHERE id=1", "DELETE FROM people WHERE id=1 LIMIT 1",
      "DELETE p FROM people p WHERE p.id=1", "BEGIN", "SET sql_mode=''", "SHOW TABLES"
    };
    for (size_t i = 0; i < sizeof(unsupported)/sizeof(unsupported[0]); ++i) {
      info("SQL: %s", unsupported[i]);
      input_sql(unsupported[i]);
      check_equal(parse_input(), ORM_STATUS_UNSUPPORTED);
      check_null(parsed.table);
      check_equal(vec_size(&parsed.assignments), 0u);
      check_contains(error.message, "TidesDB SQL at byte");
    }
  }

  it("rejects mismatched row width and duplicate decoded identifiers") {
    const char *invalid[] = {
      "INSERT INTO people (id, score) VALUES(1)", "INSERT INTO people (id) VALUES(1,2)",
      "SELECT id, `id` FROM people", "UPDATE people SET id=1, `id`=2"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
      input_sql(invalid[i]);
      check_equal(parse_input(), ORM_STATUS_SQL_ERROR);
      check_null(parsed.table);
    }
    input_sql("SELECT id FROM people; DELETE FROM people WHERE id=1");
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
    check_null(parsed.table);
  }

  it("bounds iterative WHERE traversal and cleans up after allocation failure") {
    input_sql("SELECT id FROM people WHERE (id=? AND score=?) AND active=?");
    bind_parameter(orm_i64(1)); bind_parameter(orm_i64(2)); bind_parameter(orm_bool(true));
    fail_work = true;
    check_equal(parse_input(), ORM_STATUS_OUT_OF_MEMORY);
    check_null(parsed.table);
    fail_work = false;
    limits.max_predicates = 2;
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
    check_null(parsed.table);
    limits.max_predicates = 3;
    check_equal(parse_input(), ORM_STATUS_OK);
    check_equal(vec_size(&parsed.predicates), 3u);
  }

  it("lowers a deep AND tree within the predicate budget without C recursion") {
    enum { TERMS = 512, SQL_BYTES = 16384 };
    char sql[SQL_BYTES] = "SELECT id FROM people WHERE id=0";
    size_t used = strlen(sql);
    for (size_t i = 1; i < TERMS; ++i) {
      int added = snprintf(sql + used, sizeof(sql) - used, " AND id=%zu", i);
      check_true(added > 0 && (size_t)added < sizeof(sql) - used);
      used += (size_t)added;
    }
    limits.max_query_bytes = sizeof(sql);
    limits.max_predicates = TERMS;
    input_sql(sql);
    check_equal(parse_input(), ORM_STATUS_OK);
    check_equal(vec_size(&parsed.predicates), (size_t)TERMS);
    const orm_predicate *last = vec_at_const(&parsed.predicates, TERMS - 1);
    check_equal(last->value.data.int64_value, (int64_t)(TERMS - 1));
    orm_plan_destroy(&parsed);
    limits.max_predicates = TERMS - 1;
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
    check_null(parsed.table);
  }

  it("maps the public parser stack limit before publishing a plan") {
    enum { NESTING = 4096, SQL_BYTES = 16384 };
    char sql[SQL_BYTES] = "SELECT id FROM people WHERE ";
    size_t used = strlen(sql);
    memset(sql + used, '(', NESTING); used += NESTING;
    memcpy(sql + used, "id=1", 4); used += 4;
    memset(sql + used, ')', NESTING); used += NESTING;
    sql[used] = 0;
    limits.max_query_bytes = sizeof(sql);
    input_sql(sql);
    check_equal(parse_input(), ORM_STATUS_LIMIT_EXCEEDED);
    check_null(parsed.table);
    check_contains(error.message, "TidesDB SQL at byte");
  }
}
