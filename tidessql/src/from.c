#include "from.h"
#include "name.h"
#include "work.h"
#include "error.h"
#include <string.h>
#include <stdio.h>

typedef struct from_visit { sqlparser_id ast; size_t parent, depth; bool right; } from_visit;
typedef struct from_name { char text[ORM_SQL_SELECT_NAME_BYTES+1]; } from_name;
enum { LATERAL_ENTER, LATERAL_FIRST, LATERAL_SECOND };
typedef struct from_lateral_visit { sqlparser_id ast; unsigned phase; } from_lateral_visit;
static turbodb_status_t from_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message,sizeof(message),"TidesDB SQL FROM: %s",reason);
  tdsql_error_set(error,status,message); return status;
}
static turbodb_status_t from_charge(orm_sql_from *plan, orm_sql_budget_resource resource, size_t count, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(plan->budget,&amount,error);
}
static bool from_equal(vstr left, vstr right) {
  return left.len == right.len && (!left.len || !memcmp(left.data,right.data,left.len));
}
static turbodb_status_t from_copy_name(vstr text, char *out, turbodb_error_t *error) {
  const char *reason = NULL;
  const turbodb_status_t status = orm_sql_name_validate(text,&reason);
  if (status != TURBODB_STATUS_OK) return from_error(error,status,reason);
  memcpy(out,text.data,text.len); out[text.len] = 0; return TURBODB_STATUS_OK;
}
static turbodb_status_t from_ast_name(const sqlparser_document *document, sqlparser_id id, char *out, turbodb_error_t *error) {
  const sqlparser_node *node = sqlparser_get_node(document,id);
  if (!node || node->kind != SQLPARSER_NAME) return from_error(error,TURBODB_STATUS_UNSUPPORTED,"expected plain table name or alias");
  vstr text = {sqlparser_text(document,node->span),node->span.length}, name; const char *reason = NULL;
  const turbodb_status_t status = orm_sql_name_part(&text,&name,&reason);
  if (status != TURBODB_STATUS_OK) return from_error(error,status,reason);
  if (text.len) return from_error(error,TURBODB_STATUS_UNSUPPORTED,"qualified table names are not supported");
  return from_copy_name(name,out,error);
}
const orm_sql_from_node *orm_tidesdb_sql_from_at(const orm_sql_from *plan, size_t ordinal) {
  return plan && plan->budget && ordinal < plan->count ? vec_at_const(&plan->nodes,ordinal) : NULL;
}
turbodb_status_t orm_tidesdb_sql_from_destroy(orm_sql_from *plan, turbodb_error_t *error) {
  if (!plan || !plan->budget) return TURBODB_STATUS_OK;
  if (plan->active_runs) return from_error(error,TURBODB_STATUS_BUSY,"FROM plan has active execution runs");
  for (size_t i = 0; i < plan->count; ++i)
    if (((const orm_sql_from_node *)vec_at_const(&plan->nodes,i))->condition.active_runs)
      return from_error(error,TURBODB_STATUS_BUSY,"FROM plan has active ON evaluators");
  turbodb_status_t status = TURBODB_STATUS_OK;
  for (size_t i = plan->count; i; --i) {
    orm_sql_from_node *node = vec_at(&plan->nodes,i-1);
    turbodb_status_t released = orm_tidesdb_sql_expr_destroy(&node->condition,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    vec_t *vectors[] = {&node->columns,&node->names,&node->slots,&node->query_slots,&node->keys};
    const size_t bytes[] = {node->column_bytes,node->name_bytes,node->slot_bytes,node->query_slot_bytes,node->key_bytes};
    for (size_t j = 0; j < sizeof(vectors)/sizeof(vectors[0]); ++j) {
      released = orm_sql_work_release(vectors[j],bytes[j],plan->budget,status == TURBODB_STATUS_OK ? error : NULL);
      if (status == TURBODB_STATUS_OK) status = released;
    }
  }
  vec_t *vectors[] = {&plan->nodes,&plan->parameter_types}; const size_t bytes[] = {plan->node_bytes,plan->parameter_bytes};
  for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i],bytes[i],plan->budget,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (plan->metadata_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(plan->budget,ORM_SQL_BUDGET_WORK_BYTES,
        plan->metadata_bytes,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *plan = (orm_sql_from){0}; return status;
}
static turbodb_status_t from_table(const sqlparser_document *document, const sqlparser_node *ast,
    orm_sql_from_node *node, orm_sql_from *plan, bool lateral_metadata, turbodb_error_t *error) {
  if (ast->as.table.lateral && !lateral_metadata)
    return from_error(error,TURBODB_STATUS_UNSUPPORTED,"LATERAL execution is not supported");
  if(ast->as.table.lateral) {
    if(!ast->as.table.query || !ast->as.table.alias)
      return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"LATERAL metadata requires a derived query and alias");
    plan->contains_lateral=true;
    node->lateral=true;
  }
  if (ast->as.table.arguments.count || ast->as.table.indexed_by ||
      ast->as.table.group || ast->as.table.table_function || ast->as.table.not_indexed)
    return from_error(error,TURBODB_STATUS_UNSUPPORTED,"FROM requires plain table references");
  if (ast->as.table.query && !ast->as.table.alias)
    return from_error(error,TURBODB_STATUS_SQL_ERROR,"derived table requires an alias");
  turbodb_status_t status = from_ast_name(document,ast->as.table.query ? ast->as.table.alias : ast->as.table.name,node->name,error);
  if (status == TURBODB_STATUS_OK) status = from_ast_name(document,ast->as.table.alias ? ast->as.table.alias :
      ast->as.table.name,node->qualifier,error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i+1 < plan->count; ++i) {
    const orm_sql_from_node *previous = vec_at_const(&plan->nodes,i);
    status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (previous->leaf && !strcmp(previous->qualifier,node->qualifier))
      return from_error(error,TURBODB_STATUS_SQL_ERROR,"duplicate table qualifier");
  }
  node->leaf = true; node->table = plan->tables++; return TURBODB_STATUS_OK;
}
static turbodb_status_t from_tree(const sqlparser_document *document, size_t max_depth,
    orm_sql_from *plan, bool lateral_metadata, turbodb_error_t *error) {
  const size_t capacity = sqlparser_node_count(document);
  vec_t stack = {0}; size_t bytes = 0;
  turbodb_status_t status = orm_sql_work_zero(&stack,capacity,sizeof(from_visit),_Alignof(from_visit),plan->budget,&bytes,error);
  size_t pending = 0;
  if (status == TURBODB_STATUS_OK) *(from_visit *)vec_at(&stack,pending++) = (from_visit){plan->root,SIZE_MAX,1,false};
  while (status == TURBODB_STATUS_OK && pending) {
    const from_visit visit = *(const from_visit *)vec_at_const(&stack,--pending);
    const sqlparser_node *ast = sqlparser_get_node(document,visit.ast);
    if (!ast) { status = from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"missing FROM node"); break; }
    if (visit.depth > max_depth || plan->count == capacity) {
      status = from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"FROM tree depth or capacity exceeded"); break;
    }
    orm_sql_budget_amount visit_charge = {0};
    visit_charge.value[ORM_SQL_BUDGET_PLAN_NODES] = 1;
    visit_charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
    status = orm_tidesdb_sql_budget_reserve(plan->budget,&visit_charge,error);
    if (status != TURBODB_STATUS_OK) break;
    const size_t index = plan->count++; orm_sql_from_node *node = vec_at(&plan->nodes,index);
    node->ast = visit.ast;
    if (visit.parent != SIZE_MAX) {
      orm_sql_from_node *parent = vec_at(&plan->nodes,visit.parent);
      if (visit.right) parent->right = index; else parent->left = index;
    }
    if (ast->kind == SQLPARSER_TABLE) status = from_table(document,ast,node,plan,lateral_metadata,error);
    else if (ast->kind == SQLPARSER_JOIN) {
      if (ast->as.join.kind == SQLPARSER_JOIN_FULL) {
        status = from_error(error,TURBODB_STATUS_UNSUPPORTED,"FULL joins are not supported"); break;
      }
      if (ast->as.join.kind < SQLPARSER_JOIN_INNER || ast->as.join.kind > SQLPARSER_JOIN_NATURAL) {
        status = from_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported JOIN kind"); break;
      }
      node->reversed = ast->as.join.kind == SQLPARSER_JOIN_RIGHT;
      node->common = ast->as.join.natural || ast->as.join.using_columns.count || ast->as.join.kind == SQLPARSER_JOIN_NATURAL;
      const bool outer = node->reversed || ast->as.join.kind == SQLPARSER_JOIN_LEFT;
      if (outer && !ast->as.join.condition && !node->common) { status = from_error(error,TURBODB_STATUS_SQL_ERROR,"outer JOIN requires ON or USING"); break; }
      node->kind = outer ? ORM_SQL_JOIN_LEFT : ast->as.join.condition || node->common ? ORM_SQL_JOIN_INNER : ORM_SQL_JOIN_CROSS;
      enum { JOIN_CHILDREN = 2 };
      if (capacity < JOIN_CHILDREN || pending > capacity-JOIN_CHILDREN || visit.depth == SIZE_MAX) {
        status = from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"FROM traversal capacity exceeded"); break;
      }
      *(from_visit *)vec_at(&stack,pending++) = (from_visit){ast->as.join.right,index,visit.depth+1,true};
      *(from_visit *)vec_at(&stack,pending++) = (from_visit){ast->as.join.left,index,visit.depth+1,false};
    } else status = from_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported FROM node");
  }
  const turbodb_status_t released = orm_sql_work_release(&stack,bytes,plan->budget,status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? released : status;
}
static turbodb_status_t from_columns(orm_sql_from *plan, orm_sql_from_node *node, size_t count, turbodb_error_t *error) {
  if (!count || count > SIZE_MAX-vec_size(&plan->parameter_types) ||
      count+vec_size(&plan->parameter_types) > plan->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"FROM column shape exceeds plan capacity");
  turbodb_status_t status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,count,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&node->columns,count,sizeof(orm_sql_schema_column),
      _Alignof(orm_sql_schema_column),plan->budget,&node->column_bytes,error);
  if (status == TURBODB_STATUS_OK) node->schema = (orm_sql_table_schema){vstr_from_cstr(node->name),vec_data_const(&node->columns),count};
  return status;
}
static turbodb_status_t from_leaf(orm_sql_from *plan, orm_sql_from_node *node,
    const sqlparser_document *document,const orm_sql_table_schema *schema, turbodb_error_t *error) {
  if (!schema || !schema->columns || !schema->count)
    return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM requires a declared table schema");
  const char *reason = NULL;
  turbodb_status_t status = orm_sql_name_validate(schema->name,&reason);
  if (status != TURBODB_STATUS_OK) return from_error(error,status,reason);
  if (!from_equal(schema->name,vstr_from_cstr(node->name)))
    return from_error(error,TURBODB_STATUS_SQL_ERROR,"FROM table differs from supplied schema");
  const sqlparser_node *ast=sqlparser_get_node(document,node->ast);
  const sqlparser_list aliases=ast->as.table.column_aliases;
  if(aliases.count && aliases.count!=schema->count)
    return from_error(error,TURBODB_STATUS_SQL_ERROR,"derived column list width differs from query");
  sqlparser_id alias=aliases.first;
  status = from_columns(plan,node,schema->count,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&node->names,schema->count,sizeof(from_name),
      _Alignof(from_name),plan->budget,&node->name_bytes,error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < schema->count; ++i) {
    const orm_sql_schema_column *input = &schema->columns[i];
    from_name *name = vec_at(&node->names,i); orm_sql_predicate validator;
    status = alias?from_ast_name(document,alias,name->text,error):from_copy_name(input->name,name->text,error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,input->type,NULL,&validator,error);
    for (size_t j = 0; status == TURBODB_STATUS_OK && j < i; ++j) {
      status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
      if (status == TURBODB_STATUS_OK && from_equal(vstr_from_cstr(name->text),
          ((const orm_sql_schema_column *)vec_at_const(&node->columns,j))->name))
        status = from_error(error,TURBODB_STATUS_SQL_ERROR,"duplicate table schema column");
    }
    if (status == TURBODB_STATUS_OK) *(orm_sql_schema_column *)vec_at(&node->columns,i) =
        (orm_sql_schema_column){.name=vstr_from_cstr(name->text),.type=input->type,.qualifier=vstr_from_cstr(node->qualifier),.star_order=i+1};
    if(alias) alias=sqlparser_get_node(document,alias)->next;
  }
  return status;
}
static size_t from_visible(const orm_sql_table_schema *schema) {
  size_t count = 0;
  for (size_t i = 0; i < schema->count; ++i) count += !schema->columns[i].qualified_only;
  return count;
}
static turbodb_status_t from_common_column(orm_sql_from *plan, const orm_sql_table_schema *schema,
    vstr name, size_t *out, turbodb_error_t *error) {
  *out = SIZE_MAX;
  for (size_t i = 0; i < schema->count; ++i) {
    const turbodb_status_t status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (schema->columns[i].qualified_only || !from_equal(name,schema->columns[i].name)) continue;
    if (*out != SIZE_MAX) return from_error(error,TURBODB_STATUS_SQL_ERROR,"ambiguous USING or NATURAL column");
    *out = i;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t from_common_key(orm_sql_from *plan, orm_sql_from_node *node,
    const orm_sql_table_schema *left, const orm_sql_table_schema *right, size_t l, size_t r, turbodb_error_t *error) {
  if (left->columns[l].type.kind != right->columns[r].type.kind)
    return from_error(error,TURBODB_STATUS_TYPE_ERROR,"USING and NATURAL columns require the same value kind");
  for (size_t i = 0; i < node->key_count; ++i) {
    const turbodb_status_t status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
    if (status != TURBODB_STATUS_OK) return status;
    const orm_sql_join_key *previous = vec_at_const(&node->keys,i);
    if ((node->reversed ? previous->right : previous->left) == l)
      return from_error(error,TURBODB_STATUS_SQL_ERROR,"repeated USING column");
  }
  orm_sql_predicate comparison;
  const turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL,left->columns[l].type,&right->columns[r].type,&comparison,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (node->key_count == vec_size(&node->keys))
    return from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"common JOIN key capacity exceeded");
  *(orm_sql_join_key *)vec_at(&node->keys,node->key_count++) = node->reversed ? (orm_sql_join_key){r,l} : (orm_sql_join_key){l,r};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t from_common_keys(const sqlparser_document *document, orm_sql_from *plan,
    orm_sql_from_node *node, const orm_sql_table_schema *left, const orm_sql_table_schema *right, turbodb_error_t *error) {
  const sqlparser_node *ast = sqlparser_get_node(document,node->ast);
  const sqlparser_list list = ast->as.join.using_columns;
  const size_t capacity = list.count ? list.count : left->count < right->count ? left->count : right->count;
  if (capacity > plan->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"common JOIN key shape exceeds plan capacity");
  turbodb_status_t status = orm_sql_work_zero(&node->keys,capacity,sizeof(orm_sql_join_key),_Alignof(orm_sql_join_key),plan->budget,&node->key_bytes,error);
  sqlparser_id id = list.first;
  const size_t count = list.count ? list.count : left->count;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
    status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
    if (status != TURBODB_STATUS_OK) break;
    char name[ORM_SQL_SELECT_NAME_BYTES+1] = {0}; vstr text = {0}; size_t l = SIZE_MAX, r = SIZE_MAX;
    if (list.count) {
      status = from_ast_name(document,id,name,error); text = vstr_from_cstr(name);
      if (status == TURBODB_STATUS_OK) id = sqlparser_get_node(document,id)->next;
    } else {
      if (left->columns[i].qualified_only) continue;
      text = left->columns[i].name;
    }
    if (status == TURBODB_STATUS_OK) status = from_common_column(plan,right,text,&r,error);
    if (status != TURBODB_STATUS_OK) break;
    if (r == SIZE_MAX && !list.count) continue;
    status = from_common_column(plan,left,text,&l,error);
    if (status == TURBODB_STATUS_OK && (l == SIZE_MAX || r == SIZE_MAX))
      status = from_error(error,TURBODB_STATUS_SQL_ERROR,"USING column must exist in both operands");
    if (status == TURBODB_STATUS_OK) status = from_common_key(plan,node,left,right,l,r,error);
  }
  if (status == TURBODB_STATUS_OK && id) status = from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid USING column list");
  return status;
}
static turbodb_status_t from_common_order(orm_sql_from *plan, orm_sql_from_node *node,
    const orm_sql_table_schema *left, const orm_sql_table_schema *right, turbodb_error_t *error) {
  orm_sql_schema_column *columns = vec_data(&node->columns);
  const orm_sql_table_schema *first = node->reversed ? right : left, *second = node->reversed ? left : right;
  const size_t first_offset = node->reversed ? left->count : 0, second_offset = node->reversed ? 0 : left->count;
  const size_t counts[] = {from_visible(first),from_visible(second)};
  const size_t visible = counts[0] + counts[1];
  vec_t order = {0}; size_t bytes = 0;
  turbodb_status_t status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,left->count+right->count,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&order,visible,sizeof(size_t),_Alignof(size_t),plan->budget,&bytes,error);
  const orm_sql_table_schema *sides[] = {first,second}; const size_t offsets[] = {first_offset,second_offset};
  for (size_t side = 0; status == TURBODB_STATUS_OK && side < sizeof(sides)/sizeof(sides[0]); ++side)
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < sides[side]->count; ++i) {
      status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
      const orm_sql_schema_column *column = &sides[side]->columns[i];
      if (status != TURBODB_STATUS_OK || column->qualified_only) continue;
      if (!column->star_order || column->star_order > counts[side]) {
        status = from_error(error,TURBODB_STATUS_INVALID_STATE,"invalid FROM visible column order"); break;
      }
      *(size_t *)vec_at(&order,(side ? counts[0] : 0)+column->star_order-1) = offsets[side]+i;
    }
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < node->key_count; ++i) {
    status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
    if (status != TURBODB_STATUS_OK) break;
    const orm_sql_join_key *key = vec_at_const(&node->keys,i);
    columns[node->reversed ? key->right : left->count+key->right].qualified_only = true;
  }
  size_t rank = 0;
  for (unsigned common = 1; status == TURBODB_STATUS_OK; --common) {
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < visible; ++i) {
      status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
      if (status != TURBODB_STATUS_OK) break;
      const size_t slot = *(const size_t *)vec_at_const(&order,i); bool chosen = false;
      if (columns[slot].qualified_only) continue;
      for (size_t k = 0; status == TURBODB_STATUS_OK && k < node->key_count; ++k) {
        status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
        const orm_sql_join_key *key = vec_at_const(&node->keys,k);
        chosen |= slot == (node->reversed ? left->count+key->left : key->left);
      }
      if (status == TURBODB_STATUS_OK && chosen == (common != 0)) columns[slot].star_order = ++rank;
    }
    if (!common) break;
  }
  const turbodb_status_t released = orm_sql_work_release(&order,bytes,plan->budget,status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? released : status;
}
static turbodb_status_t from_join_schema(const sqlparser_document *document, orm_sql_from *plan, orm_sql_from_node *node,
    turbodb_error_t *error) {
  const orm_sql_from_node *left = vec_at_const(&plan->nodes,node->left), *right = vec_at_const(&plan->nodes,node->right);
  if (left->schema.count > SIZE_MAX-right->schema.count)
    return from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"JOIN column count overflow");
  turbodb_status_t status = from_columns(plan,node,left->schema.count+right->schema.count,error);
  if (status == TURBODB_STATUS_OK) status = from_charge(plan,ORM_SQL_BUDGET_EXECUTION_STEPS,left->schema.count+right->schema.count,error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_schema_column *columns = vec_data(&node->columns);
  memcpy(columns,left->schema.columns,left->schema.count*sizeof(*columns));
  memcpy(columns+left->schema.count,right->schema.columns,right->schema.count*sizeof(*columns));
  const size_t left_visible = from_visible(&left->schema);
  for (size_t i = left->schema.count; i < node->schema.count; ++i)
    if (!columns[i].qualified_only) columns[i].star_order += left_visible;
  if (node->kind == ORM_SQL_JOIN_LEFT) {
    const size_t begin = node->reversed ? 0 : left->schema.count;
    const size_t end = node->reversed ? left->schema.count : node->schema.count;
    for (size_t i = begin; i < end; ++i) columns[i].type.nullable = true;
  }
  if (node->common) {
    status = from_common_keys(document,plan,node,&left->schema,&right->schema,error);
    if (status == TURBODB_STATUS_OK) status = from_common_order(plan,node,&left->schema,&right->schema,error);
  }
  return status;
}
static turbodb_status_t from_join_condition(const sqlparser_document *document, orm_sql_from *plan,
    orm_sql_from_node *node, const vec_t *offsets, size_t max_depth,
    const orm_sql_query_scope *dependencies, turbodb_error_t *error) {
  const orm_sql_from_node *left = vec_at_const(&plan->nodes,node->left);
  const orm_sql_from_node *right = vec_at_const(&plan->nodes,node->right);
  const sqlparser_node *ast = sqlparser_get_node(document,node->ast);
  if (node->common) return TURBODB_STATUS_OK;
  const orm_sql_binding_scope scope = {.document=document,.schema=&node->schema,.parameter_types=vec_data_const(&plan->parameter_types),
    .parameter_offsets=vec_data_const(offsets),.parameter_count=vec_size(&plan->parameter_types),.budget=plan->budget,
    .parameter_marker_count=plan->parameter_marker_count,
    .outer_schema=dependencies ? dependencies->outer_schema : NULL,
    .outer_qualifier=dependencies ? dependencies->outer_qualifier : (vstr){0},
    .correlated=&plan->correlated,
    .queries=dependencies ? dependencies->queries : NULL,.query_count=dependencies ? dependencies->query_count : 0};
  turbodb_status_t status = orm_sql_bind_expression(&scope,ast->as.join.condition,max_depth,
      (orm_sql_expression_target){&node->condition,&node->slots,&node->slot_bytes,&node->query_slots,&node->query_slot_bytes},true,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (node->reversed) for (size_t i = 0; i < vec_size(&node->slots); ++i) {
    size_t *slot = vec_at(&node->slots,i);
    if (*slot < left->schema.count) *slot += right->schema.count;
    else if (*slot < node->schema.count) *slot -= left->schema.count;
  }
  return TURBODB_STATUS_OK;
}
/* TABLE is an opaque leaf: a nested query's FROM can share an enclosing span
 * without belonging to this SELECT's source tree. Membership follows AST edges. */
static turbodb_status_t from_subtree_member(const sqlparser_document *document,sqlparser_id root,
    sqlparser_id subtree,size_t max_depth,orm_tidesdb_sql_budget *budget,turbodb_error_t *error) {
  const size_t capacity=sqlparser_node_count(document);
  vec_t stack={0}; size_t bytes=0,pending=0,visited=0;
  turbodb_status_t status=orm_sql_work_zero(&stack,capacity,sizeof(from_visit),_Alignof(from_visit),budget,&bytes,error);
  if(status==TURBODB_STATUS_OK) *(from_visit *)vec_at(&stack,pending++)=(from_visit){.ast=root,.depth=1};
  bool found=false;
  while(status==TURBODB_STATUS_OK && pending && !found) {
    const from_visit visit=*(const from_visit *)vec_at_const(&stack,--pending);
    const sqlparser_node *ast=sqlparser_get_node(document,visit.ast);
    orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
    status=orm_tidesdb_sql_budget_reserve(budget,&charge,error);
    if(status!=TURBODB_STATUS_OK) break;
    if(visit.depth>max_depth || ++visited>capacity) {
      status=from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"FROM membership depth or capacity exceeded"); break;
    }
    if(!ast || (ast->kind!=SQLPARSER_TABLE && ast->kind!=SQLPARSER_JOIN)) {
      status=from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid FROM membership node"); break;
    }
    found=visit.ast==subtree;
    if(!found && ast->kind==SQLPARSER_JOIN) {
      enum { MEMBER_CHILDREN=2 };
      if(capacity<MEMBER_CHILDREN || pending>capacity-MEMBER_CHILDREN || visit.depth==SIZE_MAX) {
        status=from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"FROM membership traversal overflow"); break;
      }
      *(from_visit *)vec_at(&stack,pending++)=(from_visit){.ast=ast->as.join.right,.depth=visit.depth+1};
      *(from_visit *)vec_at(&stack,pending++)=(from_visit){.ast=ast->as.join.left,.depth=visit.depth+1};
    }
  }
  if(status==TURBODB_STATUS_OK && !found)
    status=from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"selected node is not a FROM subtree");
  const turbodb_status_t released=orm_sql_work_release(&stack,bytes,budget,status==TURBODB_STATUS_OK?error:NULL);
  return status==TURBODB_STATUS_OK?released:status;
}
turbodb_status_t orm_sql_from_lateral_prefixes_at(const orm_sql_query_scope *scope,
    sqlparser_id lateral,vec_t *out,size_t *bytes,turbodb_error_t *error) {
  const sqlparser_node *statement=scope&&scope->document?
      sqlparser_get_node(scope->document,scope->root):NULL;
  const sqlparser_node *target=scope&&scope->document?
      sqlparser_get_node(scope->document,lateral):NULL;
  if(!scope||!scope->budget||!scope->max_depth||!statement||statement->kind!=SQLPARSER_SELECT||
      !statement->as.select.from||!target||target->kind!=SQLPARSER_TABLE||!target->as.table.lateral||
      !target->as.table.query||!target->as.table.alias||
      !out||out->initialized||!bytes||*bytes)
    return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid LATERAL prefix inputs");
  if(sqlparser_get_dialect(scope->document)!=SQLPARSER_MYSQL||sqlparser_statements(scope->document).count!=1)
    return from_error(error,TURBODB_STATUS_UNSUPPORTED,"LATERAL prefixes require one MySQL statement");
  const size_t capacity=sqlparser_node_count(scope->document);
  vec_t stack={0},prefixes={0}; size_t stack_bytes=0,prefix_bytes=0,pending=0,entered=0;
  turbodb_status_t status=orm_sql_work_zero(&stack,capacity,sizeof(from_lateral_visit),
      _Alignof(from_lateral_visit),scope->budget,&stack_bytes,error);
  if(status==TURBODB_STATUS_OK) *(from_lateral_visit *)vec_at(&stack,pending++)=
      (from_lateral_visit){.ast=statement->as.select.from};
  bool found=false;
  /* Keep the active path: a SECOND ancestor contributes its completed FIRST
   * subtree. This preserves outer-join metadata without flattening leaf names. */
  while(status==TURBODB_STATUS_OK&&pending&&!found) {
    orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
    status=orm_tidesdb_sql_budget_reserve(scope->budget,&charge,error);
    if(status!=TURBODB_STATUS_OK) break;
    from_lateral_visit *visit=vec_at(&stack,pending-1);
    const sqlparser_node *ast=sqlparser_get_node(scope->document,visit->ast);
    if(!ast||(ast->kind!=SQLPARSER_TABLE&&ast->kind!=SQLPARSER_JOIN)) {
      status=from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid LATERAL FROM node"); break;
    }
    if(visit->phase==LATERAL_ENTER) {
      if(pending>scope->max_depth||++entered>capacity) {
        status=from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"LATERAL prefix depth or capacity exceeded"); break;
      }
      if(ast->kind==SQLPARSER_TABLE) {
        found=visit->ast==lateral;
        if(!found) --pending;
        continue;
      }
      if(ast->as.join.kind<SQLPARSER_JOIN_INNER||ast->as.join.kind>SQLPARSER_JOIN_NATURAL) {
        status=from_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported LATERAL prefix JOIN"); break;
      }
      visit->phase=LATERAL_FIRST;
    } else if(visit->phase==LATERAL_FIRST) visit->phase=LATERAL_SECOND;
    else { --pending; continue; }
    if(pending==capacity) {
      status=from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"LATERAL prefix traversal overflow"); break;
    }
    const bool reversed=ast->as.join.kind==SQLPARSER_JOIN_RIGHT;
    const bool first=visit->phase==LATERAL_FIRST;
    const sqlparser_id child=first!=reversed?ast->as.join.left:ast->as.join.right;
    *(from_lateral_visit *)vec_at(&stack,pending++)=(from_lateral_visit){.ast=child};
  }
  if(status==TURBODB_STATUS_OK&&!found)
    status=from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"LATERAL target is not in this SELECT FROM");
  if(status==TURBODB_STATUS_OK) {
    orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=pending;
    status=orm_tidesdb_sql_budget_reserve(scope->budget,&charge,error);
    if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_reserve(scope->budget,&charge,error);
  }
  size_t count=0;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<pending;++i)
    if(((const from_lateral_visit *)vec_at_const(&stack,i))->phase==LATERAL_SECOND) ++count;
  if(status==TURBODB_STATUS_OK&&count>scope->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    status=from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"LATERAL prefix count exceeds plan capacity");
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&prefixes,count,sizeof(sqlparser_id),
      _Alignof(sqlparser_id),scope->budget,&prefix_bytes,error);
  for(size_t i=0,index=0;status==TURBODB_STATUS_OK&&i<pending;++i) {
    const from_lateral_visit *visit=vec_at_const(&stack,i);
    if(visit->phase!=LATERAL_SECOND) continue;
    const sqlparser_node *ast=sqlparser_get_node(scope->document,visit->ast);
    *(sqlparser_id *)vec_at(&prefixes,index++)=ast->as.join.kind==SQLPARSER_JOIN_RIGHT?
        ast->as.join.right:ast->as.join.left;
  }
  const turbodb_status_t released=orm_sql_work_release(&stack,stack_bytes,scope->budget,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=released;
  if(status!=TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup=orm_sql_work_release(&prefixes,prefix_bytes,scope->budget,NULL);
    return cleanup==TURBODB_STATUS_OK?status:cleanup;
  }
  *out=prefixes; *bytes=prefix_bytes; return TURBODB_STATUS_OK;
}
static turbodb_status_t from_start(const sqlparser_document *document, sqlparser_id root, sqlparser_id subtree, size_t max_depth,
    orm_tidesdb_sql_budget *budget, orm_sql_from *plan, turbodb_error_t *error) {
  if (!document || !max_depth || !budget)
    return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid FROM inputs");
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || sqlparser_statements(document).count != 1)
    return from_error(error,TURBODB_STATUS_UNSUPPORTED,"expected one MySQL SELECT");
  const sqlparser_node *statement = sqlparser_get_node(document,root ? root : sqlparser_statements(document).first);
  if (statement && statement->kind == SQLPARSER_EXPLAIN) {
    if (statement->as.explain.query_plan || (statement->as.explain.format != SQLPARSER_EXPLAIN_DEFAULT &&
        statement->as.explain.format != SQLPARSER_EXPLAIN_TRADITIONAL))
      return from_error(error,TURBODB_STATUS_UNSUPPORTED,"EXPLAIN requires default or TRADITIONAL format");
    statement = sqlparser_get_node(document,statement->as.explain.statement);
  }
  if (!statement || statement->kind != SQLPARSER_SELECT || !statement->as.select.from)
    return from_error(error,TURBODB_STATUS_UNSUPPORTED,"expected SELECT with FROM");
  if(subtree) {
    const turbodb_status_t status=from_subtree_member(document,statement->as.select.from,subtree,max_depth,budget,error);
    if(status!=TURBODB_STATUS_OK) return status;
  }
  const size_t capacity = sqlparser_node_count(document);
  *plan = (orm_sql_from){.budget=budget,.root=subtree?subtree:statement->as.select.from,
      .statement_from=statement->as.select.from};
  turbodb_status_t status = from_charge(plan,ORM_SQL_BUDGET_AST_NODES,capacity,error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_capacity(budget,1,sizeof(*plan),0,&plan->metadata_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&plan->nodes,capacity,sizeof(orm_sql_from_node),
      _Alignof(orm_sql_from_node),budget,&plan->node_bytes,error);
  if (status == TURBODB_STATUS_OK) status = from_tree(document,max_depth,plan,subtree!=0,error);
  return status;
}
static turbodb_status_t from_tables(const sqlparser_document *document, sqlparser_id root, sqlparser_id subtree,
    size_t max_depth, orm_tidesdb_sql_budget *budget, vec_t *out, size_t *bytes, turbodb_error_t *error) {
  if (!out || !bytes || out->initialized || *bytes)
    return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"empty FROM table list required");
  orm_sql_from plan = {0}; vec_t names = {0}; size_t allocated = 0;
  turbodb_status_t status = from_start(document,root,subtree,max_depth,budget,&plan,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&names,plan.tables,sizeof(orm_sql_from_table),
      _Alignof(orm_sql_from_table),budget,&allocated,error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < plan.count; ++i) {
    const orm_sql_from_node *node = orm_tidesdb_sql_from_at(&plan,i);
    if (node->leaf) {
      orm_sql_from_table *entry = vec_at(&names,node->table);
      entry->ast = node->ast;
      memcpy(entry->name,node->name,sizeof(node->name));
      if (sqlparser_get_node(document,node->ast)->as.table.query) entry->derived = node->ast;
    }
  }
  const turbodb_status_t released = orm_tidesdb_sql_from_destroy(&plan,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup = orm_sql_work_release(&names,allocated,budget,NULL);
    return cleanup == TURBODB_STATUS_OK ? status : cleanup;
  }
  *out = names; *bytes = allocated; return TURBODB_STATUS_OK;
}
static turbodb_status_t from_bind_schema(const sqlparser_document *document, sqlparser_id root, sqlparser_id subtree,
    const orm_sql_table_schema *const *schemas, size_t schema_count,
    const orm_sql_type *parameter_types, size_t parameter_count, size_t max_depth,
    orm_tidesdb_sql_budget *budget, orm_sql_from *out, turbodb_error_t *error,
    const orm_sql_query_scope *dependencies) {
  if (!document || !schemas || !schema_count || !max_depth || !budget || !out || out->budget ||
      (parameter_count && !parameter_types)) return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid FROM bind arguments");
  const size_t outer_count=dependencies&&dependencies->outer_schema?
      dependencies->outer_schema->count:0;
  if(parameter_count>SIZE_MAX-outer_count)
    return from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"FROM parameter and correlation width overflow");
  const size_t total_parameters=parameter_count+outer_count;
  if (schema_count > budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] ||
      total_parameters > budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return from_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"FROM binding capacity exceeded");
  orm_sql_from plan = {0};
  turbodb_status_t status = from_start(document,root,subtree,max_depth,budget,&plan,error);
  if (status == TURBODB_STATUS_OK && plan.tables != schema_count)
    status = from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM schema count differs from table occurrences");
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&plan.parameter_types,total_parameters,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),budget,&plan.parameter_bytes,error);
  plan.parameter_marker_count=parameter_count;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < total_parameters; ++i) {
    const orm_sql_type type=i<parameter_count?parameter_types[i]:
        dependencies->outer_schema->columns[i-parameter_count].type;
    orm_sql_predicate validator;
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,type,NULL,&validator,error);
    if (status == TURBODB_STATUS_OK) *(orm_sql_type *)vec_at(&plan.parameter_types,i) = type;
  }
  for (size_t i = plan.count; status == TURBODB_STATUS_OK && i; --i) {
    orm_sql_from_node *node = vec_at(&plan.nodes,i-1);
    status = node->leaf ? from_leaf(&plan,node,document,schemas[node->table],error) :
        from_join_schema(document,&plan,node,error);
  }
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup = orm_tidesdb_sql_from_destroy(&plan,NULL);
    return cleanup == TURBODB_STATUS_OK ? status : cleanup;
  }
  *out = plan; return TURBODB_STATUS_OK;
}
static turbodb_status_t from_bind_conditions(const sqlparser_document *document,
    size_t max_depth, orm_sql_from *plan, const orm_sql_query_scope *dependencies,
    turbodb_error_t *error) {
  if (!document || !max_depth || !plan || !plan->budget || !plan->count)
    return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"prepared FROM plan required");
  if(plan->conditions_bound) return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM conditions are already bound");
  for (size_t i = 0; i < plan->count; ++i) {
    const orm_sql_from_node *node = vec_at_const(&plan->nodes,i);
    if (!node->leaf && (node->condition.budget || node->slots.initialized || node->query_slots.initialized))
      return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM conditions are already bound");
  }
  vec_t offsets = {0}; size_t offset_bytes = 0;
  turbodb_status_t status = orm_sql_bind_parameter_offsets(document,plan->parameter_marker_count,
      plan->budget,&offsets,&offset_bytes,error);
  for (size_t i = plan->count; status == TURBODB_STATUS_OK && i; --i) {
    orm_sql_from_node *node = vec_at(&plan->nodes,i-1);
    if (!node->leaf) status = from_join_condition(document,plan,node,&offsets,max_depth,dependencies,error);
  }
  const turbodb_status_t released = orm_sql_work_release(&offsets,offset_bytes,plan->budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  if(status==TURBODB_STATUS_OK) status=released;
  if(status==TURBODB_STATUS_OK) plan->conditions_bound=true;
  return status;
}
static turbodb_status_t from_bind(const sqlparser_document *document, sqlparser_id root,
    const orm_sql_table_schema *const *schemas, size_t schema_count,
    const orm_sql_type *parameter_types, size_t parameter_count, size_t max_depth,
    orm_tidesdb_sql_budget *budget, orm_sql_from *out, turbodb_error_t *error, const orm_sql_query_scope *dependencies) {
  turbodb_status_t status=from_bind_schema(document,root,0,schemas,schema_count,parameter_types,
      parameter_count,max_depth,budget,out,error,dependencies);
  if(status==TURBODB_STATUS_OK) status=from_bind_conditions(document,max_depth,out,dependencies,error);
  if(status!=TURBODB_STATUS_OK&&out&&out->budget) {
    const turbodb_status_t cleanup=orm_tidesdb_sql_from_destroy(out,NULL);
    if(cleanup!=TURBODB_STATUS_OK) return cleanup;
  }
  return status;
}

