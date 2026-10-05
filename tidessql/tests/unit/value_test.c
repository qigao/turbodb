#include "value.h"
#include <tinytest.h>
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

enum { TEST_LIMIT = 1000000, SENTINEL = 73 };
static orm_tidesdb_sql_budget budget;
static turbodb_error_t error;

static void start_budget(uint64_t steps) {
  orm_sql_budget_limits limits = {0};
  for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i)
    limits.statement.value[i] = TEST_LIMIT;
  limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps;
  limits.transaction = (orm_sql_transaction_budget_amount){TEST_LIMIT, TEST_LIMIT, TEST_LIMIT};
  check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
}

static orm_sql_predicate bind_values(orm_sql_predicate_op op,
    turbodb_value_t left, const turbodb_value_t *right) {
  orm_sql_predicate predicate = {0};
  const orm_sql_type a = {left.kind, true};
  const orm_sql_type b = {right ? right->kind : TURBODB_VALUE_NULL, true};
  check_equal(orm_tidesdb_sql_predicate_bind(op, a, right ? &b : NULL,
                                            &predicate, &error), TURBODB_STATUS_OK);
  return predicate;
}

static turbodb_value_t binary(orm_sql_predicate_op op, turbodb_value_t a, turbodb_value_t b) {
  const orm_sql_predicate predicate = bind_values(op, a, &b);
  turbodb_value_t out = turbodb_i64(SENTINEL);
  check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &b, &budget, &out, &error), TURBODB_STATUS_OK);
  return out;
}

static turbodb_value_t unary_value(orm_sql_predicate_op op, turbodb_value_t a) {
  const orm_sql_predicate predicate = bind_values(op, a, NULL);
  turbodb_value_t out = turbodb_i64(SENTINEL);
  check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, NULL, &budget, &out, &error), TURBODB_STATUS_OK);
  return out;
}

static void expect_truth(turbodb_value_t value, int truth) {
  if (truth < 0) check_equal(value.kind, TURBODB_VALUE_NULL);
  else {
    check_equal(value.kind, TURBODB_VALUE_BOOLEAN);
    check_equal(value.data.boolean_value, truth);
  }
}

static orm_sql_arithmetic bind_arithmetic(orm_sql_arithmetic_op op,
    turbodb_value_t left, const turbodb_value_t *right) {
  orm_sql_arithmetic arithmetic = {0};
  const orm_sql_type a = {left.kind, true}, b = {right ? right->kind : TURBODB_VALUE_NULL, true};
  check_equal(orm_tidesdb_sql_arithmetic_bind(op, a, right ? &b : NULL, &arithmetic, &error), TURBODB_STATUS_OK);
  return arithmetic;
}

static turbodb_status_t bind_cast(turbodb_value_kind_t kind, bool nullable,
    turbodb_value_kind_t target, orm_sql_cast *cast) {
  const orm_sql_type source={kind,nullable};
  const orm_sql_cast_target cast_target=target==TURBODB_VALUE_INT64 ? ORM_SQL_CAST_SIGNED :
      target==TURBODB_VALUE_UINT64 ? ORM_SQL_CAST_UNSIGNED : ORM_SQL_CAST_DOUBLE;
  return orm_tidesdb_sql_cast_bind(source,cast_target,cast,&error);
}

