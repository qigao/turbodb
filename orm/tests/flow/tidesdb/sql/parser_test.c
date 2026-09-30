#include "sql.h"
#include <tinymock.h>
#include <string.h>

TINYMOCk_MOCK(tstr, literal_allocate, const void *, size_t)
static bool fail_literal;
static tstr allocate_literal(const void *input, size_t size) {
  return fail_literal ? literal_allocate(input, size) : tstr_new_len(input, size);
}
#define tstr_new_len allocate_literal
#include "../../../../../drivers/tidesdb/sql/parser.c"
#undef tstr_new_len

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

spec("TidesDB bounded SQL parser") {
  before_each() {
    limits = (orm_limits){16, 16, 16, 16, 1024, 256, 100, 4096};
    raw = (orm_query_plan){0};
    parsed = (orm_query_plan){0};
    orm_error_init(&error);
    fail_literal = false;
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
      "SELECT id FROM people -- comment", "SELECT id FROM people WHERE id = $1"
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
}
