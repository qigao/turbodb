#include "from.h"
#include "select.h"
#include <tinytest.h>
#include <string.h>

static size_t reserve_calls, resize_calls, fail_reserve, fail_resize;
static stl_status from_test_reserve(vec_t *v, size_t count) {
  return ++reserve_calls==fail_reserve?STL_OUT_OF_MEMORY:vec_reserve(v,count);
}
static stl_status from_test_resize(vec_t *v, size_t count) {
  return ++resize_calls==fail_resize?STL_OUT_OF_MEMORY:vec_resize(v,count);
}
#define vec_reserve from_test_reserve
#define vec_resize from_test_resize
#include "../../src/work.c"
#include "../../src/expr.c"
#include "../../src/binding.c"
#include "../../src/from.c"
#undef vec_reserve
#undef vec_resize

enum { TEST_LIMIT=65536, TEST_WORK=4*1024*1024, TEST_DEPTH=32, TEST_TABLES=3, TEST_COLUMNS=2, TEST_INPUTS=8 };
static orm_sql_from plan;
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static turbodb_error_t error;
static sqlparser_document *document;
static orm_sql_schema_column columns[TEST_TABLES][TEST_COLUMNS];
static orm_sql_table_schema schemas[TEST_TABLES];
static const orm_sql_table_schema *tables[TEST_TABLES];
static orm_sql_type parameters[TEST_INPUTS];
static orm_sql_select selected;
static orm_sql_select_run selected_run;
static orm_sql_join joined;
static orm_sql_from_run from_run;
static orm_sql_scan memory[2], reorder;
static orm_sql_row_source sources[3];
static orm_sql_type source_types[3][TEST_INPUTS];
static turbodb_value_t stored[2][2][TEST_COLUMNS];
typedef struct dependent_provider {
  size_t table,rows,position,opens,closes,rewinds,capture_count;
  turbodb_value_t values[2][TEST_COLUMNS],output[TEST_COLUMNS];
  const turbodb_value_t *borrowed;
  size_t borrowed_count;
  int64_t empty_key;
  bool lateral,bound,fail_open,fail_close,reenter,drift,saw_null,bare_error;
} dependent_provider;
static dependent_provider providers[TEST_TABLES];
static orm_sql_from_input dependent_inputs[TEST_TABLES];
static orm_sql_type capture_types[TEST_TABLES][TEST_INPUTS];
static void parse(const char *sql) {
  sqlparser_document_destroy(document); document=NULL; sqlparser_error e;
  const sqlparser_options options={SQLPARSER_MYSQL,false};
  check_equal(sqlparser_parse_with_options(sql,strlen(sql),&options,NULL,&document,&e),SQLPARSER_OK);
}
static turbodb_status_t bind_from(size_t count, size_t parameter_count) {
  return orm_tidesdb_sql_from_bind(document,tables,count,parameters,parameter_count,TEST_DEPTH,&budget,&plan,&error);
}
static orm_sql_query_scope from_scope(size_t count) {
  return (orm_sql_query_scope){.document=document,.root=sqlparser_statements(document).first,
      .parameter_types=parameters,.parameter_count=count,.max_depth=TEST_DEPTH,.budget=&budget};
}
static sqlparser_id from_root(void) {
  return sqlparser_get_node(document,sqlparser_statements(document).first)->as.select.from;
}
static sqlparser_id lateral_named(const char *alias) {
  sqlparser_id result=0;
  for(size_t i=1;i<=sqlparser_node_count(document);++i) {
    const sqlparser_node *node=sqlparser_get_node(document,(sqlparser_id)i);
    if(node->kind!=SQLPARSER_TABLE||!node->as.table.lateral) continue;
    const sqlparser_node *name=sqlparser_get_node(document,node->as.table.alias);
    if(name&&name->span.length==strlen(alias)&&
        !memcmp(sqlparser_text(document,name->span),alias,name->span.length)) result=(sqlparser_id)i;
  }
  check_not_equal(result,0u); return result;
}
static void clean(void) {
  fail_reserve=fail_resize=0;
  for(size_t i=0;i<TEST_TABLES;++i) providers[i].fail_close=false;
  check_equal(orm_tidesdb_sql_select_close(&selected_run,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_destroy(&selected,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_scan_close(&reorder,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_join_close(&joined,&error),TURBODB_STATUS_OK);
  for(size_t i=0;i<2;++i) check_equal(orm_tidesdb_sql_scan_close(&memory[i],&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_from_destroy(&plan,&error),TURBODB_STATUS_OK);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
}
static void reset(void) {
  clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
}
static void reject(const char *sql, size_t count, turbodb_status_t status, const char *message) {
  reset(); parse(sql); check_equal(bind_from(count,0),status); check_contains(error.message,message);
  check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
}
static turbodb_value_t evaluate(size_t node_index, const turbodb_value_t *row, size_t width,
    const turbodb_value_t *values, size_t parameter_count) {
  const orm_sql_from_node *node=orm_tidesdb_sql_from_at(&plan,node_index);
  turbodb_value_t inputs[TEST_INPUTS]; check_true(vec_size(&node->slots)<=TEST_INPUTS);
  for(size_t i=0;i<vec_size(&node->slots);++i) {
    const size_t slot=*(const size_t *)vec_at_const(&node->slots,i); check_true(slot<width+parameter_count);
    inputs[i]=slot<width?row[slot]:values[slot-width];
  }
  turbodb_value_t out=turbodb_null();
  check_equal(orm_tidesdb_sql_expr_eval((orm_sql_expr *)&node->condition,inputs,vec_size(&node->slots),&out,&error),TURBODB_STATUS_OK);
  return out;
}
static turbodb_status_t pull_scan(void *context, const turbodb_value_t **out, turbodb_error_t *e) {
  orm_sql_scan_row row; const turbodb_status_t status=orm_tidesdb_sql_scan_next(context,&row,e);
  if(status==TURBODB_STATUS_OK) *out=row.state==ORM_SQL_SCAN_ROW?row.values:NULL;
  return status;
}
static turbodb_status_t dependent_test_rewind(void *context,turbodb_error_t *e) {
  dependent_provider *provider=context; (void)e;
  check_false(sources[provider->table].active); ++provider->rewinds; provider->position=0;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependent_test_open(void *context,const turbodb_value_t *row,size_t count,turbodb_error_t *e) {
  dependent_provider *provider=context; ++provider->opens;
  check_false(provider->bound); check_false(sources[provider->table].active);
  check_equal(count,provider->capture_count);
  if(provider->reenter) {
    turbodb_error_t nested; tdsql_error_init(&nested);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&nested),TURBODB_STATUS_BUSY);
  }
  if(provider->bare_error) return TURBODB_STATUS_DATASTORE_ERROR;
  if(provider->fail_open) { tdsql_error_set(e,TURBODB_STATUS_LIMIT_EXCEEDED,"test dependent provider open failure"); return TURBODB_STATUS_LIMIT_EXCEEDED; }
  provider->borrowed=row; provider->borrowed_count=count; provider->position=0; provider->bound=true;
  if(provider->drift) source_types[provider->table][0].nullable=!source_types[provider->table][0].nullable;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependent_test_close(void *context,turbodb_error_t *e) {
  dependent_provider *provider=context; ++provider->closes;
  check_true(provider->bound); check_false(sources[provider->table].active);
  if(provider->reenter) {
    turbodb_error_t nested; tdsql_error_init(&nested);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&nested),TURBODB_STATUS_BUSY);
  }
  if(provider->fail_close) { tdsql_error_set(e,TURBODB_STATUS_BUSY,"test dependent provider close busy"); return TURBODB_STATUS_BUSY; }
  provider->borrowed=NULL; provider->borrowed_count=0; provider->bound=false;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependent_test_pull(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  dependent_provider *provider=context; (void)e;
  if(!provider->lateral) {
    *out=provider->position<provider->rows?provider->values[provider->position++]:NULL;
    return TURBODB_STATUS_OK;
  }
  check_true(provider->bound);
  if(provider->position>=provider->rows||(provider->borrowed_count&&provider->empty_key&&
      provider->borrowed[0].data.int64_value==provider->empty_key)) { *out=NULL; return TURBODB_STATUS_OK; }
  for(size_t column=0;column<TEST_COLUMNS;++column) {
    provider->output[column]=turbodb_i64((int64_t)provider->position);
    for(size_t slot=column;slot<provider->borrowed_count;slot+=TEST_COLUMNS) {
      if(provider->borrowed[slot].kind==TURBODB_VALUE_NULL) {
        provider->saw_null=true; provider->output[column]=turbodb_null(); break;
      }
      provider->output[column].data.int64_value+=provider->borrowed[slot].data.int64_value;
    }
  }
  ++provider->position; *out=provider->output; return TURBODB_STATUS_OK;
}
static void dependent_prepare(const char *sql,size_t count,orm_sql_query_scope *scope) {
  const orm_sql_table_schema *outer=scope->outer_schema; const vstr qualifier=scope->outer_qualifier;
  parse(sql); *scope=from_scope(scope->parameter_count); scope->outer_schema=outer; scope->outer_qualifier=qualifier;
  check_equal(orm_sql_from_subtree_schema_at(scope,from_root(),tables,count,&plan,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_from_bind_conditions_at(scope,&plan,&error),TURBODB_STATUS_OK);
  memset(providers,0,sizeof(providers)); memset(dependent_inputs,0,sizeof(dependent_inputs));
  for(size_t i=0;i<plan.count;++i) {
    const orm_sql_from_node *node=orm_tidesdb_sql_from_at(&plan,i); if(!node->leaf) continue;
    const size_t table=node->table; dependent_provider *provider=&providers[table];
    provider->table=table; provider->lateral=node->lateral; provider->rows=node->lateral?1:2;
    memcpy(provider->values,stored[table?1:0],sizeof(provider->values));
    for(size_t j=0;j<TEST_COLUMNS;++j) source_types[table][j]=columns[table][j].type;
    sources[table]=(orm_sql_row_source){&budget,source_types[table],TEST_COLUMNS,provider,dependent_test_pull,false};
    dependent_inputs[table]=(orm_sql_from_input){.source=&sources[table],.context=provider};
    if(node->lateral) {
      vec_t prefixes={0}; size_t bytes=0,width=0;
      check_equal(orm_sql_from_lateral_prefixes_at(scope,node->ast,&prefixes,&bytes,&error),TURBODB_STATUS_OK);
      for(size_t prefix=0;prefix<vec_size(&prefixes);++prefix) {
        const sqlparser_id ast=*(const sqlparser_id *)vec_at_const(&prefixes,prefix);
        const orm_sql_from_node *bound=NULL;
        for(size_t index=0;index<plan.count;++index) if(orm_tidesdb_sql_from_at(&plan,index)->ast==ast) bound=orm_tidesdb_sql_from_at(&plan,index);
        check_not_null(bound); check_true(width+bound->schema.count<=TEST_INPUTS);
        for(size_t column=0;column<bound->schema.count;++column) capture_types[table][width++]=bound->schema.columns[column].type;
      }
      if(outer) for(size_t column=0;column<outer->count;++column) {
        check_true(width<TEST_INPUTS); capture_types[table][width++]=outer->columns[column].type;
      }
      check_equal(orm_sql_work_release(&prefixes,bytes,&budget,&error),TURBODB_STATUS_OK);
      dependent_inputs[table].binding=(orm_sql_join_right_binding){provider,dependent_test_open,dependent_test_close};
      dependent_inputs[table].capture_count=width; dependent_inputs[table].capture_types=capture_types[table];
      provider->capture_count=width;
    } else dependent_inputs[table].rewind=dependent_test_rewind;
  }
}
static turbodb_status_t dependent_open(size_t count,const turbodb_value_t *values,size_t parameters_count) {
  return orm_sql_from_open_dependent(&plan,dependent_inputs,count,values,parameters_count,NULL,&from_run,&error);
}
static void dependent_row(const int64_t *expected,size_t width) {
  const turbodb_value_t *row=NULL;
  check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_OK); check_not_null(row);
  for(size_t i=0;i<width;++i) { check_equal(row[i].kind,TURBODB_VALUE_INT64); check_equal(row[i].data.int64_value,expected[i]); }
}
static void dependent_done(void) {
  const turbodb_value_t *row=NULL;
  check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_OK); check_null(row);
}
static void dependent_nullable_row(const int64_t *expected,size_t width,unsigned nulls) {
  const turbodb_value_t *row=NULL;
  check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_OK); check_not_null(row);
  for(size_t i=0;i<width;++i) {
    check_equal(row[i].kind,(nulls&(1u<<i))?TURBODB_VALUE_NULL:TURBODB_VALUE_INT64);
    if(!(nulls&(1u<<i))) check_equal(row[i].data.int64_value,expected[i]);
  }
}
static turbodb_status_t dependent_drain(size_t *rows) {
  *rows=0;
  for(;;) {
    const turbodb_value_t *row=NULL;
    const turbodb_status_t status=from_run.source->next(from_run.source->context,&row,&error);
    if(status!=TURBODB_STATUS_OK||!row) return status;
    ++*rows;
  }
}
static void dependent_allowance(void) {
  for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) {
    if(i==ORM_SQL_BUDGET_WORK_BYTES) continue;
    check_true(budget.used.value[i]<=UINT64_MAX-TEST_LIMIT);
    budget.limits.statement.value[i]=budget.used.value[i]+TEST_LIMIT;
  }
}
static void open_inputs(void) {
  const size_t projection[]={0,1}; const orm_sql_scan_spec scan={.projection=projection,.projection_count=2,.limit=UINT64_MAX};
  for(size_t i=0;i<2;++i) {
    for(size_t j=0;j<TEST_COLUMNS;++j) source_types[i][j]=schemas[i].columns[j].type;
    const orm_sql_memory_source input={&stored[i][0][0],2,TEST_COLUMNS,source_types[i]};
    check_equal(orm_tidesdb_sql_scan_open(&input,&scan,&budget,&memory[i],&error),TURBODB_STATUS_OK);
    sources[i]=(orm_sql_row_source){&budget,source_types[i],TEST_COLUMNS,&memory[i],pull_scan,false};
  }
}
static void open_pipeline(const turbodb_value_t *values, size_t count) {
  open_inputs();
  orm_sql_row_source *inputs[]={&sources[0],&sources[1]};
  check_equal(orm_tidesdb_sql_from_open(&plan,inputs,2,values,count,&from_run,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_open_source(&selected,from_run.source,values,count,&selected_run,&error),TURBODB_STATUS_OK);
}
static orm_sql_scan_row selected_next(void) {
  orm_sql_scan_row row; check_equal(orm_tidesdb_sql_scan_next(&selected_run.scan,&row,&error),TURBODB_STATUS_OK); return row;
}

spec("TidesDB FROM scope and join predicate binding") {
  before_each() {
    tdsql_error_init(&error); document=NULL; plan=(orm_sql_from){0};
    from_run=(orm_sql_from_run){0}; selected=(orm_sql_select){0}; selected_run=(orm_sql_select_run){0}; joined=(orm_sql_join){0};
    memset(memory,0,sizeof(memory)); reorder=(orm_sql_scan){0}; memset(sources,0,sizeof(sources));
    memset(providers,0,sizeof(providers)); memset(dependent_inputs,0,sizeof(dependent_inputs));
    reserve_calls=resize_calls=fail_reserve=fail_resize=0; limits=(orm_sql_budget_limits){0};
    for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=TEST_WORK;
    limits.transaction=(orm_sql_transaction_budget_amount){TEST_LIMIT,TEST_LIMIT,TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
    const char *names[]={"a","b","c"}, *second[]={"x","y","z"};
    for(size_t i=0;i<TEST_TABLES;++i) {
      columns[i][0]=(orm_sql_schema_column){vstr_from_cstr("id"),{TURBODB_VALUE_INT64,false}};
      columns[i][1]=(orm_sql_schema_column){vstr_from_cstr(second[i]),{TURBODB_VALUE_INT64,false}};
      schemas[i]=(orm_sql_table_schema){vstr_from_cstr(names[i]),columns[i],TEST_COLUMNS}; tables[i]=&schemas[i];
    }
    for(size_t i=0;i<TEST_INPUTS;++i) parameters[i]=(orm_sql_type){TURBODB_VALUE_INT64,false};
    stored[0][0][0]=turbodb_i64(1); stored[0][0][1]=turbodb_i64(10); stored[0][1][0]=turbodb_i64(2); stored[0][1][1]=turbodb_i64(20);
    stored[1][0][0]=turbodb_i64(2); stored[1][0][1]=turbodb_i64(200); stored[1][1][0]=turbodb_i64(3); stored[1][1][1]=turbodb_i64(300);
  }
  after_each() { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); sqlparser_document_destroy(document); }

  group("USING and NATURAL names and output") {
    it("coalesces outer common columns and orders stars by the preserved side") {
      const char *sql[]={"SELECT * FROM a LEFT JOIN b USING(id) ORDER BY id", "SELECT * FROM a NATURAL LEFT JOIN b ORDER BY id",
        "SELECT * FROM a RIGHT JOIN b USING(id) ORDER BY id", "SELECT * FROM a NATURAL RIGHT JOIN b ORDER BY id"};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        reset(); parse(sql[mode]); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK);
        check_equal(vec_size(&selected.columns),3u);
        const orm_sql_select_column *label=vec_data_const(&selected.columns);
        check_equal(strcmp(label[0].name,"id"),0); check_equal(strcmp(label[1].name,mode<2?"x":"y"),0);
        open_pipeline(NULL,0); sqlparser_document_destroy(document); document=NULL;
        orm_sql_scan_row row=selected_next(); check_equal(row.count,3u);
        check_equal(row.values[0].data.int64_value,mode<2?1:2); check_equal(row.values[1].data.int64_value,mode<2?10:200);
        if(mode<2) check_equal(row.values[2].kind,TURBODB_VALUE_NULL); else check_equal(row.values[2].data.int64_value,20);
        row=selected_next(); check_equal(row.values[0].data.int64_value,mode<2?2:3);
        check_equal(row.values[1].data.int64_value,mode<2?20:300);
        if(mode<2) check_equal(row.values[2].data.int64_value,200); else check_equal(row.values[2].kind,TURBODB_VALUE_NULL);
        check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("preserves qualified original values and the original table star order") {
      parse("SELECT id,a.*,b.id AS bid FROM a RIGHT JOIN b USING(id) ORDER BY id"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
      /* The repeated id label requires an explicit label in this driver's result contract. */
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_SQL_ERROR);
      reset(); parse("SELECT id AS merged,a.*,b.id AS bid FROM a RIGHT JOIN b USING(id) ORDER BY merged"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK); open_pipeline(NULL,0);
      orm_sql_scan_row row=selected_next(); check_equal(row.count,4u); check_equal(row.values[0].data.int64_value,2);
      check_equal(row.values[1].data.int64_value,2); check_equal(row.values[2].data.int64_value,20); check_equal(row.values[3].data.int64_value,2);
      row=selected_next(); check_equal(row.values[0].data.int64_value,3); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      check_equal(row.values[2].kind,TURBODB_VALUE_NULL); check_equal(row.values[3].data.int64_value,3);
      check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
    }
    it("places common columns before independent columns even when their physical slot is last") {
      columns[0][0].name=vstr_from_cstr("x"); columns[0][1].name=vstr_from_cstr("id");
      columns[1][0].name=vstr_from_cstr("y"); columns[1][1].name=vstr_from_cstr("id");
      stored[0][0][0]=turbodb_i64(10); stored[0][0][1]=turbodb_i64(1); stored[0][1][0]=turbodb_i64(20); stored[0][1][1]=turbodb_i64(2);
      stored[1][0][0]=turbodb_i64(200); stored[1][0][1]=turbodb_i64(2); stored[1][1][0]=turbodb_i64(300); stored[1][1][1]=turbodb_i64(3);
      parse("SELECT * FROM a JOIN b USING(id)"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK); open_pipeline(NULL,0);
      const orm_sql_scan_row row=selected_next(); check_equal(row.count,3u); check_equal(row.values[0].data.int64_value,2);
      check_equal(row.values[1].data.int64_value,20); check_equal(row.values[2].data.int64_value,200); check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
    }
    it("uses all common keys and ignores USING list order for output ordering") {
      columns[1][1].name=vstr_from_cstr("x"); stored[1][0][1]=turbodb_i64(20);
      parse("SELECT * FROM a JOIN b USING(x,id)"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_from_at(&plan,0)->key_count,2u);
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK); open_pipeline(NULL,0);
      const orm_sql_scan_row row=selected_next(); check_equal(row.count,2u); check_equal(row.values[0].data.int64_value,2);
      check_equal(row.values[1].data.int64_value,20); check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
    }
    it("treats NATURAL without common columns as an unconditional join") {
      columns[1][0].name=vstr_from_cstr("other_id"); parse("SELECT * FROM a NATURAL RIGHT JOIN b ORDER BY other_id,id");
      check_equal(bind_from(2,0),TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_from_at(&plan,0)->key_count,0u);
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK); open_pipeline(NULL,0);
      for(size_t i=0;i<4;++i) { const orm_sql_scan_row row=selected_next(); check_equal(row.count,4u);
        check_equal(row.values[0].data.int64_value,2+(int64_t)(i/2)); check_equal(row.values[2].data.int64_value,1+(int64_t)(i%2)); }
      check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
    }
    it("groups by the visible common name and keeps hidden qualified group keys distinct") {
      parse("SELECT id,COUNT(*) AS n FROM a LEFT JOIN b USING(id) GROUP BY id ORDER BY id"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK); open_pipeline(NULL,0);
      for(size_t i=0;i<2;++i) { const orm_sql_scan_row row=selected_next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1); check_equal(row.values[1].data.int64_value,1); }
      check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
      reset(); parse("SELECT id,b.id AS bid,COUNT(*) AS n FROM a LEFT JOIN b USING(id) GROUP BY id,b.id ORDER BY id");
      check_equal(bind_from(2,0),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK); open_pipeline(NULL,0);
      for(size_t i=0;i<2;++i) { const orm_sql_scan_row row=selected_next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
        check_equal(row.values[1].kind,i?TURBODB_VALUE_INT64:TURBODB_VALUE_NULL); check_equal(row.values[2].data.uint64_value,1u); }
      check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
    }
    it("rejects ambiguous operands and mismatched common types before publishing a plan") {
      parse("SELECT * FROM a JOIN b ON TRUE JOIN c USING(id)");
      check_equal(bind_from(3,0),TURBODB_STATUS_SQL_ERROR); check_contains(error.message,"ambiguous");
      check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      reset(); columns[1][0].type.kind=TURBODB_VALUE_UINT64; parse("SELECT * FROM a NATURAL JOIN b");
      check_equal(bind_from(2,0),TURBODB_STATUS_TYPE_ERROR); check_null(plan.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
    it("merges repeated common names through a RIGHT chain and an ON parent") {
      parse("SELECT id,x,y,z FROM a LEFT JOIN b USING(id) RIGHT JOIN c USING(id)");
      check_equal(bind_from(3,0),TURBODB_STATUS_OK);
      const orm_sql_table_schema *schema=&orm_tidesdb_sql_from_at(&plan,0)->schema; size_t visible=0;
      for(size_t i=0;i<schema->count;++i) visible+=!schema->columns[i].qualified_only;
      check_equal(visible,4u); check_equal(schema->columns[4].star_order,1u);
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK);
      reset(); parse("SELECT id,x,y,z FROM a LEFT JOIN b USING(id) JOIN c ON id=c.id");
      check_equal(bind_from(3,0),TURBODB_STATUS_SQL_ERROR); check_contains(error.message,"ambiguous");
      reset(); parse("SELECT a.id,x,y,z FROM a LEFT JOIN b USING(id) JOIN c ON a.id=c.id");
      check_equal(bind_from(3,0),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK);
    }
    it("refunds every common binding allocation and rejects every insufficient step allowance") {
      const char *sql[]={"SELECT * FROM a NATURAL RIGHT JOIN b JOIN c USING(id)",
        "SELECT * FROM a NATURAL JOIN b"};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        reset(); parse(sql[mode]); reserve_calls=resize_calls=0;
        check_equal(bind_from(mode?2:3,0),TURBODB_STATUS_OK);
        const size_t counts[]={reserve_calls,resize_calls}; const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        for(size_t phase=0;phase<2;++phase) for(size_t point=1;point<=counts[phase];++point) {
          reset(); reserve_calls=resize_calls=0; if(phase) fail_resize=point; else fail_reserve=point;
          check_equal(bind_from(mode?2:3,0),TURBODB_STATUS_OUT_OF_MEMORY); check_null(plan.budget);
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
        }
        for(uint64_t point=0;point<steps;++point) {
          reset(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
          check_equal(bind_from(mode?2:3,0),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(plan.budget);
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
        }
      }
    }
    it("matches dependent USING keys after each LATERAL reopen") {
      orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a LEFT JOIN LATERAL (SELECT a.id) b USING(id)",2,&scope);
      check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK); const int64_t first[]={1,10,1,10}, second[]={2,20,2,20};
      dependent_row(first,4); dependent_row(second,4); dependent_done(); check_equal(providers[1].opens,2u);
    }
  }
  it("binds explicit derived aliases over duplicate supplied output names and owns them") {
    schemas[0].name=vstr_from_cstr("d"); columns[0][1].name=columns[0][0].name;
    parse("SELECT d.x,d.y FROM (SELECT 1,2) d(x,y)");
    check_equal(bind_from(1,0),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL;
    const orm_sql_from_node *leaf=orm_tidesdb_sql_from_at(&plan,0);
    check_equal(leaf->schema.count,2u);
    check_equal(leaf->schema.columns[0].name.len,1u); check_equal(leaf->schema.columns[0].name.data[0],'x');
    check_equal(leaf->schema.columns[1].name.len,1u); check_equal(leaf->schema.columns[1].name.data[0],'y');
    for(size_t i=0;i<2;++i) {
      check_equal(leaf->schema.columns[i].type.kind,TURBODB_VALUE_INT64);
      check_false(leaf->schema.columns[i].type.nullable);
    }
  }
  it("rejects explicit derived schema width mismatches and repeated exposed names") {
    schemas[0].name=vstr_from_cstr("d");
    reject("SELECT * FROM (SELECT 1,2) d(x)",1,TURBODB_STATUS_SQL_ERROR,"width");
    reject("SELECT * FROM (SELECT 1,2) d(x,x)",1,TURBODB_STATUS_SQL_ERROR,"duplicate");
  }
  it("lazily reopens a dependent leaf for each left row after the AST dies") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b",2,&scope);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    orm_sql_row_source *ordinary[]={&sources[0],&sources[1]};
    check_equal(orm_tidesdb_sql_from_open(&plan,ordinary,2,NULL,0,&from_run,&error),TURBODB_STATUS_UNSUPPORTED);
    check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
    check_equal(providers[0].rewinds,0u); check_equal(providers[1].opens,0u);
    sqlparser_document_destroy(document); document=NULL;
    const int64_t first[]={1,10,1,10},second[]={2,20,2,20};
    dependent_row(first,4); dependent_row(second,4); dependent_done();
    check_equal(providers[0].rewinds,1u); check_equal(providers[1].opens,2u); check_equal(providers[1].closes,2u);
    check_false(providers[1].bound);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
    check_equal(plan.active_runs,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
  }
  it("null extends an empty dependent LEFT round and opens the next left row") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a LEFT JOIN LATERAL (SELECT a.id) b ON TRUE",2,&scope);
    providers[1].empty_key=1; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
    const int64_t first[]={1,10,0,0},second[]={2,20,2,20};
    dependent_nullable_row(first,4,(1u<<2)|(1u<<3)); dependent_row(second,4); dependent_done();
    check_equal(providers[1].opens,2u); check_equal(providers[1].closes,2u);
  }
  it("feeds the complete preceding join row to consecutive dependent leaves") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b,LATERAL (SELECT b.id) c",3,&scope);
    check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_OK);
    const int64_t first[]={1,10,1,10,2,20},second[]={2,20,2,20,4,40};
    dependent_row(first,6); dependent_row(second,6); dependent_done();
    for(size_t i=1;i<TEST_TABLES;++i) { check_equal(providers[i].opens,2u); check_equal(providers[i].closes,2u); }
  }
  it("does not open a later dependent leaf for an empty preceding round") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b,LATERAL (SELECT b.id) c",3,&scope);
    providers[1].empty_key=1; check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_OK);
    const int64_t row[]={2,20,2,20,4,40}; dependent_row(row,6); dependent_done();
    check_equal(providers[1].opens,2u); check_equal(providers[2].opens,1u); check_equal(providers[2].closes,1u);
  }
  it("restores logical output order when a RIGHT join supplies a dependent prefix") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM LATERAL (SELECT b.id) a RIGHT JOIN b ON TRUE",2,&scope);
    check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
    const int64_t first[]={2,200,2,200},second[]={3,300,3,300};
    dependent_row(first,4); dependent_row(second,4); dependent_done();
    check_equal(providers[0].opens,2u); check_equal(providers[1].rewinds,1u);
  }
  it("reopens the entire dependent subtree under an ancestor RIGHT join") {
    orm_sql_query_scope scope={0};
    dependent_prepare("SELECT * FROM a JOIN LATERAL (SELECT a.id+c.id) b ON TRUE RIGHT JOIN c ON TRUE",3,&scope);
    check_equal(dependent_inputs[1].capture_count,4u); check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_OK);
    const int64_t rows[][6]={{1,10,3,210,2,200},{2,20,4,220,2,200},{1,10,4,310,3,300},{2,20,5,320,3,300}};
    for(size_t i=0;i<sizeof(rows)/sizeof(rows[0]);++i) dependent_row(rows[i],6);
    dependent_done(); check_equal(providers[0].rewinds,2u); check_equal(providers[1].opens,4u); check_equal(providers[1].closes,4u);
  }
  it("preserves completed LEFT prefix null extension in dependent captures") {
    for(size_t i=0;i<TEST_COLUMNS;++i) columns[2][i].type.nullable=true;
    orm_sql_query_scope scope={0};
    dependent_prepare("SELECT * FROM a LEFT JOIN b ON a.id=b.id JOIN LATERAL (SELECT a.id+b.id) c ON TRUE",3,&scope);
    check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_OK);
    const int64_t first[]={1,10,0,0,0,0},second[]={2,20,2,200,4,220};
    dependent_nullable_row(first,6,(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)); dependent_row(second,6); dependent_done();
    check_true(providers[2].saw_null); check_equal(providers[2].opens,2u);
  }
  it("does not rewind or open the right subtree when the outer left is empty") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b",2,&scope);
    providers[0].rows=0; providers[1].fail_open=true; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
    dependent_done(); dependent_done(); check_equal(providers[0].rewinds,1u); check_equal(providers[1].opens,0u);
  }
  it("owns marker and inherited outer values used by ON and dependent captures") {
    const orm_sql_schema_column outer_columns[]={{vstr_from_cstr("id"),{TURBODB_VALUE_INT64,false}},
        {vstr_from_cstr("x"),{TURBODB_VALUE_INT64,false}}};
    const orm_sql_table_schema outer={vstr_from_cstr("ancestor"),outer_columns,TEST_COLUMNS};
    orm_sql_query_scope scope={.parameter_count=1,.outer_schema=&outer,.outer_qualifier=vstr_from_cstr("ancestor")};
    dependent_prepare("SELECT * FROM a JOIN LATERAL (SELECT a.id) b ON a.id+?=b.id",2,&scope);
    turbodb_value_t values[]={turbodb_i64(100),turbodb_i64(100),turbodb_i64(1000)};
    check_equal(dependent_open(2,values,3),TURBODB_STATUS_OK); memset(values,0,sizeof(values));
    memset(capture_types,0,sizeof(capture_types)); memset(dependent_inputs,0,sizeof(dependent_inputs));
    const int64_t first[]={1,10,101,1010},second[]={2,20,102,1020};
    dependent_row(first,4); dependent_row(second,4); dependent_done();
  }
  it("retains the left row when a dependent round cannot close and permits cleanup retry") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b",2,&scope);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
    const int64_t first[]={1,10,1,10}; dependent_row(first,4); providers[1].fail_close=true;
    const turbodb_value_t *sentinel=&stored[0][1][0],*row=sentinel;
    check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_BUSY); check_true(row==sentinel);
    check_true(sources[0].active); check_true(providers[1].bound); check_equal(providers[1].borrowed[0].data.int64_value,1);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; const size_t closes=providers[1].closes;
    check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_BUSY);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps); check_equal(providers[1].closes,closes);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_BUSY);
    check_true(sources[0].active); check_equal(plan.active_runs,1u);
    providers[1].fail_close=false; check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
    check_false(sources[0].active); check_null(providers[1].borrowed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
  }
  it("blocks pulls after an explicit close failure without losing the provider borrow") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b",2,&scope);
    check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK); const int64_t first[]={1,10,1,10}; dependent_row(first,4);
    providers[1].fail_close=true; check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_BUSY);
    const turbodb_value_t *row=NULL;
    check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(providers[1].borrowed[0].data.int64_value,1); check_true(sources[0].active);
    providers[1].fail_close=false; check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
  }
  it("rejects reentrant close during root provider open and close without a JOIN parent") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM LATERAL (SELECT 1 AS id) a",1,&scope);
    providers[0].reenter=true; check_equal(dependent_open(1,NULL,0),TURBODB_STATUS_OK);
    const int64_t expected[]={0,0}; dependent_row(expected,2);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
    check_equal(providers[0].opens,1u); check_equal(providers[0].closes,1u);
  }
  it("latches a provider error even when the provider supplies only the return code") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM LATERAL (SELECT 1 AS id) a",1,&scope);
    providers[0].bare_error=true; check_equal(dependent_open(1,NULL,0),TURBODB_STATUS_OK);
    const turbodb_value_t *row=&stored[0][0][0];
    check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(error.status,TURBODB_STATUS_DATASTORE_ERROR); check_true(row==&stored[0][0][0]);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; providers[0].bare_error=false;
    check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(providers[0].opens,1u); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
  }
  it("closes an opened provider after schema drift without publishing a row") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b",2,&scope);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    providers[1].drift=true; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
    const turbodb_value_t *row=&stored[0][0][0];
    check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_TYPE_ERROR);
    check_true(row==&stored[0][0][0]); check_true(providers[1].bound); check_true(sources[0].active);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
    check_equal(providers[1].closes,1u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
  }
  it("keeps a failed provider open terminal and releases retained ancestor operators") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b",2,&scope);
    providers[1].fail_open=true; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
    const turbodb_value_t *row=NULL;
    check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_false(providers[1].bound); check_true(sources[0].active); providers[1].fail_open=false;
    check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(providers[1].opens,1u); check_equal(providers[1].closes,0u);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK); check_false(sources[0].active);
  }
  it("validates dependent callbacks captures and independent sources before execution") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b",2,&scope);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const orm_sql_from_input original=dependent_inputs[1];
    dependent_inputs[1].binding.close=NULL; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_INVALID_ARGUMENT);
    dependent_inputs[1]=original; dependent_inputs[1].capture_count=1; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_INVALID_ARGUMENT);
    dependent_inputs[1]=original; capture_types[1][0].nullable=true; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_TYPE_ERROR);
    capture_types[1][0].nullable=false; dependent_inputs[1].source=&sources[0]; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_INVALID_ARGUMENT);
    dependent_inputs[1]=original; dependent_inputs[0].rewind=NULL; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_INVALID_ARGUMENT);
    dependent_inputs[0].rewind=dependent_test_rewind; sources[1].context=NULL; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(providers[0].rewinds,0u); check_equal(providers[1].opens,0u); check_equal(plan.active_runs,0u);
    check_null(from_run.plan); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
  }
  it("rejects incomplete conditions subtree plans and oversized registries before provider calls") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b",2,&scope);
    plan.conditions_bound=false; check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_INVALID_STATE); plan.conditions_bound=true;
    const sqlparser_id root=plan.root; plan.root=orm_tidesdb_sql_from_at(&plan,1)->ast;
    check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_UNSUPPORTED); plan.root=root;
    orm_sql_expr_query_source *dummy=NULL; const orm_sql_expr_query_sources queries={&dummy,TEST_LIMIT+1};
    check_equal(orm_sql_from_open_dependent(&plan,dependent_inputs,2,NULL,0,&queries,&from_run,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(providers[1].opens,0u); check_equal(plan.active_runs,0u);
  }
  it("holds the FROM owner while a downstream SELECT consumes its dependent source") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT a.id+b.id AS n FROM a,LATERAL (SELECT a.id) b",2,&scope);
    check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK);
    check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_open_source(&selected,from_run.source,NULL,0,&selected_run,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_BUSY);
    sqlparser_document_destroy(document); document=NULL;
    check_equal(selected_next().values[0].data.int64_value,2); check_equal(selected_next().values[0].data.int64_value,4);
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_select_close(&selected_run,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
  }
  it("refunds every dependent FROM constructor allocation without invoking providers") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b,LATERAL (SELECT b.id) c",3,&scope);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; reserve_calls=resize_calls=0;
    check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_OK); const size_t reserves=reserve_calls,resizes=resize_calls;
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
    for(size_t i=1;i<=reserves+resizes;++i) {
      reserve_calls=resize_calls=0; fail_reserve=i<=reserves?i:0; fail_resize=i>reserves?i-reserves:0;
      check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_OUT_OF_MEMORY); check_null(from_run.plan);
      check_equal(plan.active_runs,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      check_equal(providers[1].opens,0u); check_equal(providers[2].opens,0u);
    }
    fail_reserve=fail_resize=0;
  }
  it("refunds every lazy dependent FROM activation allocation and source lease") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a JOIN LATERAL (SELECT a.id+c.id) b ON TRUE RIGHT JOIN c ON TRUE",3,&scope);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_OK); reserve_calls=resize_calls=0;
    const turbodb_value_t *row=NULL; check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_OK);
    const size_t reserves=reserve_calls,resizes=resize_calls; check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
    for(size_t i=1;i<=reserves+resizes;++i) {
      check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_OK);
      reserve_calls=resize_calls=0; fail_reserve=i<=reserves?i:0; fail_resize=i>reserves?i-reserves:0;
      row=&stored[0][0][0]; check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_true(row==&stored[0][0][0]); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
      fail_reserve=fail_resize=0; check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
      for(size_t j=0;j<TEST_TABLES;++j) { check_false(sources[j].active); check_false(providers[j].bound); }
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
    }
  }
  it("enforces every dependent constructor and first-row execution step boundary") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a JOIN LATERAL (SELECT a.id+c.id) b ON TRUE RIGHT JOIN c ON TRUE",3,&scope);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_OK);
    const uint64_t construction=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    const turbodb_value_t *row=NULL; const uint64_t opened=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_OK);
    const uint64_t execution=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-opened;
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
    for(uint64_t allowance=0;allowance<construction;++allowance) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(from_run.plan);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained); check_equal(plan.active_runs,0u);
    }
    for(uint64_t allowance=0;allowance<execution;++allowance) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT;
      check_equal(dependent_open(3,NULL,0),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      row=&stored[0][0][0]; check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_true(row==&stored[0][0][0]); check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      for(size_t j=0;j<TEST_TABLES;++j) { check_false(sources[j].active); check_false(providers[j].bound); }
    }
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT;
  }
  it("refunds every later dependent round allocation after previously returned rows") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b",2,&scope);
    providers[1].rows=2; const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK); reserve_calls=resize_calls=0;
    size_t rows=0; check_equal(dependent_drain(&rows),TURBODB_STATUS_OK); check_equal(rows,4u);
    const size_t reserves=reserve_calls,resizes=resize_calls;
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
    for(size_t i=1;i<=reserves+resizes;++i) {
      dependent_allowance(); check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
      reserve_calls=resize_calls=0; fail_reserve=i<=reserves?i:0; fail_resize=i>reserves?i-reserves:0;
      check_equal(dependent_drain(&rows),TURBODB_STATUS_OUT_OF_MEMORY); check_true(rows<4);
      fail_reserve=fail_resize=0; check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
      check_false(providers[1].bound); check_null(providers[1].borrowed);
      check_false(sources[0].active); check_false(sources[1].active);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    }
  }
  it("enforces every execution step through the final dependent round and EOF") {
    orm_sql_query_scope scope={0}; dependent_prepare("SELECT * FROM a,LATERAL (SELECT a.id) b",2,&scope);
    providers[1].rows=2; const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; size_t rows=0;
    check_equal(dependent_drain(&rows),TURBODB_STATUS_OK); check_equal(rows,4u);
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
    for(uint64_t allowance=0;allowance<cost;++allowance) {
      dependent_allowance(); check_equal(dependent_open(2,NULL,0),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      check_equal(dependent_drain(&rows),TURBODB_STATUS_LIMIT_EXCEEDED);
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; const turbodb_value_t *row=NULL;
      check_equal(from_run.source->next(from_run.source->context,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
      check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
      check_false(providers[1].bound); check_null(providers[1].borrowed);
    }
  }
  it("discovers maximal completed lateral prefixes without including later sources") {
    parse("SELECT d.id FROM a LEFT JOIN b ON a.id=b.id JOIN LATERAL (SELECT a.id) d ON TRUE JOIN c ON TRUE");
    const sqlparser_id join=sqlparser_get_node(document,from_root())->as.join.left;
    const sqlparser_id expected=sqlparser_get_node(document,join)->as.join.left;
    orm_sql_query_scope scope=from_scope(0); vec_t prefixes={0}; size_t bytes=0;
    check_equal(orm_sql_from_lateral_prefixes_at(&scope,lateral_named("d"),&prefixes,&bytes,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&prefixes),1u); check_equal(*(const sqlparser_id *)vec_at_const(&prefixes,0),expected);
    check_equal(orm_sql_work_release(&prefixes,bytes,&budget,&error),TURBODB_STATUS_OK);
  }
  it("uses the right operand as the prefix of a left lateral RIGHT JOIN") {
    parse("SELECT d.id FROM LATERAL (SELECT b.id) d RIGHT JOIN b ON TRUE");
    const sqlparser_id expected=sqlparser_get_node(document,from_root())->as.join.right;
    orm_sql_query_scope scope=from_scope(0); vec_t prefixes={0}; size_t bytes=0;
    check_equal(orm_sql_from_lateral_prefixes_at(&scope,lateral_named("d"),&prefixes,&bytes,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&prefixes),1u); check_equal(*(const sqlparser_id *)vec_at_const(&prefixes,0),expected);
    check_equal(orm_sql_work_release(&prefixes,bytes,&budget,&error),TURBODB_STATUS_OK);
  }
  it("excludes the future left operand when RIGHT JOIN contextualizes lateral first") {
    parse("SELECT d.id FROM a RIGHT JOIN LATERAL (SELECT 1 AS id) d ON TRUE");
    orm_sql_query_scope scope=from_scope(0); vec_t prefixes={0}; size_t bytes=0;
    check_equal(orm_sql_from_lateral_prefixes_at(&scope,lateral_named("d"),&prefixes,&bytes,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&prefixes),0u); check_equal(bytes,0u); check_false(prefixes.initialized);
  }
  it("keeps multiple lateral prefixes in contextualization order across ancestor RIGHT joins") {
    parse("SELECT d.id FROM a JOIN LATERAL (SELECT a.id+c.id AS id) d ON TRUE RIGHT JOIN c ON TRUE");
    const sqlparser_node *root=sqlparser_get_node(document,from_root());
    const sqlparser_id expected[]={root->as.join.right,sqlparser_get_node(document,root->as.join.left)->as.join.left};
    orm_sql_query_scope scope=from_scope(0); vec_t prefixes={0}; size_t bytes=0;
    check_equal(orm_sql_from_lateral_prefixes_at(&scope,lateral_named("d"),&prefixes,&bytes,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&prefixes),sizeof(expected)/sizeof(expected[0]));
    for(size_t i=0;i<vec_size(&prefixes);++i)
      check_equal(*(const sqlparser_id *)vec_at_const(&prefixes,i),expected[i]);
    check_equal(orm_sql_work_release(&prefixes,bytes,&budget,&error),TURBODB_STATUS_OK);
  }
  it("includes preceding lateral output as one completed source subtree") {
    parse("SELECT e.id FROM a,LATERAL (SELECT a.id) d,LATERAL (SELECT d.id) e");
    const sqlparser_id expected=sqlparser_get_node(document,from_root())->as.join.left;
    orm_sql_query_scope scope=from_scope(0); vec_t prefixes={0}; size_t bytes=0;
    check_equal(orm_sql_from_lateral_prefixes_at(&scope,lateral_named("e"),&prefixes,&bytes,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&prefixes),1u); check_equal(*(const sqlparser_id *)vec_at_const(&prefixes,0),expected);
    check_equal(orm_sql_work_release(&prefixes,bytes,&budget,&error),TURBODB_STATUS_OK);
  }
  it("rejects lateral targets inside another query and ordinary source identities") {
    parse("SELECT q.id FROM a JOIN (SELECT d.id FROM b,LATERAL (SELECT b.id) d) q ON TRUE");
    orm_sql_query_scope scope=from_scope(0); vec_t prefixes={0}; size_t bytes=0;
    check_equal(orm_sql_from_lateral_prefixes_at(&scope,lateral_named("d"),&prefixes,&bytes,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_contains(error.message,"not in this SELECT FROM");
    const sqlparser_id invalid[]={0,UINT32_MAX,scope.root,from_root(),sqlparser_get_node(document,from_root())->as.join.left};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      check_equal(orm_sql_from_lateral_prefixes_at(&scope,invalid[i],&prefixes,&bytes,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_false(prefixes.initialized); check_equal(bytes,0u);
    }
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  }
  it("collects common lateral join prefixes and rejects excessive traversal depth") {
    const char *sql[]={"SELECT d.id FROM a NATURAL JOIN LATERAL (SELECT 1 AS id) d",
        "SELECT d.id FROM a JOIN LATERAL (SELECT 1 AS id) d USING(id)"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); orm_sql_query_scope scope=from_scope(0); vec_t prefixes={0}; size_t bytes=0;
      check_equal(orm_sql_from_lateral_prefixes_at(&scope,lateral_named("d"),&prefixes,&bytes,&error),TURBODB_STATUS_OK);
      check_equal(vec_size(&prefixes),1u); check_equal(orm_sql_work_release(&prefixes,bytes,&budget,&error),TURBODB_STATUS_OK);
    }
    parse("SELECT d.id FROM a JOIN LATERAL (SELECT a.id) d ON TRUE");
    orm_sql_query_scope scope=from_scope(0); scope.max_depth=1; vec_t prefixes={0}; size_t bytes=0;
    check_equal(orm_sql_from_lateral_prefixes_at(&scope,lateral_named("d"),&prefixes,&bytes,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_false(prefixes.initialized); check_equal(bytes,0u);
  }
  it("refunds every lateral prefix allocation and step failure") {
    parse("SELECT d.id FROM a JOIN LATERAL (SELECT a.id+c.id AS id) d ON TRUE RIGHT JOIN c ON TRUE");
    orm_sql_query_scope scope=from_scope(0); const sqlparser_id target=lateral_named("d");
    vec_t prefixes={0}; size_t bytes=0; reserve_calls=resize_calls=0;
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_from_lateral_prefixes_at(&scope,target,&prefixes,&bytes,&error),TURBODB_STATUS_OK);
    const size_t counts[]={reserve_calls,resize_calls};
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_sql_work_release(&prefixes,bytes,&budget,&error),TURBODB_STATUS_OK); bytes=0;
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      reserve_calls=resize_calls=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_sql_from_lateral_prefixes_at(&scope,target,&prefixes,&bytes,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0; check_false(prefixes.initialized); check_equal(bytes,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
    for(uint64_t allowance=0;allowance<steps;++allowance) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      check_equal(orm_sql_from_lateral_prefixes_at(&scope,target,&prefixes,&bytes,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_false(prefixes.initialized); check_equal(bytes,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }
  it("prepares only the selected FROM prefix while later lateral output remains unbound") {
    parse("SELECT d.id FROM a p LEFT JOIN b q ON p.id=q.id JOIN LATERAL (SELECT p.id) d ON TRUE");
    const sqlparser_id subtree=sqlparser_get_node(document,from_root())->as.join.left;
    orm_sql_query_scope scope=from_scope(0); vec_t names={0}; size_t bytes=0;
    check_equal(orm_sql_from_subtree_tables_at(&scope,subtree,&names,&bytes,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&names),2u);
    check_equal(strcmp(((const orm_sql_from_table *)vec_at_const(&names,0))->name,"a"),0);
    check_equal(strcmp(((const orm_sql_from_table *)vec_at_const(&names,1))->name,"b"),0);
    check_equal(orm_sql_work_release(&names,bytes,&budget,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_from_subtree_schema_at(&scope,subtree,tables,2,&plan,&error),TURBODB_STATUS_OK);
    check_equal(plan.root,subtree); check_equal(plan.tables,2u); check_false(plan.conditions_bound);
    check_false(plan.contains_lateral);
    const orm_sql_from_node *root=orm_tidesdb_sql_from_at(&plan,0);
    check_equal(root->schema.count,4u); check_false(root->schema.columns[0].type.nullable);
    check_true(root->schema.columns[2].type.nullable);
    check_equal(root->schema.columns[0].qualifier.data[0],'p'); check_equal(root->schema.columns[2].qualifier.data[0],'q');
    sqlparser_document_destroy(document); document=NULL; memset(schemas,0,sizeof(schemas)); memset(columns,0,sizeof(columns));
    check_equal(strcmp(root->schema.columns[0].name.data,"id"),0);
    check_equal(strcmp(root->schema.columns[3].name.data,"y"),0);
    check_equal(root->schema.columns[2].qualifier.data[0],'q');
  }

  it("keeps full-statement marker positions while completing only selected JOIN conditions") {
    parse("SELECT ? FROM a LEFT JOIN b ON a.id+?=b.id JOIN LATERAL (SELECT ? AS n) d ON TRUE");
    const sqlparser_id subtree=sqlparser_get_node(document,from_root())->as.join.left;
    orm_sql_query_scope scope=from_scope(3);
    check_equal(orm_sql_from_subtree_schema_at(&scope,subtree,tables,2,&plan,&error),TURBODB_STATUS_OK);
    open_inputs(); orm_sql_row_source *inputs[]={&sources[0],&sources[1]};
    const turbodb_value_t values[]={turbodb_i64(99),turbodb_i64(1),turbodb_i64(88)};
    check_equal(orm_tidesdb_sql_from_open(&plan,inputs,2,values,3,&from_run,&error),TURBODB_STATUS_INVALID_STATE);
    check_false(sources[0].active); check_false(sources[1].active); check_null(from_run.plan);
    check_equal(plan.active_runs,0u); check_equal(plan.parameter_marker_count,3u);
    check_equal(orm_tidesdb_sql_from_bind_conditions_at(&scope,&plan,&error),TURBODB_STATUS_OK);
    check_true(plan.conditions_bound);
    const orm_sql_from_node *root=orm_tidesdb_sql_from_at(&plan,0);
    bool marker=false;
    for(size_t i=0;i<vec_size(&root->slots);++i) {
      const size_t slot=*(const size_t *)vec_at_const(&root->slots,i);
      if(slot>=root->schema.count) { check_equal(slot,root->schema.count+1); marker=true; }
    }
    check_true(marker);
    check_equal(orm_tidesdb_sql_from_open(&plan,inputs,2,values,3,&from_run,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_from_bind_conditions_at(&scope,&plan,&error),TURBODB_STATUS_INVALID_ARGUMENT);
  }

  it("prepares lateral leaf metadata from its declared schema without granting execution") {
    parse("SELECT * FROM a,LATERAL (SELECT a.id) d");
    const sqlparser_id subtree=sqlparser_get_node(document,from_root())->as.join.right;
    orm_sql_query_scope scope=from_scope(0); schemas[0].name=vstr_from_cstr("d");
    check_equal(orm_sql_from_subtree_schema_at(&scope,subtree,tables,1,&plan,&error),TURBODB_STATUS_OK);
    check_true(plan.contains_lateral); check_equal(plan.tables,1u);
    const orm_sql_from_node *root=orm_tidesdb_sql_from_at(&plan,0);
    check_equal(root->schema.columns[0].qualifier.data[0],'d');
    check_equal(orm_tidesdb_sql_from_bind_conditions_at(&scope,&plan,&error),TURBODB_STATUS_OK);
    open_inputs(); orm_sql_row_source *inputs[]={&sources[0]};
    check_equal(orm_tidesdb_sql_from_open(&plan,inputs,1,NULL,0,&from_run,&error),TURBODB_STATUS_UNSUPPORTED);
    check_false(sources[0].active); check_null(from_run.plan);
  }

  it("rejects nested query bodies and non-FROM identities as selected sibling subtrees") {
    parse("SELECT (SELECT id FROM b) AS n FROM a,LATERAL (SELECT id FROM c) d");
    orm_sql_query_scope scope=from_scope(0);
    for(size_t i=1;i<=sqlparser_node_count(document);++i) {
      const sqlparser_node *node=sqlparser_get_node(document,(sqlparser_id)i);
      if(node->kind!=SQLPARSER_SELECT && (node->kind!=SQLPARSER_TABLE || node->as.table.query)) continue;
      if(node->kind==SQLPARSER_TABLE && sqlparser_text(document,node->span)[0]=='a') continue;
      vec_t names={0}; size_t bytes=0;
      check_equal(orm_sql_from_subtree_tables_at(&scope,(sqlparser_id)i,&names,&bytes,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_false(names.initialized); check_equal(bytes,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
    vec_t names={0}; size_t bytes=0;
    check_equal(orm_sql_from_subtree_tables_at(&scope,0,&names,&bytes,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_sql_from_subtree_tables_at(&scope,UINT32_MAX,&names,&bytes,&error),TURBODB_STATUS_INVALID_ARGUMENT);
  }

  it("preserves RIGHT JOIN logical order and nullability in selected prefix metadata") {
    parse("SELECT * FROM a p RIGHT JOIN b q ON p.id=q.id JOIN LATERAL (SELECT q.id) d ON TRUE");
    const sqlparser_id subtree=sqlparser_get_node(document,from_root())->as.join.left;
    orm_sql_query_scope scope=from_scope(0);
    check_equal(orm_sql_from_subtree_schema_at(&scope,subtree,tables,2,&plan,&error),TURBODB_STATUS_OK);
    const orm_sql_from_node *root=orm_tidesdb_sql_from_at(&plan,0);
    check_true(root->reversed); check_true(root->schema.columns[0].type.nullable);
    check_false(root->schema.columns[2].type.nullable);
    check_equal(root->schema.columns[0].qualifier.data[0],'p'); check_equal(root->schema.columns[2].qualifier.data[0],'q');
  }

  it("requires explicit condition completion even for an unconditional schema-only FROM") {
    parse("SELECT a.id FROM a,b"); orm_sql_query_scope scope=from_scope(0);
    check_equal(orm_tidesdb_sql_from_bind_schema_at(&scope,tables,2,&plan,&error),TURBODB_STATUS_OK);
    open_inputs(); orm_sql_row_source *inputs[]={&sources[0],&sources[1]};
    check_equal(orm_tidesdb_sql_from_open(&plan,inputs,2,NULL,0,&from_run,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(plan.active_runs,0u); check_false(sources[0].active); check_false(sources[1].active);
    check_equal(orm_tidesdb_sql_from_bind_conditions_at(&scope,&plan,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_from_open(&plan,inputs,2,NULL,0,&from_run,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_from_bind_conditions_at(&scope,&plan,&error),TURBODB_STATUS_INVALID_ARGUMENT);
  }

  it("fails selected FROM metadata depth and schema shape before publishing a plan") {
    parse("SELECT * FROM a LEFT JOIN b ON TRUE JOIN LATERAL (SELECT a.id) d ON TRUE");
    const sqlparser_id subtree=sqlparser_get_node(document,from_root())->as.join.left;
    orm_sql_query_scope scope=from_scope(0); scope.max_depth=1;
    check_equal(orm_sql_from_subtree_schema_at(&scope,subtree,tables,2,&plan,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    scope.max_depth=TEST_DEPTH;
    check_equal(orm_sql_from_subtree_schema_at(&scope,subtree,tables,1,&plan,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    columns[1][1].name=columns[1][0].name;
    check_equal(orm_sql_from_subtree_schema_at(&scope,subtree,tables,2,&plan,&error),TURBODB_STATUS_SQL_ERROR);
    check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  }

  it("discovers owned Catalog names in SQL occurrence order before schema binding") {
    parse("EXPLAIN SELECT p.id FROM a p RIGHT JOIN b q ON p.id=q.id JOIN a r ON r.id=q.id");
    vec_t requests={0}; size_t bytes=0;
    check_equal(orm_tidesdb_sql_from_tables(document,TEST_DEPTH,&budget,&requests,&bytes,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&requests),3u); sqlparser_document_destroy(document); document=NULL;
    const char *names[]={"a","b","a"};
    for(size_t i=0;i<3;++i) check_equal(strcmp(((const orm_sql_from_table *)vec_at_const(&requests,i))->name,names[i]),0);
    check_equal(orm_sql_work_release(&requests,bytes,&budget,&error),TURBODB_STATUS_OK);
  }
  it("retains CROSS plans and all leaf leases until downstream and FROM runs close") {
    parse("SELECT a.id FROM a CROSS JOIN b"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK); open_pipeline(NULL,0);
    check_equal(plan.active_runs,1u); check_true(sources[0].active); check_true(sources[1].active);
    check_equal(orm_tidesdb_sql_from_destroy(&plan,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_BUSY);
    check_equal(selected_next().state,ORM_SQL_SCAN_ROW);
    check_equal(orm_tidesdb_sql_select_close(&selected_run,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_from_close(&from_run,&error),TURBODB_STATUS_OK);
    check_equal(plan.active_runs,0u); check_false(sources[0].active); check_false(sources[1].active);
    check_equal(orm_tidesdb_sql_from_destroy(&plan,&error),TURBODB_STATUS_OK);
  }
  it("discovers derived occurrences separately from Catalog names and binds supplied output schemas") {
    parse("SELECT p.id FROM (SELECT id,x FROM a) p JOIN b ON p.id=b.id");
    vec_t requests={0}; size_t bytes=0;
    check_equal(orm_tidesdb_sql_from_tables(document,TEST_DEPTH,&budget,&requests,&bytes,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&requests),2u);
    const orm_sql_from_table *derived=vec_at_const(&requests,0), *plain=vec_at_const(&requests,1);
    check_equal(strcmp(derived->name,"p"),0); check_true(derived->derived!=0); check_equal(plain->derived,0u);
    check_true(sqlparser_get_node(document,derived->derived)->as.table.query!=0);
    check_equal(orm_sql_work_release(&requests,bytes,&budget,&error),TURBODB_STATUS_OK);
    schemas[0].name=vstr_from_cstr("p"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    const orm_sql_from_node *leaf=orm_tidesdb_sql_from_at(&plan,1);
    check_true(leaf->leaf); check_equal(leaf->schema.count,2u); check_equal(strcmp(leaf->qualifier,"p"),0);
  }
  it("rejects aliased sources changed schemas and invalid parameters before opening any lease") {
    parse("SELECT a.id FROM a JOIN b ON a.id+?=b.id"); check_equal(bind_from(2,1),TURBODB_STATUS_OK); open_inputs();
    orm_sql_row_source *inputs[]={&sources[0],&sources[0]}; const turbodb_value_t value=turbodb_i64(1);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_from_open(&plan,inputs,2,&value,1,&from_run,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    inputs[1]=&sources[1]; source_types[1][0].nullable=true;
    check_equal(orm_tidesdb_sql_from_open(&plan,inputs,2,&value,1,&from_run,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    source_types[1][0].nullable=false; const turbodb_value_t invalid=turbodb_text("wrong");
    check_equal(orm_tidesdb_sql_from_open(&plan,inputs,2,&invalid,1,&from_run,&error),TURBODB_STATUS_TYPE_ERROR);
    sources[1].active=true;
    check_equal(orm_tidesdb_sql_from_open(&plan,inputs,2,&value,1,&from_run,&error),TURBODB_STATUS_BUSY); sources[1].active=false;
    check_null(from_run.plan); check_equal(plan.active_runs,0u); check_false(sources[0].active); check_false(sources[1].active);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("binds qualified repeated names and evaluates ON over logical left and right columns") {
    parse("SELECT a.id FROM a INNER JOIN b ON a.id=b.id AND x<y"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    check_equal(plan.count,3u); check_equal(plan.tables,2u);
    const orm_sql_from_node *root=orm_tidesdb_sql_from_at(&plan,0);
    check_equal(root->kind,ORM_SQL_JOIN_INNER); check_false(root->reversed); check_equal(root->schema.count,4u);
    check_equal(orm_tidesdb_sql_from_at(&plan,root->left)->table,0u);
    check_equal(orm_tidesdb_sql_from_at(&plan,root->right)->table,1u);
    const turbodb_value_t values[]={turbodb_i64(2),turbodb_i64(4),turbodb_i64(2),turbodb_i64(5)};
    check_equal(evaluate(0,values,4,NULL,0).data.boolean_value,1);
    check_equal(root->schema.columns[0].qualifier.data[0],'a'); check_equal(root->schema.columns[2].qualifier.data[0],'b');
    check_null(orm_tidesdb_sql_from_at(&plan,plan.count));
  }
  it("owns schema names aliases and ON code after document and input metadata changes") {
    char name[]="id"; columns[0][0].name=vstr_from_cstr(name);
    parse("SELECT p.id FROM a AS p LEFT JOIN b AS q ON p.id=q.id"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL; name[0]='!'; columns[0][0].type.kind=TURBODB_VALUE_TEXT;
    const orm_sql_from_node *root=orm_tidesdb_sql_from_at(&plan,0);
    check_equal(memcmp(root->schema.columns[0].name.data,"id",2),0);
    check_equal(root->schema.columns[0].qualifier.data[0],'p'); check_equal(root->schema.columns[2].qualifier.data[0],'q');
    check_false(root->schema.columns[0].type.nullable); check_true(root->schema.columns[2].type.nullable);
    const turbodb_value_t values[]={turbodb_i64(2),turbodb_i64(0),turbodb_i64(2),turbodb_i64(0)};
    check_equal(evaluate(0,values,4,NULL,0).data.boolean_value,1);
  }
  it("preserves earlier outer nullability when a later ON sees the joined subtree") {
    parse("SELECT a.id FROM a LEFT JOIN b ON a.id=b.id INNER JOIN c ON b.id=c.id"); check_equal(bind_from(3,0),TURBODB_STATUS_OK);
    const orm_sql_from_node *root=orm_tidesdb_sql_from_at(&plan,0);
    check_equal(plan.count,5u); check_true(root->schema.columns[2].type.nullable); check_false(root->schema.columns[4].type.nullable);
    const turbodb_value_t values[]={turbodb_i64(2),turbodb_i64(0),turbodb_null(),turbodb_null(),turbodb_i64(2),turbodb_i64(0)};
    check_equal(evaluate(0,values,6,NULL,0).kind,TURBODB_VALUE_NULL);
  }
  it("uses source ordered parameter positions across SELECT ON WHERE and LIMIT") {
    parse("SELECT ? AS n FROM a JOIN b ON a.id+?=b.id WHERE a.x>? LIMIT ?"); check_equal(bind_from(2,4),TURBODB_STATUS_OK);
    const turbodb_value_t values[]={turbodb_i64(5),turbodb_i64(0),turbodb_i64(7),turbodb_i64(0)};
    const turbodb_value_t inputs[]={turbodb_i64(999),turbodb_i64(2),turbodb_i64(333),turbodb_i64(1)};
    check_equal(evaluate(0,values,4,inputs,4).data.boolean_value,1);
    check_equal(vec_size(&plan.parameter_types),4u);
  }
  it("rewrites RIGHT to LEFT with swapped ON slots and unchanged logical output order") {
    parse("SELECT ? AS n FROM a RIGHT JOIN b ON a.id+?=b.id WHERE a.x>? LIMIT ?"); check_equal(bind_from(2,4),TURBODB_STATUS_OK);
    const orm_sql_from_node *root=orm_tidesdb_sql_from_at(&plan,0);
    check_equal(root->kind,ORM_SQL_JOIN_LEFT); check_true(root->reversed);
    check_true(root->schema.columns[0].type.nullable); check_false(root->schema.columns[2].type.nullable);
    check_equal(root->schema.columns[0].qualifier.data[0],'a'); check_equal(root->schema.columns[2].qualifier.data[0],'b');
    const turbodb_value_t physical[]={turbodb_i64(7),turbodb_i64(0),turbodb_i64(5),turbodb_i64(0)};
    const turbodb_value_t inputs[]={turbodb_i64(999),turbodb_i64(2),turbodb_i64(333),turbodb_i64(1)};
    check_equal(evaluate(0,physical,4,inputs,4).data.boolean_value,1);
  }
  it("accepts self joins only with distinct effective aliases") {
    tables[1]=&schemas[0]; parse("SELECT p.id FROM a p JOIN a q ON p.id=q.id"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    check_equal(plan.tables,2u); const orm_sql_from_node *root=orm_tidesdb_sql_from_at(&plan,0);
    check_equal(root->schema.columns[0].qualifier.data[0],'p'); check_equal(root->schema.columns[2].qualifier.data[0],'q');
    reject("SELECT a.id FROM a JOIN a ON a.id=a.id",2,TURBODB_STATUS_SQL_ERROR,"duplicate table");
  }
  it("treats INNER CROSS and comma without ON as unconditional combinations") {
    const char *sql[]={"SELECT a.id FROM a JOIN b","SELECT a.id FROM a CROSS JOIN b","SELECT a.id FROM a,b"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset(); parse(sql[i]); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
      const orm_sql_from_node *root=orm_tidesdb_sql_from_at(&plan,0);
      check_equal(root->kind,ORM_SQL_JOIN_CROSS); check_null(root->condition.budget);
    }
    reset(); parse("SELECT a.id FROM a CROSS JOIN b ON a.id=b.id"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_from_at(&plan,0)->kind,ORM_SQL_JOIN_INNER);
  }
  it("rejects ambiguous names hidden original names and ON references outside its subtree") {
    reject("SELECT a.id FROM a JOIN b ON id=b.id",2,TURBODB_STATUS_SQL_ERROR,"ambiguous");
    reject("SELECT p.id FROM a p JOIN b ON a.id=b.id",2,TURBODB_STATUS_SQL_ERROR,"qualifier");
    reject("SELECT a.id FROM a JOIN b ON a.missing=b.id",2,TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT a.id FROM a JOIN b ON a.id=c.id JOIN c ON b.id=c.id",3,TURBODB_STATUS_SQL_ERROR,"qualifier");
    reject("SELECT a.id AS output FROM a JOIN b ON output=b.id",2,TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT a.id FROM a p JOIN b p ON p.id=p.id",2,TURBODB_STATUS_SQL_ERROR,"duplicate table");
  }
  it("rejects unsupported join field semantics rather than ignoring modifiers") {
    reject("SELECT d.id FROM a JOIN LATERAL (SELECT a.id) d ON TRUE",2,TURBODB_STATUS_UNSUPPORTED,"LATERAL");
    reject("SELECT a.id FROM a JOIN b USING(missing)",2,TURBODB_STATUS_SQL_ERROR,"both operands");
    reject("SELECT a.id FROM a JOIN b USING(id,id)",2,TURBODB_STATUS_SQL_ERROR,"repeated");
    reject("SELECT a.id FROM (SELECT id FROM a) p JOIN b ON p.id=b.id",2,TURBODB_STATUS_SQL_ERROR,"differs from supplied schema");
    reject("SELECT a.id FROM a JOIN b ON SUM(a.id)=b.id",2,TURBODB_STATUS_UNSUPPORTED,"function");
    reject("SELECT a.id FROM a JOIN b ON a.id",2,TURBODB_STATUS_UNSUPPORTED,"BOOL");
  }
  it("checks each supplied schema and all parameters even when unrelated to ON") {
    parse("SELECT a.id FROM a JOIN b ON TRUE"); check_equal(bind_from(1,0),TURBODB_STATUS_INVALID_ARGUMENT);
    reset(); tables[1]=&schemas[2]; check_equal(bind_from(2,0),TURBODB_STATUS_SQL_ERROR); tables[1]=&schemas[1];
    reset(); columns[1][1].name=columns[1][0].name; check_equal(bind_from(2,0),TURBODB_STATUS_SQL_ERROR);
    columns[1][1].name=vstr_from_cstr("y");
    reset(); parse("SELECT ? AS n FROM a JOIN b ON TRUE"); parameters[0]=(orm_sql_type){TURBODB_VALUE_NULL,false};
    check_equal(bind_from(2,1),TURBODB_STATUS_TYPE_ERROR);
    reset(); parameters[0]=(orm_sql_type){TURBODB_VALUE_INT64,false}; check_equal(bind_from(2,0),TURBODB_STATUS_SQL_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  }
  it("binds a single plain table through the same owned scope") {
    parse("SELECT p.id FROM a p"); check_equal(bind_from(1,0),TURBODB_STATUS_OK);
    check_equal(plan.count,1u); check_true(orm_tidesdb_sql_from_at(&plan,0)->leaf);
    check_equal(orm_tidesdb_sql_from_at(&plan,0)->schema.columns[0].qualifier.data[0],'p');
  }
  it("leaves a plan intact while any ON evaluator is active") {
    parse("SELECT a.id FROM a JOIN b ON a.id=b.id"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    orm_sql_expr_run evaluator={0}; orm_sql_from_node *root=vec_at(&plan.nodes,0);
    check_equal(orm_tidesdb_sql_expr_run_open(&root->condition,&evaluator,&error),TURBODB_STATUS_OK);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_from_destroy(&plan,&error),TURBODB_STATUS_BUSY);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained); check_equal(plan.count,3u);
    check_equal(orm_tidesdb_sql_expr_run_close(&evaluator,&error),TURBODB_STATUS_OK);
  }
  it("enforces depth plan and workspace bounds and restores work on failure") {
    parse("SELECT a.id FROM a JOIN b ON a.id=b.id JOIN c ON b.id=c.id");
    check_equal(orm_tidesdb_sql_from_bind(document,tables,3,NULL,0,1,&budget,&plan,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    reset(); budget.limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES]=2;
    check_equal(bind_from(3,0),TURBODB_STATUS_LIMIT_EXCEEDED);
    reset(); budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=1;
    check_equal(bind_from(3,0),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(plan.budget);
  }
  it("refunds every fixed allocation across topology names schema parameters and ON binding") {
    parse("SELECT ? AS n FROM a LEFT JOIN b ON a.id+?=b.id RIGHT JOIN c ON b.id=c.id");
    reserve_calls=resize_calls=0; check_equal(bind_from(3,2),TURBODB_STATUS_OK);
    const size_t reserves=reserve_calls,resizes=resize_calls;
    for(size_t i=1;i<=reserves+resizes;++i) {
      reset(); reserve_calls=resize_calls=0; if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      check_equal(bind_from(3,2),TURBODB_STATUS_OUT_OF_MEMORY); check_null(plan.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }
  it("enforces every binding step budget without publishing an incomplete plan") {
    parse("SELECT a.id FROM a LEFT JOIN b ON a.id=b.id"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    for(uint64_t allowance=1;allowance<cost;++allowance) {
      reset(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=allowance;
      check_equal(bind_from(2,0),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(plan.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }
  it("binds complete SELECT over LEFT JOIN and runs filtering projection ordering and parameters") {
    parse("SELECT a.id+? AS aid,b.y AS value FROM a LEFT JOIN b ON a.id+?=b.id WHERE a.id>? ORDER BY aid LIMIT ?");
    check_equal(bind_from(2,4),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK);
    check_true(orm_tidesdb_sql_select_column_at(&selected,1)->type.nullable);
    sqlparser_document_destroy(document); document=NULL;
    const turbodb_value_t values[]={turbodb_i64(10),turbodb_i64(0),turbodb_i64(0),turbodb_i64(2)}; open_pipeline(values,4);
    orm_sql_scan_row row=selected_next(); check_equal(row.values[0].data.int64_value,11); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    row=selected_next(); check_equal(row.values[0].data.int64_value,12); check_equal(row.values[1].data.int64_value,200);
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
  }
  it("restores logical columns after RIGHT rewrite and preserves qualified projection metadata") {
    parse("SELECT a.id AS aid,b.id AS bid FROM a RIGHT JOIN b ON a.id=b.id ORDER BY bid");
    check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK); open_pipeline(NULL,0);
    check_true(orm_tidesdb_sql_select_column_at(&selected,0)->type.nullable);
    check_false(orm_tidesdb_sql_select_column_at(&selected,1)->type.nullable);
    orm_sql_scan_row row=selected_next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,2);
    row=selected_next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].data.int64_value,3);
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
  }
  it("groups join outputs by qualified source columns with HAVING aliases and hidden order keys") {
    parse("SELECT a.id AS aid,COUNT(b.id) AS n FROM a LEFT JOIN b ON a.id=b.id GROUP BY a.id HAVING n>=0 ORDER BY MAX(a.x)");
    check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK); open_pipeline(NULL,0);
    orm_sql_scan_row row=selected_next(); check_equal(row.values[0].data.int64_value,1); check_equal(row.values[1].data.int64_value,0);
    row=selected_next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,1);
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
  }
  it("distinguishes same-named group keys across tables and preserves repeated identical keys") {
    const char *sql[]={
      "SELECT a.id AS aid,b.id AS bid,COUNT(*) AS n FROM a LEFT JOIN b ON a.id=b.id GROUP BY a.id,b.id ORDER BY aid",
      "SELECT a.id AS aid,b.id AS bid,COUNT(*) AS n FROM a LEFT JOIN b ON a.id=b.id GROUP BY a.id,a.id,b.id ORDER BY aid"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset(); parse(sql[i]); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK); open_pipeline(NULL,0);
      orm_sql_scan_row row=selected_next(); check_equal(row.values[0].data.int64_value,1); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      row=selected_next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,2);
      check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("expands only the requested table star and copies SELECT metadata independently of FROM") {
    parse("SELECT a.*,b.id AS bid FROM a JOIN b ON a.id=b.id ORDER BY bid"); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&selected.columns),3u); check_equal(*(const size_t *)vec_at_const(&selected.projection,2),2u);
    check_equal(orm_tidesdb_sql_from_destroy(&plan,&error),TURBODB_STATUS_OK); sqlparser_document_destroy(document); document=NULL;
    const turbodb_value_t rows[]={turbodb_i64(2),turbodb_i64(20),turbodb_i64(2),turbodb_i64(200)};
    check_equal(orm_tidesdb_sql_select_open(&selected,rows,1,&selected_run,&error),TURBODB_STATUS_OK);
    const orm_sql_scan_row row=selected_next(); check_equal(row.count,3u); check_equal(row.values[1].data.int64_value,20);
  }
  it("rejects ambiguous SELECT GROUP and WHERE columns while retaining explicit output-name rules") {
    const char *sql[]={"SELECT id FROM a JOIN b ON TRUE",
      "SELECT a.id AS aid FROM a JOIN b ON TRUE WHERE id>0",
      "SELECT COUNT(*) AS n FROM a JOIN b ON TRUE GROUP BY id",
      "SELECT * FROM a JOIN b ON TRUE"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset(); parse(sql[i]); check_equal(bind_from(2,0),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_SQL_ERROR);
      check_contains(error.message,i==3?"duplicate output":"ambiguous"); check_null(selected.budget);
    }
  }
  it("refunds each SELECT allocation over an already bound joined scope") {
    parse("SELECT a.id AS aid,COUNT(b.id) AS n FROM a LEFT JOIN b ON a.id=b.id GROUP BY a.id HAVING n>0 ORDER BY aid");
    check_equal(bind_from(2,0),TURBODB_STATUS_OK); const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls=resize_calls=0;
    check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OK);
    const size_t reserves=reserve_calls,resizes=resize_calls;
    check_equal(orm_tidesdb_sql_select_destroy(&selected,&error),TURBODB_STATUS_OK);
    for(size_t i=1;i<=reserves+resizes;++i) {
      reserve_calls=resize_calls=0; if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      check_equal(orm_tidesdb_sql_select_bind_from(document,&plan,TEST_DEPTH,&selected,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(selected.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      fail_reserve=fail_resize=0;
    }
  }
}