spec("TidesDB relational scalar predicates") {
  before_each() { tdsql_error_init(&error); start_budget(TEST_LIMIT); }

  group("explicit numeric CAST") {
    it("rounds FLOAT into exact binary32 values carried by DOUBLE") {
      const struct { turbodb_value_t input; double expected; } cases[]={
        {turbodb_i64(16777217),16777216.0},{turbodb_i64(-16777217),-16777216.0},
        {turbodb_u64(UINT64_MAX),18446744073709551616.0},{turbodb_bool(true),1.0},
        {turbodb_text("0.1"),0.100000001490116119384765625},
        {turbodb_blob("16777217",sizeof("16777217")-1),16777216.0},
        {turbodb_f64((double)FLT_MAX),(double)FLT_MAX},
        {turbodb_f64(1.0+(double)FLT_EPSILON/2.0),1.0},
        {turbodb_f64(1.0+3.0*(double)FLT_EPSILON/2.0),1.0+2.0*(double)FLT_EPSILON}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        const orm_sql_type source={cases[i].input.kind,false};
        orm_sql_cast cast; orm_sql_cast_condition condition=ORM_SQL_CAST_COMPLEMENT;
        check_equal(orm_tidesdb_sql_cast_bind(source,ORM_SQL_CAST_FLOAT,&cast,&error),TURBODB_STATUS_OK);
        turbodb_value_t out=cases[i].input;
        check_equal(orm_tidesdb_sql_cast_eval(&cast,&out,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,TURBODB_VALUE_DOUBLE); check_equal(out.data.double_value,cases[i].expected);
        check_equal(condition,ORM_SQL_CAST_EXACT);
      }
      const orm_sql_type source={TURBODB_VALUE_DOUBLE,true}; orm_sql_cast cast;
      check_equal(orm_tidesdb_sql_cast_bind(source,ORM_SQL_CAST_FLOAT,&cast,&error),TURBODB_STATUS_OK);
      const double inputs[]={0.0,-0.0,(double)FLT_TRUE_MIN,-(double)FLT_TRUE_MIN,
        (double)FLT_TRUE_MIN/2.0,-(double)FLT_TRUE_MIN/2.0};
      const double expected[]={0.0,-0.0,(double)FLT_TRUE_MIN,-(double)FLT_TRUE_MIN,0.0,-0.0};
      for(size_t i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) {
        turbodb_value_t out=turbodb_f64(inputs[i]); orm_sql_cast_condition condition;
        check_equal(orm_tidesdb_sql_cast_eval(&cast,&out,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
        check_equal(out.data.double_value,expected[i]);
        check_equal(signbit(out.data.double_value)!=0,signbit(expected[i])!=0);
      }
      turbodb_value_t out=turbodb_null(); orm_sql_cast_condition condition;
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&out,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
      check_equal(out.kind,TURBODB_VALUE_NULL); check_equal(condition,ORM_SQL_CAST_EXACT);
    }
    it("rejects FLOAT range failures before rounding instead of clipping them") {
      const orm_sql_type source={TURBODB_VALUE_DOUBLE,false}; orm_sql_cast cast;
      check_equal(orm_tidesdb_sql_cast_bind(source,ORM_SQL_CAST_FLOAT,&cast,&error),TURBODB_STATUS_OK);
      const double inputs[]={nextafter((double)FLT_MAX,INFINITY),nextafter(-(double)FLT_MAX,-INFINITY),DBL_MAX,-DBL_MAX};
      for(size_t i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) {
        turbodb_value_t input=turbodb_f64(inputs[i]),out=turbodb_i64(SENTINEL);
        orm_sql_cast_condition condition=ORM_SQL_CAST_COMPLEMENT;
        check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_OUT_OF_RANGE);
        check_equal(out.data.int64_value,(int64_t)SENTINEL); check_equal(condition,ORM_SQL_CAST_COMPLEMENT);
      }
      const orm_sql_type text_source={TURBODB_VALUE_TEXT,false};
      check_equal(orm_tidesdb_sql_cast_bind(text_source,ORM_SQL_CAST_FLOAT,&cast,&error),TURBODB_STATUS_OK);
      turbodb_value_t input=turbodb_text("1e39"),out=turbodb_i64(SENTINEL); orm_sql_cast_condition condition=ORM_SQL_CAST_COMPLEMENT;
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_OUT_OF_RANGE);
      check_equal(out.data.int64_value,(int64_t)SENTINEL); check_equal(condition,ORM_SQL_CAST_COMPLEMENT);
      check_equal(orm_tidesdb_sql_cast_bind(text_source,ORM_SQL_CAST_DOUBLE,&cast,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
      check_equal(out.data.double_value,1e39);
    }
    it("validates FLOAT descriptors and byte admission before exposing a rounded result") {
      const orm_sql_type source={TURBODB_VALUE_INT64,false}; orm_sql_cast cast;
      check_equal(orm_tidesdb_sql_cast_bind(source,ORM_SQL_CAST_FLOAT,&cast,&error),TURBODB_STATUS_OK);
      cast.result.kind=TURBODB_VALUE_INT64;
      turbodb_value_t input=turbodb_i64(16777217),out=turbodb_i64(SENTINEL);
      orm_sql_cast_condition condition=ORM_SQL_CAST_COMPLEMENT;
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value,(int64_t)SENTINEL); check_equal(condition,ORM_SQL_CAST_COMPLEMENT);
      check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); start_budget(1);
      check_equal(orm_tidesdb_sql_cast_bind(source,ORM_SQL_CAST_FLOAT,&cast,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
      check_equal(out.data.double_value,16777216.0);
      out=turbodb_i64(SENTINEL); condition=ORM_SQL_CAST_COMPLEMENT;
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value,(int64_t)SENTINEL); check_equal(condition,ORM_SQL_CAST_COMPLEMENT);
    }
    it("uses exact integer prefixes and distinguishes truncation from complement warnings") {
      const struct { const char *text; turbodb_value_kind_t target; uint64_t bits; unsigned conditions; } cases[]={
        {"12.9e3",TURBODB_VALUE_INT64,12,ORM_SQL_CAST_TRUNCATED},
        {"  -2.5 ",TURBODB_VALUE_INT64,(uint64_t)-2,ORM_SQL_CAST_TRUNCATED},
        {"18446744073709551615",TURBODB_VALUE_UINT64,UINT64_MAX,ORM_SQL_CAST_EXACT},
        {"18446744073709551615",TURBODB_VALUE_INT64,UINT64_MAX,ORM_SQL_CAST_COMPLEMENT},
        {"-1",TURBODB_VALUE_UINT64,UINT64_MAX,ORM_SQL_CAST_COMPLEMENT},
        {"-0",TURBODB_VALUE_UINT64,0,ORM_SQL_CAST_COMPLEMENT},
        {"-1.9",TURBODB_VALUE_UINT64,UINT64_MAX,ORM_SQL_CAST_TRUNCATED|ORM_SQL_CAST_COMPLEMENT},
        {"9223372036854775808",TURBODB_VALUE_INT64,(uint64_t)INT64_MAX+1u,ORM_SQL_CAST_COMPLEMENT},
        {"-9223372036854775808",TURBODB_VALUE_INT64,(uint64_t)INT64_MAX+1u,ORM_SQL_CAST_EXACT},
        {"18446744073709551616",TURBODB_VALUE_INT64,UINT64_MAX,ORM_SQL_CAST_TRUNCATED},
        {"-9223372036854775809",TURBODB_VALUE_UINT64,(uint64_t)INT64_MAX+1u,ORM_SQL_CAST_TRUNCATED},
        {"+0000012 \t",TURBODB_VALUE_INT64,12,ORM_SQL_CAST_EXACT},
        {".5",TURBODB_VALUE_INT64,0,ORM_SQL_CAST_TRUNCATED},
        {"",TURBODB_VALUE_UINT64,0,ORM_SQL_CAST_TRUNCATED},
        {"-garbage",TURBODB_VALUE_UINT64,0,ORM_SQL_CAST_TRUNCATED}
      };
      for(size_t binary=0;binary<2;++binary) for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        turbodb_value_t input=binary ? turbodb_blob(cases[i].text,strlen(cases[i].text)) : turbodb_text(cases[i].text);
        orm_sql_cast cast; orm_sql_cast_condition condition=ORM_SQL_CAST_EXACT;
        check_equal(bind_cast(input.kind,true,cases[i].target,&cast),TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&input,&condition,&error),TURBODB_STATUS_OK);
        check_equal(input.kind,cases[i].target); check_equal((unsigned)condition,cases[i].conditions);
        check_equal(input.kind==TURBODB_VALUE_INT64 ? (uint64_t)input.data.int64_value : input.data.uint64_value,cases[i].bits);
      }
    }
    it("wraps numeric integer kinds without textual complement conditions") {
      const turbodb_value_t numbers[]={turbodb_i64(INT64_MIN),turbodb_i64(-1),turbodb_i64(0),turbodb_i64(INT64_MAX),turbodb_u64(UINT64_MAX)};
      for(size_t i=0;i<sizeof(numbers)/sizeof(numbers[0]);++i) {
        turbodb_value_t value=numbers[i]; orm_sql_cast cast; orm_sql_cast_condition condition;
        check_equal(bind_cast(value.kind,false,TURBODB_VALUE_UINT64,&cast),TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_cast_eval(&cast,&value,&budget,&value,&condition,&error),TURBODB_STATUS_OK);
        check_equal(condition,ORM_SQL_CAST_EXACT);
        check_equal(value.data.uint64_value,numbers[i].kind==TURBODB_VALUE_INT64 ? (uint64_t)numbers[i].data.int64_value : numbers[i].data.uint64_value);
        check_equal(bind_cast(value.kind,false,TURBODB_VALUE_INT64,&cast),TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_cast_eval(&cast,&value,&budget,&value,&condition,&error),TURBODB_STATUS_OK);
        check_equal(condition,ORM_SQL_CAST_EXACT);
        check_equal(value.data.int64_value,numbers[i].kind==TURBODB_VALUE_INT64 ? numbers[i].data.int64_value : (int64_t)-1);
      }
    }
    it("converts booleans NULL and finite real values using the profile real lane") {
      const struct { double input; int64_t expected; } cases[]={
        {2.5,2},{3.5,4},{-2.5,-2},{-3.5,-4},{-0.0,0},
        {DBL_MAX,INT64_MAX},{-DBL_MAX,INT64_MIN},{9007199254740992.0,INT64_C(9007199254740992)}
      };
      orm_sql_cast cast; orm_sql_cast_condition condition;
      check_equal(bind_cast(TURBODB_VALUE_DOUBLE,false,TURBODB_VALUE_INT64,&cast),TURBODB_STATUS_OK);
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        turbodb_value_t value=turbodb_f64(cases[i].input);
        check_equal(orm_tidesdb_sql_cast_eval(&cast,&value,&budget,&value,&condition,&error),TURBODB_STATUS_OK);
        check_equal(value.data.int64_value,cases[i].expected); check_equal(condition,ORM_SQL_CAST_EXACT);
      }
      check_equal(bind_cast(TURBODB_VALUE_BOOLEAN,true,TURBODB_VALUE_DOUBLE,&cast),TURBODB_STATUS_OK);
      turbodb_value_t value=turbodb_bool(true);
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&value,&budget,&value,&condition,&error),TURBODB_STATUS_OK);
      check_equal(value.data.double_value,1.0);
      value=turbodb_null();
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&value,&budget,&value,&condition,&error),TURBODB_STATUS_OK);
      check_equal(value.kind,TURBODB_VALUE_NULL); check_equal(condition,ORM_SQL_CAST_EXACT);
    }
    it("parses DOUBLE numeric prefixes but fails finite range overflow") {
      const struct { const char *text; double expected; unsigned condition; } cases[]={
        {"12.9e3",12900.0,ORM_SQL_CAST_EXACT},{"2.5junk",2.5,ORM_SQL_CAST_TRUNCATED},
        {"not numeric",0.0,ORM_SQL_CAST_TRUNCATED},{"18446744073709551616",18446744073709551616.0,ORM_SQL_CAST_EXACT}
      };
      orm_sql_cast cast;
      check_equal(bind_cast(TURBODB_VALUE_TEXT,false,TURBODB_VALUE_DOUBLE,&cast),TURBODB_STATUS_OK);
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        turbodb_value_t value=turbodb_text(cases[i].text); orm_sql_cast_condition condition;
        check_equal(orm_tidesdb_sql_cast_eval(&cast,&value,&budget,&value,&condition,&error),TURBODB_STATUS_OK);
        check_equal(value.kind,TURBODB_VALUE_DOUBLE); check_equal(value.data.double_value,cases[i].expected);
        check_equal((unsigned)condition,cases[i].condition);
      }
      turbodb_value_t input=turbodb_text("1e9999"),out=turbodb_i64(SENTINEL);
      orm_sql_cast_condition condition=ORM_SQL_CAST_COMPLEMENT;
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_OUT_OF_RANGE);
      check_equal(out.data.int64_value,(int64_t)SENTINEL); check_equal(condition,ORM_SQL_CAST_COMPLEMENT);
    }
    it("admits byte work before scanning and preserves outputs for malformed values or quotas") {
      check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); start_budget(3);
      orm_sql_cast cast; turbodb_value_t input=turbodb_text("12"),out=turbodb_i64(SENTINEL);
      orm_sql_cast_condition condition=ORM_SQL_CAST_COMPLEMENT;
      check_equal(bind_cast(TURBODB_VALUE_TEXT,false,TURBODB_VALUE_INT64,&cast),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
      check_equal(out.data.int64_value,(int64_t)12);
      out=turbodb_i64(SENTINEL); condition=ORM_SQL_CAST_COMPLEMENT;
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value,(int64_t)SENTINEL); check_equal(condition,ORM_SQL_CAST_COMPLEMENT);
      check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); start_budget(TEST_LIMIT);
      const char invalid[]={ -1 };
      input=(turbodb_value_t){.kind=TURBODB_VALUE_TEXT,.data.text_value={invalid,sizeof(invalid)}};
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_TYPE_ERROR);
      input=turbodb_null();
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(bind_cast(TURBODB_VALUE_DOUBLE,false,TURBODB_VALUE_INT64,&cast),TURBODB_STATUS_OK);
      input=turbodb_f64(INFINITY);
      check_equal(orm_tidesdb_sql_cast_eval(&cast,&input,&budget,&out,&condition,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value,(int64_t)SENTINEL); check_equal(condition,ORM_SQL_CAST_COMPLEMENT);
    }
  }

  group("implicit numeric result promotion") {
    it("promotes exact scalar inputs including precision boundaries and supports aliased output") {
      const struct { turbodb_value_t input; double expected; } cases[] = {
        {turbodb_i64(-7),-7.0},{turbodb_u64(7),7.0},{turbodb_f64(-0.0),-0.0},
        {turbodb_i64(INT64_C(9007199254740993)),9007199254740992.0},
        {turbodb_i64(INT64_MIN),-9223372036854775808.0},
        {turbodb_i64(INT64_MAX),9223372036854775808.0},
        {turbodb_u64(UINT64_MAX),18446744073709551616.0}
      };
      for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        turbodb_value_t value=cases[i].input;
        const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        check_equal(orm_tidesdb_sql_real_promote(&value,&budget,&value,&error),TURBODB_STATUS_OK);
        check_equal(value.kind,TURBODB_VALUE_DOUBLE);check_equal(value.data.double_value,cases[i].expected);
        check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps+1);
        if (i==2) check_true(signbit(value.data.double_value));
      }
      turbodb_value_t value=turbodb_null();
      check_equal(orm_tidesdb_sql_real_promote(&value,&budget,&value,&error),TURBODB_STATUS_OK);
      check_equal(value.kind,TURBODB_VALUE_NULL);check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
    it("rejects invalid shapes before charging and preserves output on exact step exhaustion") {
      turbodb_value_t reserved=turbodb_i64(7);reserved.reserved=1;
      const turbodb_value_t invalid[]={turbodb_bool(1),turbodb_text("7"),turbodb_blob("7",1),turbodb_f64(NAN),turbodb_f64(INFINITY),reserved};
      turbodb_value_t out=turbodb_i64(SENTINEL);
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        check_equal(orm_tidesdb_sql_real_promote(&invalid[i],&budget,&out,&error),TURBODB_STATUS_TYPE_ERROR);
        check_equal(out.kind,TURBODB_VALUE_INT64);check_equal(out.data.int64_value,SENTINEL);
        check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],0u);
      }
      const turbodb_value_t input=turbodb_u64(UINT64_MAX);
      check_equal(orm_tidesdb_sql_real_promote(NULL,&budget,&out,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_tidesdb_sql_real_promote(&input,&budget,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=0;
      check_equal(orm_tidesdb_sql_real_promote(&input,&budget,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value,SENTINEL);check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],0u);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
      check_equal(orm_tidesdb_sql_real_promote(&input,&budget,&out,&error),TURBODB_STATUS_OK);
      check_equal(out.kind,TURBODB_VALUE_DOUBLE);check_equal(out.data.double_value,18446744073709551616.0);
    }
  }

  group("division and modulo primitives") {
    it("truncates integer division toward zero and follows the numerator remainder sign") {
      const int64_t left[]={7,7,-7,-7,INT64_MIN,INT64_MAX};
      const int64_t right[]={3,-3,3,-3,-1,1};
      for(size_t i=0;i<sizeof(left)/sizeof(left[0]);++i) {
        const turbodb_value_t a=turbodb_i64(left[i]),b=turbodb_i64(right[i]); turbodb_value_t out=turbodb_i64(SENTINEL);
        const orm_sql_arithmetic mod=bind_arithmetic(ORM_SQL_MODULO,a,&b);
        check_equal(orm_tidesdb_sql_arithmetic_eval(&mod,&a,&b,&budget,&out,&error),TURBODB_STATUS_OK);
        check_equal(out.data.int64_value,i==4?0:left[i]%right[i]);
        const orm_sql_arithmetic div=bind_arithmetic(ORM_SQL_INTEGER_DIVIDE,a,&b); check_true(div.result.nullable);
        check_equal(orm_tidesdb_sql_arithmetic_eval(&div,&a,&b,&budget,&out,&error),i==4?TURBODB_STATUS_LIMIT_EXCEEDED:TURBODB_STATUS_OK);
        if(i!=4) check_equal(out.data.int64_value,left[i]/right[i]);
      }
    }
    it("preserves unsigned precision and mixed-integer result signedness") {
      const struct { turbodb_value_t a,b; orm_sql_arithmetic_op op; turbodb_value_t expected; turbodb_status_t status; } cases[]={
        {turbodb_u64(UINT64_MAX),turbodb_u64(1),ORM_SQL_INTEGER_DIVIDE,turbodb_u64(UINT64_MAX),TURBODB_STATUS_OK},
        {turbodb_u64(UINT64_MAX),turbodb_i64(2),ORM_SQL_INTEGER_DIVIDE,turbodb_u64(INT64_MAX),TURBODB_STATUS_OK},
        {turbodb_i64(-7),turbodb_u64(3),ORM_SQL_MODULO,turbodb_i64(-1),TURBODB_STATUS_OK},
        {turbodb_u64(UINT64_MAX),turbodb_i64(-3),ORM_SQL_MODULO,turbodb_u64(0),TURBODB_STATUS_OK},
        {turbodb_i64(INT64_MIN),turbodb_u64(UINT64_MAX),ORM_SQL_MODULO,turbodb_i64(INT64_MIN),TURBODB_STATUS_OK},
        {turbodb_i64(-7),turbodb_u64(3),ORM_SQL_INTEGER_DIVIDE,turbodb_null(),TURBODB_STATUS_LIMIT_EXCEEDED},
        {turbodb_i64(-1),turbodb_u64(3),ORM_SQL_INTEGER_DIVIDE,turbodb_u64(0),TURBODB_STATUS_OK},
        {turbodb_i64(INT64_MIN),turbodb_i64(1),ORM_SQL_INTEGER_DIVIDE,turbodb_null(),TURBODB_STATUS_LIMIT_EXCEEDED}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        const orm_sql_arithmetic fn=bind_arithmetic(cases[i].op,cases[i].a,&cases[i].b); turbodb_value_t out=turbodb_i64(SENTINEL);
        check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&cases[i].a,&cases[i].b,&budget,&out,&error),cases[i].status);
        if(cases[i].status!=TURBODB_STATUS_OK) { check_equal(out.data.int64_value,SENTINEL); continue; }
        check_equal(out.kind,cases[i].expected.kind);
        if(out.kind==TURBODB_VALUE_UINT64) check_equal(out.data.uint64_value,cases[i].expected.data.uint64_value);
        else check_equal(out.data.int64_value,cases[i].expected.data.int64_value);
      }
    }
    it("reports zero separately from NULL and preserves legacy primitive output on zero") {
      const orm_sql_arithmetic_op ops[]={ORM_SQL_INTEGER_DIVIDE,ORM_SQL_MODULO,ORM_SQL_DIVIDE};
      for(size_t i=0;i<sizeof(ops)/sizeof(ops[0]);++i) {
        turbodb_value_t a=i==2?turbodb_f64(7.0):turbodb_i64(7),b=i==2?turbodb_f64(-0.0):turbodb_i64(0),out=turbodb_i64(SENTINEL);
        const orm_sql_arithmetic fn=bind_arithmetic(ops[i],a,&b); orm_sql_numeric_condition condition=ORM_SQL_NUMERIC_EXACT;
        check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&fn,&a,&b,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,TURBODB_VALUE_NULL); check_equal(condition,ORM_SQL_NUMERIC_DIVISION_BY_ZERO);
        out=turbodb_i64(SENTINEL); check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&a,&b,&budget,&out,&error),TURBODB_STATUS_SQL_ERROR);
        check_equal(out.data.int64_value,SENTINEL);
        a=turbodb_null(); check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&fn,&a,&b,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,TURBODB_VALUE_NULL); check_equal(condition,ORM_SQL_NUMERIC_EXACT);
      }
    }
    it("evaluates finite DOUBLE division and modulo while checking nonfinite results") {
      const turbodb_value_t a=turbodb_f64(-7.5),b=turbodb_f64(2.0); turbodb_value_t out=turbodb_i64(SENTINEL);
      const orm_sql_arithmetic div=bind_arithmetic(ORM_SQL_DIVIDE,a,&b),mod=bind_arithmetic(ORM_SQL_MODULO,a,&b);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&div,&a,&b,&budget,&out,&error),TURBODB_STATUS_OK); check_equal(out.data.double_value,-3.75);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&mod,&a,&b,&budget,&out,&error),TURBODB_STATUS_OK); check_equal(out.data.double_value,-1.5);
      const turbodb_value_t large=turbodb_f64(DBL_MAX),small=turbodb_f64(DBL_MIN); out=turbodb_i64(SENTINEL);
      orm_sql_numeric_condition condition=ORM_SQL_NUMERIC_DIVISION_BY_ZERO;
      check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&div,&large,&small,&budget,&out,&condition,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value,SENTINEL); check_equal(condition,ORM_SQL_NUMERIC_DIVISION_BY_ZERO);
      const turbodb_value_t invalid=turbodb_f64(INFINITY); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(orm_tidesdb_sql_arithmetic_eval(&div,&invalid,&b,&budget,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
    }
    it("rejects missing DECIMAL and coercion while charging zero-division steps exactly") {
      orm_sql_arithmetic fn={0}; const orm_sql_type integer={TURBODB_VALUE_INT64,false},real={TURBODB_VALUE_DOUBLE,false},boolean={TURBODB_VALUE_BOOLEAN,false};
      check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_DIVIDE,integer,&integer,&fn,&error),TURBODB_STATUS_UNSUPPORTED);
      check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_INTEGER_DIVIDE,real,&real,&fn,&error),TURBODB_STATUS_UNSUPPORTED);
      check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_INTEGER_DIVIDE,integer,&real,&fn,&error),TURBODB_STATUS_UNSUPPORTED);
      check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_MODULO,boolean,&integer,&fn,&error),TURBODB_STATUS_UNSUPPORTED);
      check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); start_budget(1);
      turbodb_value_t a=turbodb_i64(7),b=turbodb_i64(0); fn=bind_arithmetic(ORM_SQL_MODULO,a,&b);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&a,&b,&budget,&a,&error),TURBODB_STATUS_SQL_ERROR); check_equal(a.data.int64_value,7);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],1u); b=turbodb_i64(2);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&a,&b,&budget,&a,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(a.data.int64_value,7);
    }
  }

  group("mixed real arithmetic") {
    it("promotes signed and unsigned peers for every real binary operator in both orders") {
      const orm_sql_arithmetic_op ops[]={ORM_SQL_ADD,ORM_SQL_SUBTRACT,ORM_SQL_MULTIPLY,ORM_SQL_DIVIDE,ORM_SQL_MODULO};
      const double expected[]={9.0,5.0,14.0,3.5,1.0};
      for(size_t kind=0;kind<2;++kind) for(size_t order=0;order<2;++order) for(size_t op=0;op<sizeof(ops)/sizeof(ops[0]);++op) {
        const turbodb_value_t a=order?turbodb_f64(7.0):kind?turbodb_u64(7):turbodb_i64(7);
        const turbodb_value_t b=order?(kind?turbodb_u64(2):turbodb_i64(2)):turbodb_f64(2.0);
        const orm_sql_arithmetic fn=bind_arithmetic(ops[op],a,&b);
        check_equal(fn.left.kind,a.kind); check_equal(fn.right.kind,b.kind); check_equal(fn.result.kind,TURBODB_VALUE_DOUBLE);
        turbodb_value_t out=turbodb_i64(SENTINEL);
        check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&a,&b,&budget,&out,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,TURBODB_VALUE_DOUBLE); check_equal(out.data.double_value,expected[op]);
      }
    }
    it("uses real rounding beyond exact integer precision and preserves modulo signs") {
      const struct {turbodb_value_t a,b; orm_sql_arithmetic_op op; double result;} cases[]={
        {turbodb_u64(UINT64_MAX),turbodb_f64(2.0),ORM_SQL_MODULO,0.0},
        {turbodb_u64(UINT64_MAX),turbodb_f64(1.0),ORM_SQL_ADD,18446744073709551616.0},
        {turbodb_i64(INT64_MAX),turbodb_f64(1.0),ORM_SQL_SUBTRACT,9223372036854775808.0},
        {turbodb_i64(INT64_MIN),turbodb_f64(-1.0),ORM_SQL_MULTIPLY,9223372036854775808.0},
        {turbodb_i64(-7),turbodb_f64(2.5),ORM_SQL_MODULO,-2.0},
        {turbodb_f64(-7.5),turbodb_i64(-2),ORM_SQL_MODULO,-1.5},
        {turbodb_i64(-7),turbodb_f64(2.0),ORM_SQL_DIVIDE,-3.5}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        const orm_sql_arithmetic fn=bind_arithmetic(cases[i].op,cases[i].a,&cases[i].b); turbodb_value_t out=turbodb_i64(SENTINEL);
        check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&cases[i].a,&cases[i].b,&budget,&out,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,TURBODB_VALUE_DOUBLE); check_equal(out.data.double_value,cases[i].result);
      }
    }
    it("keeps zero conditions and runtime NULL independent of mixed numeric types") {
      const orm_sql_arithmetic_op ops[]={ORM_SQL_DIVIDE,ORM_SQL_MODULO};
      for(size_t order=0;order<2;++order) for(size_t op=0;op<sizeof(ops)/sizeof(ops[0]);++op) {
        turbodb_value_t a=order?turbodb_f64(7.5):turbodb_i64(7),b=order?turbodb_i64(0):turbodb_f64(-0.0),out=turbodb_i64(SENTINEL);
        const orm_sql_arithmetic fn=bind_arithmetic(ops[op],a,&b);
        orm_sql_numeric_condition condition=ORM_SQL_NUMERIC_EXACT;
        check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&fn,&a,&b,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,TURBODB_VALUE_NULL); check_equal(condition,ORM_SQL_NUMERIC_DIVISION_BY_ZERO);
        a=turbodb_null(); out=turbodb_i64(SENTINEL);
        check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&fn,&a,&b,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,TURBODB_VALUE_NULL); check_equal(condition,ORM_SQL_NUMERIC_EXACT);
        a=order?turbodb_f64(7.5):turbodb_i64(7); b=turbodb_null();
        check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&fn,&a,&b,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,TURBODB_VALUE_NULL); check_equal(condition,ORM_SQL_NUMERIC_EXACT);
      }
    }
    it("rejects invalid operand types before charging and preserves aliased overflow outputs") {
      turbodb_value_t a=turbodb_i64(2),b=turbodb_f64(DBL_MAX),out=turbodb_i64(SENTINEL);
      const orm_sql_arithmetic fn=bind_arithmetic(ORM_SQL_MULTIPLY,a,&b);
      const turbodb_value_t invalid[]={turbodb_f64(2.0),turbodb_u64(2),turbodb_bool(1)};
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&invalid[i],&b,&budget,&out,&error),TURBODB_STATUS_TYPE_ERROR);
        check_equal(out.data.int64_value,SENTINEL); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
      }
      b=turbodb_f64(INFINITY); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&a,&b,&budget,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
      b=turbodb_f64(DBL_MAX); check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&a,&b,&budget,&b,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(b.kind,TURBODB_VALUE_DOUBLE); check_equal(b.data.double_value,DBL_MAX);
      check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); start_budget(1);
      b=turbodb_f64(1.5); check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&a,&b,&budget,&a,&error),TURBODB_STATUS_OK);
      check_equal(a.kind,TURBODB_VALUE_DOUBLE); check_equal(a.data.double_value,3.0);
      a=turbodb_i64(2); check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&a,&b,&budget,&a,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(a.kind,TURBODB_VALUE_INT64); check_equal(a.data.int64_value,2);
    }
  }

  group("decimal-place numeric rounding") {
    it("keeps exact integer kinds and checks rounding carries at both 64-bit limits") {
      const struct { turbodb_value_t input; int64_t digits; turbodb_value_t rounded,truncated; bool overflow; } cases[]={
        {turbodb_i64(25),-1,turbodb_i64(30),turbodb_i64(20),false},
        {turbodb_i64(-25),-1,turbodb_i64(-30),turbodb_i64(-20),false},
        {turbodb_i64(12345),-2,turbodb_i64(12300),turbodb_i64(12300),false},
        {turbodb_i64(-12350),-2,turbodb_i64(-12400),turbodb_i64(-12300),false},
        {turbodb_i64(INT64_MAX),-1,turbodb_null(),turbodb_i64(INT64_C(9223372036854775800)),true},
        {turbodb_i64(INT64_MIN),-1,turbodb_null(),turbodb_i64(-INT64_C(9223372036854775800)),true},
        {turbodb_i64(INT64_MIN),-2,turbodb_i64(-INT64_C(9223372036854775800)),turbodb_i64(-INT64_C(9223372036854775800)),false},
        {turbodb_i64(INT64_MIN),-19,turbodb_null(),turbodb_i64(0),true},
        {turbodb_u64(UINT64_MAX),-1,turbodb_null(),turbodb_u64(UINT64_C(18446744073709551610)),true},
        {turbodb_u64(UINT64_MAX),-2,turbodb_u64(UINT64_C(18446744073709551600)),turbodb_u64(UINT64_C(18446744073709551600)),false},
        {turbodb_u64(UINT64_MAX),-19,turbodb_null(),turbodb_u64(UINT64_C(10000000000000000000)),true},
        {turbodb_u64(UINT64_C(9999999999999999999)),-19,turbodb_u64(UINT64_C(10000000000000000000)),turbodb_u64(0),false},
        {turbodb_u64(UINT64_MAX),-20,turbodb_u64(0),turbodb_u64(0),false},
        {turbodb_i64(INT64_MIN),INT64_MIN,turbodb_i64(0),turbodb_i64(0),false}
      };
      const orm_sql_arithmetic_op ops[]={ORM_SQL_ROUND,ORM_SQL_TRUNCATE};
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) for(size_t op=0;op<sizeof(ops)/sizeof(ops[0]);++op) {
        const turbodb_value_t digits=turbodb_i64(cases[i].digits); const orm_sql_arithmetic fn=bind_arithmetic(ops[op],cases[i].input,&digits);
        turbodb_value_t out=turbodb_i64(SENTINEL); const bool failed=op==0 && cases[i].overflow;
        check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&cases[i].input,&digits,&budget,&out,&error),failed?TURBODB_STATUS_LIMIT_EXCEEDED:TURBODB_STATUS_OK);
        if(failed) { check_equal(out.data.int64_value,SENTINEL); continue; }
        const turbodb_value_t expected=op==0?cases[i].rounded:cases[i].truncated;
        check_equal(out.kind,expected.kind);
        if(expected.kind==TURBODB_VALUE_UINT64) check_equal(out.data.uint64_value,expected.data.uint64_value);
        else check_equal(out.data.int64_value,expected.data.int64_value);
      }
      const turbodb_value_t inputs[]={turbodb_i64(INT64_MIN),turbodb_i64(INT64_MAX),turbodb_u64(UINT64_MAX)};
      const turbodb_value_t digits[]={turbodb_i64(0),turbodb_i64(INT64_MAX),turbodb_u64(UINT64_MAX)};
      for(size_t i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) for(size_t p=0;p<sizeof(digits)/sizeof(digits[0]);++p)
        for(size_t op=0;op<sizeof(ops)/sizeof(ops[0]);++op) {
          const orm_sql_arithmetic fn=bind_arithmetic(ops[op],inputs[i],&digits[p]); turbodb_value_t out=inputs[i];
          check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&out,&digits[p],&budget,&out,&error),TURBODB_STATUS_OK);
          check_equal(out.kind,inputs[i].kind);
          if(out.kind==TURBODB_VALUE_UINT64) check_equal(out.data.uint64_value,inputs[i].data.uint64_value);
          else check_equal(out.data.int64_value,inputs[i].data.int64_value);
        }
    }
    it("uses approximate ties and truncation with finite scaling signed zero and extreme precision") {
      const struct { double input; int64_t digits; double rounded,truncated; bool overflow; } cases[]={
        {2.5,0,2.0,2.0,false},{3.5,0,4.0,3.0,false},{-3.5,0,-4.0,-3.0,false},
        {25.0,-1,20.0,20.0,false},{35.0,-1,40.0,30.0,false},
        {1.375,2,1.38,1.37,false},{-1.375,2,-1.38,-1.37,false},
        {3.125,2,3.12,3.12,false},{1e100,-100,1e100,1e100,false},
        {DBL_MAX,1,DBL_MAX,DBL_MAX,false},{-DBL_MAX,INT64_MAX,-DBL_MAX,-DBL_MAX,false},
        {DBL_MAX,-308,0.0,1e308,true},{-DBL_MAX,-308,0.0,-1e308,true},
        {-0.0,2,-0.0,-0.0,false},{-DBL_TRUE_MIN,308,-0.0,-0.0,false},
        {-DBL_MAX,-309,0.0,0.0,false},{DBL_MAX,INT64_MIN,0.0,0.0,false}
      };
      const orm_sql_arithmetic_op ops[]={ORM_SQL_ROUND,ORM_SQL_TRUNCATE};
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) for(size_t op=0;op<sizeof(ops)/sizeof(ops[0]);++op) {
        turbodb_value_t value=turbodb_f64(cases[i].input),precision=turbodb_i64(cases[i].digits),out=turbodb_i64(SENTINEL);
        const orm_sql_arithmetic fn=bind_arithmetic(ops[op],value,&precision); const bool failed=op==0&&cases[i].overflow;
        check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&value,&precision,&budget,&out,&error),failed?TURBODB_STATUS_LIMIT_EXCEEDED:TURBODB_STATUS_OK);
        if(failed) { check_equal(out.data.int64_value,SENTINEL); continue; }
        const double expected=op==0?cases[i].rounded:cases[i].truncated;
        check_equal(out.kind,TURBODB_VALUE_DOUBLE); check_equal(out.data.double_value,expected);
        if(expected==0.0) check_equal(signbit(out.data.double_value)!=0,signbit(expected)!=0);
      }
      const turbodb_value_t value=turbodb_f64(-0.0),precision=turbodb_u64(UINT64_MAX); turbodb_value_t out=turbodb_null();
      const orm_sql_arithmetic fn=bind_arithmetic(ORM_SQL_ROUND,value,&precision);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&value,&precision,&budget,&out,&error),TURBODB_STATUS_OK);
      check_true(signbit(out.data.double_value)!=0);
    }
    it("binds numeric results independently of precision kind and rejects unsupported coercions before execution") {
      const orm_sql_arithmetic_op ops[]={ORM_SQL_ROUND,ORM_SQL_TRUNCATE};
      for(size_t op=0;op<sizeof(ops)/sizeof(ops[0]);++op) for(int a=TURBODB_VALUE_NULL;a<=TURBODB_VALUE_BLOB;++a)
        for(int b=TURBODB_VALUE_NULL;b<=TURBODB_VALUE_BLOB;++b) {
          const orm_sql_type left={a,true},right={b,true}; orm_sql_arithmetic fn={.op=ORM_SQL_NEGATE};
          const bool supported=(a==TURBODB_VALUE_NULL||a==TURBODB_VALUE_INT64||a==TURBODB_VALUE_UINT64||a==TURBODB_VALUE_DOUBLE)&&
              (b==TURBODB_VALUE_NULL||b==TURBODB_VALUE_INT64||b==TURBODB_VALUE_UINT64);
          check_equal(orm_tidesdb_sql_arithmetic_bind(ops[op],left,&right,&fn,&error),supported?TURBODB_STATUS_OK:TURBODB_STATUS_UNSUPPORTED);
          if(supported) { check_equal(fn.result.kind,a); check_true(fn.result.nullable); }
          else check_equal(fn.op,ORM_SQL_NEGATE);
        }
      const orm_sql_type left={TURBODB_VALUE_DOUBLE,false},right={TURBODB_VALUE_INT64,false}; orm_sql_arithmetic fn;
      check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_ROUND,left,&right,&fn,&error),TURBODB_STATUS_OK); check_false(fn.result.nullable);
      check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_ROUND,left,NULL,&fn,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    }
    it("propagates NULL validates both inputs and preserves outputs and conditions at the step boundary") {
      turbodb_value_t value=turbodb_f64(1.375),precision=turbodb_i64(2),out=turbodb_i64(SENTINEL);
      const orm_sql_arithmetic fn=bind_arithmetic(ORM_SQL_ROUND,value,&precision); orm_sql_numeric_condition condition=ORM_SQL_NUMERIC_DIVISION_BY_ZERO;
      const turbodb_value_t null=turbodb_null();
      check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&fn,&null,&precision,&budget,&out,&condition,&error),TURBODB_STATUS_OK);
      check_equal(out.kind,TURBODB_VALUE_NULL); check_equal(condition,ORM_SQL_NUMERIC_EXACT);
      check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&fn,&value,&null,&budget,&out,&condition,&error),TURBODB_STATUS_OK); check_equal(out.kind,TURBODB_VALUE_NULL);
      const turbodb_value_t invalid=turbodb_f64(INFINITY); out=turbodb_i64(SENTINEL); const uint64_t before=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&fn,&invalid,&null,&budget,&out,&condition,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value,SENTINEL); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],before);
      check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); start_budget(1);
      check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&fn,&value,&precision,&budget,&precision,&condition,&error),TURBODB_STATUS_OK);
      check_equal(precision.kind,TURBODB_VALUE_DOUBLE); check_equal(precision.data.double_value,1.38);
      precision=turbodb_i64(2); out=turbodb_i64(SENTINEL); condition=ORM_SQL_NUMERIC_DIVISION_BY_ZERO;
      check_equal(orm_tidesdb_sql_arithmetic_eval_condition(&fn,&value,&precision,&budget,&out,&condition,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value,SENTINEL); check_equal(condition,ORM_SQL_NUMERIC_DIVISION_BY_ZERO);
    }
  }

  group("single argument numeric functions") {
    it("keeps integer precision and returns a signed integer for SIGN") {
      const turbodb_value_t inputs[]={turbodb_i64(INT64_MIN),turbodb_i64(-7),turbodb_i64(0),turbodb_i64(INT64_MAX),turbodb_u64(0),turbodb_u64(UINT64_MAX)};
      const orm_sql_arithmetic_op ops[]={ORM_SQL_ABSOLUTE,ORM_SQL_SIGN,ORM_SQL_FLOOR,ORM_SQL_CEIL};
      for(size_t op=0;op<sizeof(ops)/sizeof(ops[0]);++op) for(size_t i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) {
        const orm_sql_arithmetic fn=bind_arithmetic(ops[op],inputs[i],NULL); turbodb_value_t out=turbodb_i64(SENTINEL);
        const bool overflow=ops[op]==ORM_SQL_ABSOLUTE && i==0;
        check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&inputs[i],NULL,&budget,&out,&error),overflow?TURBODB_STATUS_LIMIT_EXCEEDED:TURBODB_STATUS_OK);
        if(overflow) { check_equal(out.data.int64_value,SENTINEL); continue; }
        check_equal(out.kind,ops[op]==ORM_SQL_SIGN?TURBODB_VALUE_INT64:inputs[i].kind);
        if(ops[op]==ORM_SQL_SIGN) check_equal(out.data.int64_value,i<2?-1:i==2||i==4?0:1);
        else if(inputs[i].kind==TURBODB_VALUE_UINT64) check_equal(out.data.uint64_value,inputs[i].data.uint64_value);
        else check_equal(out.data.int64_value,ops[op]==ORM_SQL_ABSOLUTE && i==1?7:inputs[i].data.int64_value);
      }
    }
    it("handles finite DOUBLE extremes fractions and signed zeros without integer conversion") {
      const double inputs[]={-DBL_MAX,-1.25,-DBL_MIN,-0.0,0.0,DBL_MIN,1.25,DBL_MAX};
      const double floored[]={-DBL_MAX,-2.0,-1.0,-0.0,0.0,0.0,1.0,DBL_MAX};
      const double ceiling[]={-DBL_MAX,-1.0,-0.0,-0.0,0.0,1.0,2.0,DBL_MAX};
      const orm_sql_arithmetic_op ops[]={ORM_SQL_ABSOLUTE,ORM_SQL_SIGN,ORM_SQL_FLOOR,ORM_SQL_CEIL};
      for(size_t op=0;op<sizeof(ops)/sizeof(ops[0]);++op) for(size_t i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) {
        turbodb_value_t value=turbodb_f64(inputs[i]); const orm_sql_arithmetic fn=bind_arithmetic(ops[op],value,NULL);
        check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&value,NULL,&budget,&value,&error),TURBODB_STATUS_OK);
        check_equal(value.kind,ops[op]==ORM_SQL_SIGN?TURBODB_VALUE_INT64:TURBODB_VALUE_DOUBLE);
        if(ops[op]==ORM_SQL_SIGN) check_equal(value.data.int64_value,i<3?-1:i<5?0:1);
        else { const double expected=ops[op]==ORM_SQL_ABSOLUTE?(inputs[i]<0?-inputs[i]:inputs[i]):ops[op]==ORM_SQL_FLOOR?floored[i]:ceiling[i];
          check_equal(value.data.double_value,expected); if(value.data.double_value==0.0) check_equal(signbit(value.data.double_value)!=0,ops[op]!=ORM_SQL_ABSOLUTE && i<4); }
      }
    }
    it("propagates NULL while rejecting coercion invalid arity and nonfinite inputs") {
      const orm_sql_arithmetic_op ops[]={ORM_SQL_ABSOLUTE,ORM_SQL_SIGN,ORM_SQL_FLOOR,ORM_SQL_CEIL};
      for(size_t i=0;i<sizeof(ops)/sizeof(ops[0]);++i) {
        const orm_sql_type type={TURBODB_VALUE_DOUBLE,true},bad={TURBODB_VALUE_TEXT,false}; orm_sql_arithmetic fn;
        check_equal(orm_tidesdb_sql_arithmetic_bind(ops[i],type,NULL,&fn,&error),TURBODB_STATUS_OK); check_true(fn.result.nullable);
        turbodb_value_t value=turbodb_null(),out=turbodb_i64(SENTINEL); check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&value,NULL,&budget,&out,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,TURBODB_VALUE_NULL); const orm_sql_arithmetic previous=fn;
        check_equal(orm_tidesdb_sql_arithmetic_bind(ops[i],type,&type,&fn,&error),TURBODB_STATUS_INVALID_ARGUMENT);
        check_equal(orm_tidesdb_sql_arithmetic_bind(ops[i],bad,NULL,&fn,&error),TURBODB_STATUS_UNSUPPORTED); check_equal(fn.result.kind,previous.result.kind);
        const turbodb_value_t invalid[]={turbodb_f64(INFINITY),turbodb_f64(-INFINITY),turbodb_f64(NAN),turbodb_bool(1)};
        for(size_t j=0;j<sizeof(invalid)/sizeof(invalid[0]);++j) { out=turbodb_i64(SENTINEL); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
          check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&invalid[j],NULL,&budget,&out,&error),TURBODB_STATUS_TYPE_ERROR);
          check_equal(out.data.int64_value,SENTINEL); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps); }
        fn=bind_arithmetic(ops[i],turbodb_null(),NULL); check_equal(fn.result.kind,TURBODB_VALUE_NULL);
      }
    }
    it("charges one step and preserves aliased inputs on range or budget failure") {
      check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); start_budget(1);
      turbodb_value_t value=turbodb_i64(INT64_MIN); const orm_sql_arithmetic fn=bind_arithmetic(ORM_SQL_ABSOLUTE,value,NULL);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&value,NULL,&budget,&value,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(value.data.int64_value,INT64_MIN); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],1u);
      value=turbodb_i64(-1); check_equal(orm_tidesdb_sql_arithmetic_eval(&fn,&value,NULL,&budget,&value,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(value.data.int64_value,-1); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],1u);
    }
  }
  it("preserves subnormal decimal literals without intermediate scale underflow") {
    const struct { const char *text; double expected; } samples[] = {
      {"4.9406564584124654e-324", DBL_TRUE_MIN}, {"0.49406564584124654e-323", DBL_TRUE_MIN},
      {"49406564584124654e-340", DBL_TRUE_MIN}, {"9.8813129168249309e-324", DBL_TRUE_MIN * 2.0},
      {"1.0e-320", 1.0e-320}, {"1e-1000000", 0.0}};
    for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) for (unsigned negative = 0; negative < 2; ++negative) {
      turbodb_value_t output = turbodb_i64(7);
      check_equal(orm_tidesdb_sql_number_literal(vstr_from_cstr(samples[i].text), negative != 0, &output, &error), TURBODB_STATUS_OK);
      check_equal(output.kind, TURBODB_VALUE_DOUBLE);
      check_true(output.data.double_value == (negative ? -samples[i].expected : samples[i].expected));
      if (samples[i].expected == 0.0) check_equal(signbit(output.data.double_value) != 0, negative != 0);
    }
  }
  it("converts bounded decimal literals without locale dependent parsing") {
    const struct { const char *text; bool negative; double expected; } cases[] = {
      {"1.5", false, 1.5}, {".000001", false, 0.000001},
      {"250e-2", false, 2.5}, {"0.0", true, -0.0},
      {"1e-1000000", false, 0.0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      turbodb_value_t out = turbodb_i64(SENTINEL);
      const turbodb_status_t status = orm_tidesdb_sql_number_literal(
          (vstr){cases[i].text,strlen(cases[i].text)},cases[i].negative,
          &out,&error);
      check_equal(status,TURBODB_STATUS_OK);
      check_equal(out.kind,TURBODB_VALUE_DOUBLE);
      check_equal(out.data.double_value,cases[i].expected);
      if (cases[i].negative && cases[i].expected == 0.0)
        check_true(signbit(out.data.double_value));
    }
    turbodb_value_t out = turbodb_i64(SENTINEL);
    const turbodb_status_t status = orm_tidesdb_sql_number_literal(
        (vstr){"1e1000000",9},false,&out,&error);
    check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.kind,TURBODB_VALUE_INT64);
    check_equal(out.data.int64_value,SENTINEL);
  }

  it("converts MySQL numeric column assignments in strict mode") {
    const struct {
      orm_sql_type target;
      turbodb_value_t input;
      turbodb_value_kind_t kind;
      int64_t integer;
      double floating;
      orm_sql_assignment_adjustment adjustment;
    } cases[] = {
      {{TURBODB_VALUE_INT64,false},turbodb_u64(7),TURBODB_VALUE_INT64,7,0,
        ORM_SQL_ASSIGNMENT_EXACT},
      {{TURBODB_VALUE_UINT64,false},turbodb_i64(7),TURBODB_VALUE_UINT64,7,0,
        ORM_SQL_ASSIGNMENT_EXACT},
      {{TURBODB_VALUE_DOUBLE,false},turbodb_i64(-7),TURBODB_VALUE_DOUBLE,0,-7.0,
        ORM_SQL_ASSIGNMENT_EXACT},
      {{TURBODB_VALUE_INT64,false},turbodb_f64(2.5),TURBODB_VALUE_INT64,3,0,
        ORM_SQL_ASSIGNMENT_ROUNDED},
      {{TURBODB_VALUE_INT64,false},turbodb_f64(-2.5),TURBODB_VALUE_INT64,-3,0,
        ORM_SQL_ASSIGNMENT_ROUNDED},
      {{TURBODB_VALUE_INT64,false},turbodb_text("  -12.9e3"),TURBODB_VALUE_INT64,-12900,0,
        ORM_SQL_ASSIGNMENT_EXACT},
      {{TURBODB_VALUE_UINT64,false},turbodb_blob("42",2),TURBODB_VALUE_UINT64,42,0,
        ORM_SQL_ASSIGNMENT_EXACT},
      {{TURBODB_VALUE_DOUBLE,false},turbodb_text(" 1.25e2 "),TURBODB_VALUE_DOUBLE,0,125.0,
        ORM_SQL_ASSIGNMENT_EXACT},
      {{TURBODB_VALUE_INT64,false},turbodb_bool(1),TURBODB_VALUE_INT64,1,0,
        ORM_SQL_ASSIGNMENT_EXACT}
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
      turbodb_value_t out=turbodb_null();
      orm_sql_assignment_adjustment adjustment=ORM_SQL_ASSIGNMENT_INVALID;
      check_equal(orm_tidesdb_sql_assignment_convert(cases[i].target,
          &cases[i].input,false,&budget,&out,&adjustment,&error),TURBODB_STATUS_OK);
      check_equal(out.kind,cases[i].kind);
      if (out.kind==TURBODB_VALUE_INT64)
        check_equal(out.data.int64_value,cases[i].integer);
      else if (out.kind==TURBODB_VALUE_UINT64)
        check_equal(out.data.uint64_value,(uint64_t)cases[i].integer);
      else check_equal(out.data.double_value,cases[i].floating);
      check_equal(adjustment,cases[i].adjustment);
    }
  }

  group("decimal column assignment") {
    it("keeps exact 64 bit text values across decimal points exponents and rounding") {
      const struct {const char *text; bool unsigned_target; uint64_t magnitude; bool negative; bool rounded;} cases[]={
        {"9007199254740993.0",false,UINT64_C(9007199254740993),false,false},
        {"90071992547409930e-1",false,UINT64_C(9007199254740993),false,false},
        {"9223372036854775807.4",false,INT64_MAX,false,true},
        {"-9223372036854775808.4",false,UINT64_C(9223372036854775808),true,true},
        {"18446744073709551615.4",true,UINT64_MAX,false,true},
        {"184467440737095516150e-1",true,UINT64_MAX,false,false},
        {"00018446744073709551615.0",true,UINT64_MAX,false,false},
        {"1.8446744073709551615e19",true,UINT64_MAX,false,false},
        {"-0.49",true,0,false,true}, {"-.5",false,1,true,true},
        {".500",true,1,false,true}, {"125e-1",false,13,false,true},
        {"0e9999999999999999999999999",false,0,false,false},
        {"1e-9999999999999999999999999",false,0,false,true}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        turbodb_value_t value=turbodb_text(cases[i].text);
        const orm_sql_type target={cases[i].unsigned_target?TURBODB_VALUE_UINT64:TURBODB_VALUE_INT64,false};
        orm_sql_assignment_adjustment adjustment=ORM_SQL_ASSIGNMENT_INVALID;
        check_equal(orm_tidesdb_sql_assignment_convert(target,&value,false,&budget,&value,&adjustment,&error),TURBODB_STATUS_OK);
        check_equal(value.kind,target.kind);
        if(cases[i].unsigned_target) check_equal(value.data.uint64_value,cases[i].magnitude);
        else check_equal(value.data.int64_value,cases[i].negative?
            cases[i].magnitude==UINT64_C(9223372036854775808)?INT64_MIN:-(int64_t)cases[i].magnitude:
            (int64_t)cases[i].magnitude);
        check_equal(adjustment,cases[i].rounded?ORM_SQL_ASSIGNMENT_ROUNDED:ORM_SQL_ASSIGNMENT_EXACT);
      }
    }
    it("rounds hundredths against an integer rational reference for text and binary inputs") {
      enum { MAGNITUDE=750, SCALE=100, HALF=50, TEXT_BYTES=32 };
      const orm_sql_type target={TURBODB_VALUE_INT64,false};
      for(int n=-MAGNITUDE;n<=MAGNITUDE;++n) for(size_t form=0;form<2;++form) {
        const int magnitude=n<0?-n:n;
        const int expected=(n<0?-1:1)*((magnitude+HALF)/SCALE);
        char text[TEXT_BYTES];
        if(form) (void)snprintf(text,sizeof(text),"%de-2",n);
        else (void)snprintf(text,sizeof(text),"%s%d.%02d",n<0?"-":"",magnitude/SCALE,magnitude%SCALE);
        const turbodb_value_t input=form?turbodb_blob(text,strlen(text)):turbodb_text(text);
        turbodb_value_t out=turbodb_null(); orm_sql_assignment_adjustment adjustment=ORM_SQL_ASSIGNMENT_INVALID;
        check_equal(orm_tidesdb_sql_assignment_convert(target,&input,false,
            &budget,&out,&adjustment,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,TURBODB_VALUE_INT64); check_equal(out.data.int64_value,expected);
        check_equal(adjustment,n%SCALE?ORM_SQL_ASSIGNMENT_ROUNDED:ORM_SQL_ASSIGNMENT_EXACT);
      }
    }
    it("rejects decimal range and suffix errors while retaining bounded IGNORE adjustments") {
      const struct {const char *text; bool unsigned_target; turbodb_status_t status; int64_t signed_value; uint64_t unsigned_value;
        orm_sql_assignment_adjustment adjustment;} cases[]={
        {"9223372036854775807.5",false,TURBODB_STATUS_OUT_OF_RANGE,INT64_MAX,0,ORM_SQL_ASSIGNMENT_OUT_OF_RANGE},
        {"-9223372036854775808.5",false,TURBODB_STATUS_OUT_OF_RANGE,INT64_MIN,0,ORM_SQL_ASSIGNMENT_OUT_OF_RANGE},
        {"18446744073709551615.5",true,TURBODB_STATUS_OUT_OF_RANGE,0,UINT64_MAX,ORM_SQL_ASSIGNMENT_OUT_OF_RANGE},
        {"-0.5",true,TURBODB_STATUS_OUT_OF_RANGE,0,0,ORM_SQL_ASSIGNMENT_OUT_OF_RANGE},
        {"1e9999999999999999999999999",true,TURBODB_STATUS_OUT_OF_RANGE,0,UINT64_MAX,ORM_SQL_ASSIGNMENT_OUT_OF_RANGE},
        {"2.5garbage",false,TURBODB_STATUS_TYPE_ERROR,3,0,ORM_SQL_ASSIGNMENT_TRUNCATED},
        {"2.5e+",false,TURBODB_STATUS_TYPE_ERROR,3,0,ORM_SQL_ASSIGNMENT_TRUNCATED},
        {".xyz",false,TURBODB_STATUS_TYPE_ERROR,0,0,ORM_SQL_ASSIGNMENT_INVALID}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        const turbodb_value_t input=turbodb_text(cases[i].text); turbodb_value_t out=turbodb_i64(SENTINEL);
        const orm_sql_type target={cases[i].unsigned_target?TURBODB_VALUE_UINT64:TURBODB_VALUE_INT64,false};
        orm_sql_assignment_adjustment adjustment=ORM_SQL_ASSIGNMENT_EXACT;
        check_equal(orm_tidesdb_sql_assignment_convert(target,&input,false,&budget,&out,&adjustment,&error),cases[i].status);
        check_equal(out.data.int64_value,SENTINEL); check_equal(adjustment,ORM_SQL_ASSIGNMENT_EXACT);
        check_equal(orm_tidesdb_sql_assignment_convert(target,&input,true,&budget,&out,&adjustment,&error),TURBODB_STATUS_OK);
        check_equal(out.kind,target.kind); check_equal(adjustment,cases[i].adjustment);
        if(cases[i].unsigned_target) check_equal(out.data.uint64_value,cases[i].unsigned_value);
        else check_equal(out.data.int64_value,cases[i].signed_value);
      }
    }
    it("always produces finite DOUBLE for integer shaped and large numeric strings") {
      const char *text[]={"7","-7","0007","999999999999999999999","-0"};
      const double expected[]={7.0,-7.0,7.0,1e21,-0.0};
      const orm_sql_type target={TURBODB_VALUE_DOUBLE,false};
      for(size_t i=0;i<sizeof(text)/sizeof(text[0]);++i) {
        turbodb_value_t value=turbodb_text(text[i]); orm_sql_assignment_adjustment adjustment=ORM_SQL_ASSIGNMENT_INVALID;
        check_equal(orm_tidesdb_sql_assignment_convert(target,&value,false,
            &budget,&value,&adjustment,&error),TURBODB_STATUS_OK);
        check_equal(value.kind,TURBODB_VALUE_DOUBLE); check_equal(value.data.double_value,expected[i]);
        check_equal(signbit(value.data.double_value)!=0,signbit(expected[i])!=0);
        check_equal(adjustment,ORM_SQL_ASSIGNMENT_EXACT);
      }
    }
    it("keeps already integral large DOUBLE values during half away rounding") {
      const double input[]={4503599627370497.0,-4503599627370497.0,9007199254740991.0,
        -9007199254740991.0,4503599627370495.5,-4503599627370495.5};
      const int64_t expected[]={INT64_C(4503599627370497),INT64_C(-4503599627370497),INT64_C(9007199254740991),
        INT64_C(-9007199254740991),INT64_C(4503599627370496),INT64_C(-4503599627370496)};
      const orm_sql_type target={TURBODB_VALUE_INT64,false};
      for(size_t i=0;i<sizeof(input)/sizeof(input[0]);++i) {
        turbodb_value_t value=turbodb_f64(input[i]); orm_sql_assignment_adjustment adjustment=ORM_SQL_ASSIGNMENT_INVALID;
        check_equal(orm_tidesdb_sql_assignment_convert(target,&value,false,
            &budget,&value,&adjustment,&error),TURBODB_STATUS_OK);
        check_equal(value.kind,TURBODB_VALUE_INT64); check_equal(value.data.int64_value,expected[i]);
        check_equal(adjustment,i<4?ORM_SQL_ASSIGNMENT_EXACT:ORM_SQL_ASSIGNMENT_ROUNDED);
      }
    }
    it("charges decimal byte work before scanning and preserves output on exact quota exhaustion") {
      const char text[]="184467440737095516150e-1";
      const orm_sql_type target={TURBODB_VALUE_UINT64,false};
      const turbodb_value_t input=turbodb_blob(text,sizeof(text)-1); turbodb_value_t out=turbodb_i64(SENTINEL);
      orm_sql_assignment_adjustment adjustment=ORM_SQL_ASSIGNMENT_INVALID;
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=sizeof(text)-1;
      check_equal(orm_tidesdb_sql_assignment_convert(target,&input,false,
          &budget,&out,&adjustment,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value,SENTINEL); check_equal(adjustment,ORM_SQL_ASSIGNMENT_INVALID);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],0u);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=sizeof(text);
      check_equal(orm_tidesdb_sql_assignment_convert(target,&input,false,
          &budget,&out,&adjustment,&error),TURBODB_STATUS_OK);
      check_equal(out.data.uint64_value,UINT64_MAX);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],sizeof(text));
    }
  }

  it("rejects strict assignment loss and applies bounded IGNORE adjustments") {
    const orm_sql_type signed_type={TURBODB_VALUE_INT64,false};
    const orm_sql_type unsigned_type={TURBODB_VALUE_UINT64,false};
    const orm_sql_type double_type={TURBODB_VALUE_DOUBLE,false};
    const struct {
      orm_sql_type target;
      turbodb_value_t input;
      turbodb_status_t strict;
      turbodb_value_t replacement;
      orm_sql_assignment_adjustment adjustment;
    } cases[] = {
      {signed_type,turbodb_u64(UINT64_MAX),TURBODB_STATUS_OUT_OF_RANGE,
        turbodb_i64(INT64_MAX),ORM_SQL_ASSIGNMENT_OUT_OF_RANGE},
      {unsigned_type,turbodb_i64(-1),TURBODB_STATUS_OUT_OF_RANGE,
        turbodb_u64(0),ORM_SQL_ASSIGNMENT_OUT_OF_RANGE},
      {signed_type,turbodb_text("abc"),TURBODB_STATUS_TYPE_ERROR,
        turbodb_i64(0),ORM_SQL_ASSIGNMENT_INVALID},
      {signed_type,turbodb_text("12xyz"),TURBODB_STATUS_TYPE_ERROR,
        turbodb_i64(12),ORM_SQL_ASSIGNMENT_TRUNCATED},
      {double_type,turbodb_text("1e99999"),TURBODB_STATUS_OUT_OF_RANGE,
        turbodb_f64(DBL_MAX),ORM_SQL_ASSIGNMENT_OUT_OF_RANGE},
      {signed_type,turbodb_null(),TURBODB_STATUS_CONSTRAINT,
        turbodb_i64(0),ORM_SQL_ASSIGNMENT_NULL_TO_NOT_NULL}
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
      turbodb_value_t out=turbodb_bool(1);
      orm_sql_assignment_adjustment adjustment=ORM_SQL_ASSIGNMENT_ROUNDED;
      check_equal(orm_tidesdb_sql_assignment_convert(cases[i].target,
          &cases[i].input,false,&budget,&out,&adjustment,&error),cases[i].strict);
      check_equal(out.kind,TURBODB_VALUE_BOOLEAN);
      check_equal(adjustment,ORM_SQL_ASSIGNMENT_ROUNDED);
      check_equal(orm_tidesdb_sql_assignment_convert(cases[i].target,
          &cases[i].input,true,&budget,&out,&adjustment,&error),TURBODB_STATUS_OK);
      check_equal(out.kind,cases[i].replacement.kind);
      if (out.kind==TURBODB_VALUE_INT64)
        check_equal(out.data.int64_value,cases[i].replacement.data.int64_value);
      else if (out.kind==TURBODB_VALUE_UINT64)
        check_equal(out.data.uint64_value,cases[i].replacement.data.uint64_value);
      else check_equal(out.data.double_value,
          cases[i].replacement.data.double_value);
      check_equal(adjustment,cases[i].adjustment);
    }
    check_true(orm_tidesdb_sql_assignment_compatible(signed_type,
        (orm_sql_type){TURBODB_VALUE_TEXT,true}));
    check_false(orm_tidesdb_sql_assignment_compatible(
        (orm_sql_type){TURBODB_VALUE_TEXT,true},double_type));
  }

  it("implements the complete SQL NOT AND OR three-valued truth tables") {
    const turbodb_value_t values[] = {turbodb_bool(0), turbodb_bool(1), turbodb_null()};
    const int and_table[][3] = {{0, 0, 0}, {0, 1, -1}, {0, -1, -1}};
    const int or_table[][3] = {{0, 1, -1}, {1, 1, 1}, {-1, 1, -1}};
    const int not_table[] = {1, 0, -1};
    for (size_t i = 0; i < 3; ++i) {
      expect_truth(unary_value(ORM_SQL_NOT, values[i]), not_table[i]);
      for (size_t j = 0; j < 3; ++j) {
        expect_truth(binary(ORM_SQL_AND, values[i], values[j]), and_table[i][j]);
        expect_truth(binary(ORM_SQL_OR, values[i], values[j]), or_table[i][j]);
      }
    }
  }

  it("distinguishes ordinary NULL comparisons from NULL-safe equality and IS NULL") {
    const turbodb_value_t values[] = {turbodb_null(), turbodb_i64(0), turbodb_text(""), turbodb_bool(0)};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
      for (int op = ORM_SQL_EQUAL; op <= ORM_SQL_GREATER_EQUAL; ++op) {
        expect_truth(binary((orm_sql_predicate_op)op, turbodb_null(), values[i]), -1);
        expect_truth(binary((orm_sql_predicate_op)op, values[i], turbodb_null()), -1);
      }
      expect_truth(binary(ORM_SQL_NULL_SAFE_EQUAL, turbodb_null(), values[i]), i == 0);
      expect_truth(binary(ORM_SQL_NULL_SAFE_EQUAL, values[i], turbodb_null()), i == 0);
      expect_truth(unary_value(ORM_SQL_IS_NULL, values[i]), i == 0);
      expect_truth(unary_value(ORM_SQL_IS_NOT_NULL, values[i]), i != 0);
    }
  }

  it("preserves exact integer ordering across signed unsigned and double precision boundaries") {
    const turbodb_value_t values[] = {turbodb_i64(INT64_MIN), turbodb_i64(-1), turbodb_i64(0), turbodb_u64(0),
      turbodb_i64(1), turbodb_u64(1), turbodb_i64(INT64_C(9007199254740992)),
      turbodb_u64(UINT64_C(9007199254740993)), turbodb_i64(INT64_MAX), turbodb_u64(INT64_MAX),
      turbodb_u64((uint64_t)INT64_MAX + 1), turbodb_u64(UINT64_MAX)};
    const int rank[] = {0, 1, 2, 2, 3, 3, 4, 5, 6, 6, 7, 8};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
      for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
        expect_truth(binary(ORM_SQL_EQUAL, values[i], values[j]), rank[i] == rank[j]);
        expect_truth(binary(ORM_SQL_NULL_SAFE_EQUAL, values[i], values[j]), rank[i] == rank[j]);
        expect_truth(binary(ORM_SQL_NOT_EQUAL, values[i], values[j]), rank[i] != rank[j]);
        expect_truth(binary(ORM_SQL_LESS, values[i], values[j]), rank[i] < rank[j]);
        expect_truth(binary(ORM_SQL_LESS_EQUAL, values[i], values[j]), rank[i] <= rank[j]);
        expect_truth(binary(ORM_SQL_GREATER, values[i], values[j]), rank[i] > rank[j]);
        expect_truth(binary(ORM_SQL_GREATER_EQUAL, values[i], values[j]), rank[i] >= rank[j]);
      }
    }
  }

  group("mixed real comparisons") {
    it("compares numeric peers in both orders for every comparison operator") {
      const struct {turbodb_value_t a,b; int order;} cases[]={
        {turbodb_i64(-7),turbodb_f64(-6.5),-1},{turbodb_i64(7),turbodb_f64(6.5),1},
        {turbodb_u64(7),turbodb_f64(7.0),0},{turbodb_u64(0),turbodb_f64(-0.0),0},
        {turbodb_i64(0),turbodb_f64(-0.5),1},{turbodb_u64(UINT64_MAX),turbodb_f64(18446744073709551616.0),0}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) for(size_t reverse=0;reverse<2;++reverse) {
        const turbodb_value_t a=reverse?cases[i].b:cases[i].a,b=reverse?cases[i].a:cases[i].b;
        const int order=reverse?-cases[i].order:cases[i].order;
        const bool expected[]={order==0,order!=0,order<0,order<=0,order>0,order>=0,order==0};
        for(int op=ORM_SQL_EQUAL;op<=ORM_SQL_NULL_SAFE_EQUAL;++op)
          expect_truth(binary((orm_sql_predicate_op)op,a,b),expected[op]);
      }
    }
    it("rounds only real comparisons and retains exact integer distinctions") {
      const turbodb_value_t exact=turbodb_u64(UINT64_C(9007199254740993)),peer=turbodb_i64(INT64_C(9007199254740992));
      expect_truth(binary(ORM_SQL_EQUAL,exact,peer),0);
      expect_truth(binary(ORM_SQL_GREATER,exact,peer),1);
      expect_truth(binary(ORM_SQL_EQUAL,exact,turbodb_f64(9007199254740992.0)),1);
      expect_truth(binary(ORM_SQL_EQUAL,turbodb_i64(INT64_MAX),turbodb_f64(9223372036854775808.0)),1);
      expect_truth(binary(ORM_SQL_EQUAL,turbodb_i64(INT64_MIN),turbodb_f64(-9223372036854775808.0)),1);
      expect_truth(binary(ORM_SQL_LESS,turbodb_f64(-9223372036854775808.0),turbodb_u64(UINT64_MAX)),1);
    }
    it("validates original kinds and finite values before charging and preserves aliased output") {
      const orm_sql_type integer={TURBODB_VALUE_INT64,true},real={TURBODB_VALUE_DOUBLE,true};
      orm_sql_predicate fn={0};
      check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL,integer,&real,&fn,&error),TURBODB_STATUS_OK);
      check_equal(fn.left.kind,TURBODB_VALUE_INT64);check_equal(fn.right.kind,TURBODB_VALUE_DOUBLE);
      check_true(fn.real_comparison);check_true(fn.result.nullable);
      turbodb_value_t a=turbodb_f64(1.0),b=turbodb_f64(1.0),out=turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_predicate_eval(&fn,&a,&b,&budget,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      a=turbodb_i64(1);b=turbodb_f64(INFINITY);
      check_equal(orm_tidesdb_sql_predicate_eval(&fn,&a,&b,&budget,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      b=turbodb_f64(NAN);
      check_equal(orm_tidesdb_sql_predicate_eval(&fn,&a,&b,&budget,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],0u);check_equal(out.data.int64_value,SENTINEL);
      b=turbodb_f64(1.0);budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
      check_equal(orm_tidesdb_sql_predicate_eval(&fn,&a,&b,&budget,&a,&error),TURBODB_STATUS_OK);expect_truth(a,1);
      a=turbodb_i64(1);
      check_equal(orm_tidesdb_sql_predicate_eval(&fn,&a,&b,&budget,&a,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(a.kind,TURBODB_VALUE_INT64);check_equal(a.data.int64_value,1);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT;
      a=turbodb_null();expect_truth(binary(ORM_SQL_EQUAL,a,b),-1);expect_truth(binary(ORM_SQL_NULL_SAFE_EQUAL,a,b),0);
      expect_truth(binary(ORM_SQL_NULL_SAFE_EQUAL,a,turbodb_null()),1);
    }
  }

  it("checks all comparison type pairs and refuses string and boolean numeric conversion") {
    for (int a = TURBODB_VALUE_NULL; a <= TURBODB_VALUE_BLOB; ++a) {
      for (int b = TURBODB_VALUE_NULL; b <= TURBODB_VALUE_BLOB; ++b) {
        const orm_sql_type left = {a, true}, right = {b, true};
        orm_sql_predicate predicate = {.op = ORM_SQL_OR};
        const bool supported = a == TURBODB_VALUE_NULL || b == TURBODB_VALUE_NULL || a == b ||
            ((a == TURBODB_VALUE_INT64 || a == TURBODB_VALUE_UINT64 || a == TURBODB_VALUE_DOUBLE) &&
             (b == TURBODB_VALUE_INT64 || b == TURBODB_VALUE_UINT64 || b == TURBODB_VALUE_DOUBLE));
        check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL, left, &right, &predicate, &error),
                    supported ? TURBODB_STATUS_OK : TURBODB_STATUS_UNSUPPORTED);
        if (!supported) check_equal(predicate.op, ORM_SQL_OR);
      }
    }
    const orm_sql_type integer = {TURBODB_VALUE_INT64, false};
    orm_sql_predicate predicate = {0};
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_NOT, integer, NULL, &predicate, &error), TURBODB_STATUS_UNSUPPORTED);
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_AND, integer, &integer, &predicate, &error), TURBODB_STATUS_UNSUPPORTED);
  }

  it("binds declared nullability and rejects runtime type drift before charging") {
    const orm_sql_type integer = {TURBODB_VALUE_INT64, false}, nullable = {TURBODB_VALUE_INT64, true};
    orm_sql_predicate predicate = {0};
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL, integer, &integer, &predicate, &error), TURBODB_STATUS_OK);
    check_equal(predicate.result.kind, TURBODB_VALUE_BOOLEAN);
    check_false(predicate.result.nullable);
    turbodb_value_t left = turbodb_null(), right = turbodb_i64(1), out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &left, &right, &budget, &out, &error), TURBODB_STATUS_TYPE_ERROR);
    left = turbodb_u64(1);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &left, &right, &budget, &out, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(out.kind, TURBODB_VALUE_INT64); check_equal(out.data.int64_value, SENTINEL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 0u);
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL, nullable, &integer, &predicate, &error), TURBODB_STATUS_OK);
    check_true(predicate.result.nullable);
    left = turbodb_null();
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &left, &right, &budget, &out, &error), TURBODB_STATUS_OK);
    expect_truth(out, -1);
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_NULL_SAFE_EQUAL, nullable, &nullable, &predicate, &error), TURBODB_STATUS_OK);
    check_false(predicate.result.nullable);
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, nullable, NULL, &predicate, &error), TURBODB_STATUS_OK);
    check_false(predicate.result.nullable);
  }

  it("compares text by complete UTF-8 bytes with significant NUL case and trailing spaces") {
    const char first[] = {'a', 0, 'b'}, second[] = {'a', 0, 'c'};
    expect_truth(binary(ORM_SQL_LESS, turbodb_text_v((vstr){first, sizeof(first)}),
      turbodb_text_v((vstr){second, sizeof(second)})), 1);
    expect_truth(binary(ORM_SQL_EQUAL, turbodb_text("a"), turbodb_text("a ")), 0);
    expect_truth(binary(ORM_SQL_EQUAL, turbodb_text("A"), turbodb_text("a")), 0);
    expect_truth(binary(ORM_SQL_LESS, turbodb_text(""), turbodb_text("a")), 1);
    expect_truth(binary(ORM_SQL_EQUAL, turbodb_text_v((vstr){NULL, 0}), turbodb_text("")), 1);
    expect_truth(binary(ORM_SQL_LESS, turbodb_text("z"), turbodb_text("\xe4\xb8\xad")), 1);
  }

  it("rejects malformed UTF-8 as TEXT but compares the same bytes as BLOB") {
    const unsigned char invalid[] = {0xc0, 0x80};
    turbodb_value_t a = turbodb_text_v((vstr){(const char *)invalid, sizeof(invalid)}), b = turbodb_text("ok");
    const orm_sql_predicate predicate = bind_values(ORM_SQL_EQUAL, a, &b);
    turbodb_value_t out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &b, &budget, &out, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(out.data.int64_value, SENTINEL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 7u);
    expect_truth(binary(ORM_SQL_EQUAL, turbodb_blob(invalid, sizeof(invalid)), turbodb_blob(invalid, sizeof(invalid))), 1);
    const unsigned char low[] = {0, 0x7f}, high[] = {0, 0xff};
    expect_truth(binary(ORM_SQL_LESS, turbodb_blob(low, sizeof(low)), turbodb_blob(high, sizeof(high))), 1);
  }

  it("supports finite floating comparisons and rejects NaN and infinities") {
    expect_truth(binary(ORM_SQL_EQUAL, turbodb_f64(-0.0), turbodb_f64(0.0)), 1);
    expect_truth(binary(ORM_SQL_LESS, turbodb_f64(-1.5), turbodb_f64(0.5)), 1);
    const double invalid[] = {NAN, INFINITY, -INFINITY};
    turbodb_value_t a = turbodb_f64(1), b = turbodb_f64(1), out = turbodb_i64(SENTINEL);
    const orm_sql_predicate predicate = bind_values(ORM_SQL_EQUAL, a, &b);
    const uint64_t used = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      a = turbodb_f64(invalid[i]);
      check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &b, &budget, &out, &error), TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value, SENTINEL);
    }
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], used);
  }

  it("rejects noncanonical booleans reserved flags and nonempty NULL byte views") {
    turbodb_value_t invalid[] = {turbodb_bool(1), turbodb_i64(1), turbodb_text_v((vstr){NULL, 1}), turbodb_blob(NULL, 1)};
    invalid[0].data.boolean_value = 2; invalid[1].reserved = 1;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      const orm_sql_predicate predicate = bind_values(ORM_SQL_IS_NULL, invalid[i], NULL);
      turbodb_value_t out = turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &invalid[i], NULL, &budget, &out, &error), TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value, SENTINEL);
    }
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 0u);
  }

  it("charges validation and comparison bytes before reading text and preserves output at exhaustion") {
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    start_budget(7);
    turbodb_value_t a = turbodb_text("ab"), b = turbodb_text("ab"), out = turbodb_i64(SENTINEL);
    const orm_sql_predicate predicate = bind_values(ORM_SQL_EQUAL, a, &b);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &b, &budget, &out, &error), TURBODB_STATUS_OK);
    expect_truth(out, 1);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 7u);
    out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &b, &budget, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.data.int64_value, SENTINEL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 7u);
  }

  it("rejects huge byte lengths before dereferencing and keeps counters unchanged") {
    turbodb_value_t a = turbodb_text_v((vstr){"x", SIZE_MAX}), b = a, out = turbodb_i64(SENTINEL);
    const orm_sql_predicate predicate = bind_values(ORM_SQL_EQUAL, a, &b);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &b, &budget, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 0u);
    check_equal(out.data.int64_value, SENTINEL);
  }

  it("permits output aliasing without retaining input bytes") {
    turbodb_value_t a = turbodb_text("same"), b = turbodb_text("same");
    const orm_sql_predicate predicate = bind_values(ORM_SQL_EQUAL, a, &b);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &b, &budget, &a, NULL), TURBODB_STATUS_OK);
    expect_truth(a, 1);
  }

  it("fails on invalid bindings arguments and inactive execution budgets") {
    const orm_sql_type integer = {TURBODB_VALUE_INT64, false}, bad_null = {TURBODB_VALUE_NULL, false};
    const orm_sql_type unknown = {-1, false};
    orm_sql_predicate predicate = {0};
    check_equal(orm_tidesdb_sql_predicate_bind((orm_sql_predicate_op)-1, integer, &integer, &predicate, &error), TURBODB_STATUS_UNSUPPORTED);
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL, integer, NULL, &predicate, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_NOT, integer, &integer, &predicate, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL, bad_null, &integer, &predicate, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL, integer, &unknown, &predicate, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL, integer, &integer, NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL, integer, &integer, &predicate, &error), TURBODB_STATUS_OK);
    turbodb_value_t a = turbodb_i64(1), out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_predicate_eval(NULL, &a, &a, &budget, &out, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, NULL, &budget, &out, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    predicate.op = (orm_sql_predicate_op)-1;
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &a, &budget, &out, &error), TURBODB_STATUS_INVALID_STATE);
    predicate.op = ORM_SQL_EQUAL;
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &a, &budget, &out, &error), TURBODB_STATUS_INVALID_STATE);
    check_equal(out.data.int64_value, SENTINEL);
  }

  it("binds numeric arithmetic with DOUBLE promotion and propagates declared NULL") {
    for (int op = ORM_SQL_ADD; op <= ORM_SQL_MULTIPLY; ++op)
      for (int a = TURBODB_VALUE_NULL; a <= TURBODB_VALUE_BLOB; ++a)
        for (int b = TURBODB_VALUE_NULL; b <= TURBODB_VALUE_BLOB; ++b) {
          const orm_sql_type left = {a, true}, right = {b, true};
          const bool numeric_a = a == TURBODB_VALUE_NULL || a == TURBODB_VALUE_INT64 || a == TURBODB_VALUE_UINT64 || a == TURBODB_VALUE_DOUBLE;
          const bool numeric_b = b == TURBODB_VALUE_NULL || b == TURBODB_VALUE_INT64 || b == TURBODB_VALUE_UINT64 || b == TURBODB_VALUE_DOUBLE;
          const bool real = a == TURBODB_VALUE_DOUBLE || b == TURBODB_VALUE_DOUBLE;
          const bool supported = numeric_a && numeric_b && (real || a == b || a == TURBODB_VALUE_NULL || b == TURBODB_VALUE_NULL);
          orm_sql_arithmetic arithmetic = {.op = ORM_SQL_NEGATE};
          check_equal(orm_tidesdb_sql_arithmetic_bind((orm_sql_arithmetic_op)op, left, &right, &arithmetic, &error),
              supported ? TURBODB_STATUS_OK : TURBODB_STATUS_UNSUPPORTED);
          if (supported) {
            check_equal(arithmetic.result.kind, real ? TURBODB_VALUE_DOUBLE : a == TURBODB_VALUE_NULL ? b : a);
            check_equal(arithmetic.result.nullable, true);
          } else check_equal(arithmetic.op, ORM_SQL_NEGATE);
        }
    const orm_sql_type integer = {TURBODB_VALUE_INT64, false}, nullable = {TURBODB_VALUE_INT64, true};
    orm_sql_arithmetic arithmetic;
    check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_ADD, integer, &integer, &arithmetic, &error), TURBODB_STATUS_OK);
    check_equal(arithmetic.result.nullable, false);
    check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_ADD, integer, &nullable, &arithmetic, &error), TURBODB_STATUS_OK);
    check_equal(arithmetic.result.nullable, true);
  }

  it("checks signed arithmetic over a small exhaustive reference range") {
    enum { MAGNITUDE = 20 };
    const turbodb_value_t zero = turbodb_i64(0);
    for (int op = ORM_SQL_ADD; op <= ORM_SQL_MULTIPLY; ++op) {
      const orm_sql_arithmetic arithmetic = bind_arithmetic((orm_sql_arithmetic_op)op, zero, &zero);
      for (int a = -MAGNITUDE; a <= MAGNITUDE; ++a) for (int b = -MAGNITUDE; b <= MAGNITUDE; ++b) {
        const int expected = op == ORM_SQL_ADD ? a + b : op == ORM_SQL_SUBTRACT ? a - b : a * b;
        const turbodb_value_t left = turbodb_i64(a), right = turbodb_i64(b); turbodb_value_t out = turbodb_null();
        check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &left, &right, &budget, &out, &error), TURBODB_STATUS_OK);
        check_equal(out.kind, TURBODB_VALUE_INT64); check_equal(out.data.int64_value, expected);
      }
    }
  }

  it("rejects signed overflow before computing and preserves exact extreme products") {
    const struct { orm_sql_arithmetic_op op; int64_t a, b, expected; bool overflow; } cases[] = {
      {ORM_SQL_ADD, INT64_MAX, 1, 0, true}, {ORM_SQL_ADD, INT64_MIN, -1, 0, true},
      {ORM_SQL_ADD, INT64_MIN, INT64_MAX, -1, false}, {ORM_SQL_ADD, INT64_MAX, 0, INT64_MAX, false},
      {ORM_SQL_SUBTRACT, INT64_MIN, 1, 0, true}, {ORM_SQL_SUBTRACT, INT64_MAX, -1, 0, true},
      {ORM_SQL_SUBTRACT, INT64_MIN, INT64_MIN, 0, false}, {ORM_SQL_SUBTRACT, -1, INT64_MIN, INT64_MAX, false},
      {ORM_SQL_MULTIPLY, INT64_MIN, -1, 0, true}, {ORM_SQL_MULTIPLY, -1, INT64_MIN, 0, true},
      {ORM_SQL_MULTIPLY, INT64_MAX, 2, 0, true}, {ORM_SQL_MULTIPLY, INT64_MIN, 1, INT64_MIN, false},
      {ORM_SQL_MULTIPLY, -2, INT64_C(4611686018427387904), INT64_MIN, false},
      {ORM_SQL_MULTIPLY, INT64_C(-4611686018427387904), 2, INT64_MIN, false},
      {ORM_SQL_MULTIPLY, INT64_C(-4611686018427387904), -2, 0, true},
      {ORM_SQL_MULTIPLY, INT64_MIN, 0, 0, false}, {ORM_SQL_MULTIPLY, 0, INT64_MIN, 0, false}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      const turbodb_value_t a = turbodb_i64(cases[i].a), b = turbodb_i64(cases[i].b);
      const orm_sql_arithmetic arithmetic = bind_arithmetic(cases[i].op, a, &b);
      turbodb_value_t out = turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error),
          cases[i].overflow ? TURBODB_STATUS_LIMIT_EXCEEDED : TURBODB_STATUS_OK);
      check_equal(out.kind, TURBODB_VALUE_INT64); check_equal(out.data.int64_value, cases[i].overflow ? SENTINEL : cases[i].expected);
      if (cases[i].overflow) check_contains(error.message, "arithmetic result");
    }
  }

  it("rejects unsigned wraparound while retaining full U64 precision") {
    const struct { orm_sql_arithmetic_op op; uint64_t a, b, expected; bool overflow; } cases[] = {
      {ORM_SQL_ADD, UINT64_MAX, 1, 0, true}, {ORM_SQL_ADD, UINT64_MAX - 1, 1, UINT64_MAX, false},
      {ORM_SQL_SUBTRACT, 0, 1, 0, true}, {ORM_SQL_SUBTRACT, UINT64_MAX, UINT64_MAX, 0, false},
      {ORM_SQL_MULTIPLY, UINT64_MAX, 2, 0, true}, {ORM_SQL_MULTIPLY, UINT64_MAX, 1, UINT64_MAX, false},
      {ORM_SQL_MULTIPLY, UINT64_MAX, 0, 0, false}, {ORM_SQL_MULTIPLY, 0, UINT64_MAX, 0, false},
      {ORM_SQL_MULTIPLY, UINT64_C(4294967295), UINT64_C(4294967295), UINT64_C(18446744065119617025), false}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      const turbodb_value_t a = turbodb_u64(cases[i].a), b = turbodb_u64(cases[i].b);
      const orm_sql_arithmetic arithmetic = bind_arithmetic(cases[i].op, a, &b);
      turbodb_value_t out = turbodb_u64(SENTINEL);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error),
          cases[i].overflow ? TURBODB_STATUS_LIMIT_EXCEEDED : TURBODB_STATUS_OK);
      check_equal(out.kind, TURBODB_VALUE_UINT64); check_equal(out.data.uint64_value, cases[i].overflow ? SENTINEL : cases[i].expected);
    }
  }

  it("handles finite F64 arithmetic and rejects nonfinite inputs and results") {
    const double expected[] = {2.5, 0.5, 1.5};
    for (int op = ORM_SQL_ADD; op <= ORM_SQL_MULTIPLY; ++op) {
      turbodb_value_t a = turbodb_f64(1.5), b = turbodb_f64(1.0), out = turbodb_null();
      const orm_sql_arithmetic arithmetic = bind_arithmetic((orm_sql_arithmetic_op)op, a, &b);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_OK);
      check_equal(out.kind, TURBODB_VALUE_DOUBLE); check_equal(out.data.double_value, expected[op]);
      a = turbodb_f64(DBL_MAX); b = turbodb_f64(op == ORM_SQL_SUBTRACT ? -DBL_MAX : DBL_MAX); out = turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value, SENTINEL);
      const uint64_t steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      a = turbodb_null(); b = turbodb_f64(INFINITY);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_TYPE_ERROR);
      b = turbodb_f64(NAN);
      check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_TYPE_ERROR);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], steps);
    }
  }

  it("checks unary signs NULL propagation and output aliasing") {
    turbodb_value_t a = turbodb_i64(-1);
    orm_sql_arithmetic arithmetic = bind_arithmetic(ORM_SQL_NEGATE, a, NULL);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, NULL, &budget, &a, &error), TURBODB_STATUS_OK);
    check_equal(a.data.int64_value, 1);
    a = turbodb_i64(INT64_MIN);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, NULL, &budget, &a, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(a.data.int64_value, INT64_MIN);
    a = turbodb_f64(0.0); arithmetic = bind_arithmetic(ORM_SQL_NEGATE, a, NULL);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, NULL, &budget, &a, &error), TURBODB_STATUS_OK);
    check_equal(signbit(a.data.double_value) != 0, true);
    a = turbodb_u64(UINT64_MAX); arithmetic = bind_arithmetic(ORM_SQL_POSITIVE, a, NULL);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, NULL, &budget, &a, &error), TURBODB_STATUS_OK);
    check_equal(a.data.uint64_value, UINT64_MAX);
    const orm_sql_type unsigned_type = {TURBODB_VALUE_UINT64, false};
    check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_NEGATE, unsigned_type, NULL, &arithmetic, &error), TURBODB_STATUS_UNSUPPORTED);
    a = turbodb_i64(7); turbodb_value_t b = turbodb_i64(3); arithmetic = bind_arithmetic(ORM_SQL_SUBTRACT, a, &b);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &b, &error), TURBODB_STATUS_OK);
    check_equal(b.data.int64_value, 4);
    a = turbodb_null();
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &b, &error), TURBODB_STATUS_OK);
    check_equal(b.kind, TURBODB_VALUE_NULL);
  }

  it("validates arithmetic arity descriptors and runtime types without corrupting output") {
    const orm_sql_type integer = {TURBODB_VALUE_INT64, false}, bad_null = {TURBODB_VALUE_NULL, false};
    orm_sql_arithmetic arithmetic = {.op = ORM_SQL_NEGATE};
    check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_ADD, integer, NULL, &arithmetic, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_NEGATE, integer, &integer, &arithmetic, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_ADD, integer, &bad_null, &arithmetic, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(orm_tidesdb_sql_arithmetic_bind((orm_sql_arithmetic_op)-1, integer, &integer, &arithmetic, &error), TURBODB_STATUS_UNSUPPORTED);
    check_equal(arithmetic.op, ORM_SQL_NEGATE);
    check_equal(orm_tidesdb_sql_arithmetic_bind(ORM_SQL_ADD, integer, &integer, &arithmetic, &error), TURBODB_STATUS_OK);
    turbodb_value_t a = turbodb_i64(1), b = turbodb_null(), out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, NULL, &budget, &out, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_TYPE_ERROR);
    b = turbodb_u64(1);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_TYPE_ERROR);
    b = turbodb_i64(1); b.reserved = 1;
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 0u);
    arithmetic.op = (orm_sql_arithmetic_op)-1;
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_INVALID_STATE);
    check_equal(out.data.int64_value, SENTINEL);
  }

  it("charges failed numeric overflow once and refuses execution after budget exhaustion") {
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK); start_budget(1);
    const turbodb_value_t a = turbodb_i64(INT64_MAX), b = turbodb_i64(1);
    const orm_sql_arithmetic arithmetic = bind_arithmetic(ORM_SQL_ADD, a, &b);
    turbodb_value_t out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_contains(error.message, "arithmetic result");
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 1u);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.data.int64_value, SENTINEL);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_arithmetic_eval(&arithmetic, &a, &b, &budget, &out, &error), TURBODB_STATUS_INVALID_STATE);
  }

  it("matches ASCII LIKE wildcards escapes spaces and embedded NUL with negation") {
    const orm_sql_type text_type = {TURBODB_VALUE_TEXT, false};
    const struct { const char *text, *pattern; int escape; bool match; } cases[] = {
      {"", "", -1, true}, {"", "%", -1, true}, {"", "_", -1, false},
      {"abc", "a_c", -1, true}, {"abc", "a%", -1, true}, {"abc", "%b%", -1, true},
      {"abababc", "%ababc", -1, true}, {"abc", "%a%z", -1, false}, {"abc", "ABC", -1, false},
      {"a ", "a", -1, false}, {"a ", "a_", -1, true}, {"%_", "|%|_", '|', true},
      {"a|", "a|", '|', true}, {"a|b", "a||b", '|', true}, {"a\\b", "a\\b", -1, true},
      {"a%b", "a%%b", '%', true}, {"a_b", "a__b", '_', true}, {"aaaab", "%a%a%ab", -1, true}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      const turbodb_value_t a = turbodb_text(cases[i].text), b = turbodb_text(cases[i].pattern);
      info("LIKE case %zu: text=%s pattern=%s escape=%d", i, cases[i].text, cases[i].pattern, cases[i].escape);
      for (unsigned negate = 0; negate < 2; ++negate) {
        orm_sql_like like; turbodb_value_t out = turbodb_null();
        check_equal(orm_tidesdb_sql_like_bind(text_type, text_type, cases[i].escape,
            negate != 0, &like, &error), TURBODB_STATUS_OK);
        check_equal(like.result.nullable, false);
        check_equal(orm_tidesdb_sql_like_eval(&like, &a, &b, &budget, &out, &error), TURBODB_STATUS_OK);
        expect_truth(out, negate ? !cases[i].match : cases[i].match);
      }
    }
    orm_sql_like like; turbodb_value_t a = turbodb_text_v((vstr){"a\0b", 3}), b = turbodb_text("a_b"), out = turbodb_null();
    check_equal(orm_tidesdb_sql_like_bind(text_type, text_type, ORM_SQL_LIKE_NO_ESCAPE, false, &like, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_like_eval(&like, &a, &b, &budget, &a, &error), TURBODB_STATUS_OK);
    expect_truth(a, 1);
    check_equal(orm_tidesdb_sql_like_bind(text_type, text_type, 0, false, &like, &error), TURBODB_STATUS_OK);
    a = turbodb_text("a%b"); b = turbodb_text_v((vstr){"a\0%b", 4});
    check_equal(orm_tidesdb_sql_like_eval(&like, &a, &b, &budget, &out, &error), TURBODB_STATUS_OK); expect_truth(out, 1);
  }

  it("selects ASCII case folding explicitly without changing default LIKE or escape syntax") {
    const orm_sql_type text={TURBODB_VALUE_TEXT,false}; orm_sql_like like={0};
    const turbodb_value_t name=turbodb_text("transaction_read_only"), pattern=turbodb_text("TRANSACTION\\_READ\\_O_L%");
    turbodb_value_t output=turbodb_null();
    check_equal(orm_tidesdb_sql_like_bind(text,text,'\\',false,&like,&error),TURBODB_STATUS_OK);
    check_false(like.ascii_insensitive);
    check_equal(orm_tidesdb_sql_like_eval(&like,&name,&pattern,&budget,&output,&error),TURBODB_STATUS_OK); expect_truth(output,0);
    like.ascii_insensitive=true;
    check_equal(orm_tidesdb_sql_like_eval(&like,&name,&pattern,&budget,&output,&error),TURBODB_STATUS_OK); expect_truth(output,1);
    like.negated=true;
    check_equal(orm_tidesdb_sql_like_eval(&like,&name,&pattern,&budget,&output,&error),TURBODB_STATUS_OK); expect_truth(output,0);
    check_equal(orm_tidesdb_sql_like_bind(text,text,'\\',false,&like,&error),TURBODB_STATUS_OK); check_false(like.ascii_insensitive);
  }

  it("propagates LIKE NULL and rejects conversions malformed views and non ASCII") {
    orm_sql_like like; const orm_sql_type text = {TURBODB_VALUE_TEXT, true};
    check_equal(orm_tidesdb_sql_like_bind(text, text, '\\', true, &like, &error), TURBODB_STATUS_OK);
    check_equal(like.result.nullable, true);
    turbodb_value_t a = turbodb_null(), b = turbodb_text("%"), out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_like_eval(&like, &a, &b, &budget, &out, &error), TURBODB_STATUS_OK); expect_truth(out, -1);
    a = turbodb_text("abc"); b = turbodb_null();
    check_equal(orm_tidesdb_sql_like_eval(&like, &a, &b, &budget, &out, &error), TURBODB_STATUS_OK); expect_truth(out, -1);
    const turbodb_value_t bad[] = {turbodb_text("\xc3\xa9"), turbodb_text("\xc0\x80"), turbodb_text_v((vstr){NULL, 1}), turbodb_i64(1)};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
      out = turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_like_eval(&like, &bad[i], &b, &budget, &out, &error),
          i == 0 ? TURBODB_STATUS_UNSUPPORTED : TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value, SENTINEL);
    }
    const orm_sql_type integer = {TURBODB_VALUE_INT64, false}, blob = {TURBODB_VALUE_BLOB, false};
    check_equal(orm_tidesdb_sql_like_bind(text, integer, '\\', false, &like, &error), TURBODB_STATUS_UNSUPPORTED);
    check_equal(orm_tidesdb_sql_like_bind(text, blob, '\\', false, &like, &error), TURBODB_STATUS_UNSUPPORTED);
    check_equal(orm_tidesdb_sql_like_bind(text, text, ORM_SQL_ASCII_MAX + 1, false, &like, &error), TURBODB_STATUS_INVALID_ARGUMENT);
  }

  it("bounds LIKE retries and validates length arithmetic before reading payloads") {
    orm_sql_like like; const orm_sql_type text = {TURBODB_VALUE_TEXT, false};
    check_equal(orm_tidesdb_sql_like_bind(text, text, -1, false, &like, &error), TURBODB_STATUS_OK);
    turbodb_value_t a = turbodb_text("aaaaaaaaaaaa"), b = turbodb_text("%aaaaab"), out = turbodb_i64(SENTINEL);
    enum { RETRY_BUDGET = 50 };
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK); start_budget(RETRY_BUDGET);
    check_equal(orm_tidesdb_sql_like_eval(&like, &a, &b, &budget, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.data.int64_value, SENTINEL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], RETRY_BUDGET);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK); start_budget(TEST_LIMIT);
    a = turbodb_text_v((vstr){"x", SIZE_MAX});
    check_equal(orm_tidesdb_sql_like_eval(&like, &a, &b, &budget, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 0u);
    check_equal(out.data.int64_value, SENTINEL);
  }

  it("agrees with a bounded prefix DP oracle for every short binary text and wildcard pattern") {
    enum { MAX_LENGTH = 4, TEXT_WORDS = 31, PATTERN_WORDS = 341, PATTERN_ALPHABET = 4 };
    const char alphabet[] = "ab%_";
    orm_sql_like like; const orm_sql_type type = {TURBODB_VALUE_TEXT, false};
    check_equal(orm_tidesdb_sql_like_bind(type, type, ORM_SQL_LIKE_NO_ESCAPE, false, &like, &error), TURBODB_STATUS_OK);
    for (unsigned word = 0; word < TEXT_WORDS; ++word) {
      check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK); start_budget(TEST_LIMIT);
      char text[MAX_LENGTH], pattern[MAX_LENGTH]; size_t n = 0;
      for (unsigned code = word; code; code = (code - 1) / 2) text[n++] = alphabet[(code - 1) % 2];
      for (unsigned pattern_word = 0; pattern_word < PATTERN_WORDS; ++pattern_word) {
        size_t m = 0;
        for (unsigned code = pattern_word; code; code = (code - 1) / PATTERN_ALPHABET)
          pattern[m++] = alphabet[(code - 1) % PATTERN_ALPHABET];
        bool dp[MAX_LENGTH + 1][MAX_LENGTH + 1] = {0}; dp[0][0] = true;
        for (size_t j = 1; j <= m; ++j) for (size_t i = 0; i <= n; ++i)
          dp[i][j] = pattern[j - 1] == '%' ? dp[i][j - 1] || (i && dp[i - 1][j]) :
              i && dp[i - 1][j - 1] && (pattern[j - 1] == '_' || pattern[j - 1] == text[i - 1]);
        turbodb_value_t a = turbodb_text_v((vstr){text, n}), b = turbodb_text_v((vstr){pattern, m}), out = turbodb_null();
        check_equal(orm_tidesdb_sql_like_eval(&like, &a, &b, &budget, &out, &error), TURBODB_STATUS_OK);
        expect_truth(out, dp[n][m]);
      }
    }
  }

  it("charges scalar predicates against the same remaining execution budget") {
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    start_budget(1);
    turbodb_value_t a = turbodb_i64(1), b = turbodb_u64(1), out = turbodb_i64(SENTINEL);
    const orm_sql_predicate predicate = bind_values(ORM_SQL_EQUAL, a, &b);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &b, &budget, &out, &error), TURBODB_STATUS_OK);
    expect_truth(out, 1);
    out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_predicate_eval(&predicate, &a, &b, &budget, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.kind, TURBODB_VALUE_INT64);
    check_equal(out.data.int64_value, SENTINEL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 1u);
  }
}