turbodb_status_t orm_tidesdb_sql_from_tables(const sqlparser_document *document,
    size_t max_depth, orm_tidesdb_sql_budget *budget, vec_t *out, size_t *bytes, turbodb_error_t *error) {
  return from_tables(document,0,0,max_depth,budget,out,bytes,error);
}
turbodb_status_t orm_tidesdb_sql_from_bind(const sqlparser_document *document,
    const orm_sql_table_schema *const *schemas, size_t schema_count,
    const orm_sql_type *parameter_types, size_t parameter_count, size_t max_depth,
    orm_tidesdb_sql_budget *budget, orm_sql_from *out, turbodb_error_t *error) {
  return from_bind(document,0,schemas,schema_count,parameter_types,parameter_count,max_depth,budget,out,error,NULL);
}
turbodb_status_t orm_tidesdb_sql_from_tables_at(const orm_sql_query_scope *scope,
    vec_t *out, size_t *bytes, turbodb_error_t *error) {
  if (!scope || !scope->root) return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM query block required");
  return from_tables(scope->document,scope->root,0,scope->max_depth,scope->budget,out,bytes,error);
}
turbodb_status_t orm_tidesdb_sql_from_bind_at(const orm_sql_query_scope *scope,
    const orm_sql_table_schema *const *schemas, size_t count, orm_sql_from *out, turbodb_error_t *error) {
  if (!scope || !scope->root) return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM query block required");
  return from_bind(scope->document,scope->root,schemas,count,scope->parameter_types,scope->parameter_count,
      scope->max_depth,scope->budget,out,error,scope);
}
turbodb_status_t orm_tidesdb_sql_from_bind_schema_at(const orm_sql_query_scope *scope,
    const orm_sql_table_schema *const *schemas,size_t count,orm_sql_from *out,turbodb_error_t *error) {
  if(!scope||!scope->root) return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM query block required");
  return from_bind_schema(scope->document,scope->root,0,schemas,count,scope->parameter_types,
      scope->parameter_count,scope->max_depth,scope->budget,out,error,scope);
}
turbodb_status_t orm_tidesdb_sql_from_bind_conditions_at(const orm_sql_query_scope *scope,
    orm_sql_from *plan,turbodb_error_t *error) {
  const sqlparser_node *statement=scope&&scope->document&&scope->root?
      sqlparser_get_node(scope->document,scope->root):NULL;
  if(!statement||statement->kind!=SQLPARSER_SELECT||!plan||plan->statement_from!=statement->as.select.from)
    return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM query block differs from prepared plan");
  return from_bind_conditions(scope->document,scope->max_depth,plan,scope,error);
}
turbodb_status_t orm_sql_from_subtree_tables_at(const orm_sql_query_scope *scope,
    sqlparser_id subtree,vec_t *out,size_t *bytes,turbodb_error_t *error) {
  if(!scope || !scope->root || !subtree)
    return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM subtree query block required");
  return from_tables(scope->document,scope->root,subtree,scope->max_depth,scope->budget,out,bytes,error);
}
turbodb_status_t orm_sql_from_subtree_schema_at(const orm_sql_query_scope *scope,
    sqlparser_id subtree,const orm_sql_table_schema *const *schemas,size_t count,
    orm_sql_from *out,turbodb_error_t *error) {
  if(!scope || !scope->root || !subtree)
    return from_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM subtree query block required");
  return from_bind_schema(scope->document,scope->root,subtree,schemas,count,scope->parameter_types,
      scope->parameter_count,scope->max_depth,scope->budget,out,error,scope);
}
