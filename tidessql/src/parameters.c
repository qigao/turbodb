#include "parameters.h"
#include "work.h"
#include "error.h"
#include <stdio.h>
#include <string.h>

typedef struct parameter_fact {
  orm_sql_type type;
  bool entered, known, markers;
} parameter_fact;
typedef struct parameter_visit {
  sqlparser_id node;
  size_t depth;
  bool after;
  orm_sql_type context;
  bool has_context;
} parameter_visit;
typedef struct parameter_inference {
  orm_sql_parameters *parameters;
  const orm_sql_binding_scope *scope;
  vec_t facts, stack, candidate, additions;
  size_t fact_bytes, stack_bytes, candidate_bytes, addition_bytes, pending, capacity, max_depth;
  bool cleanup_failed;
  turbodb_error_t *error;
} parameter_inference;
typedef struct parameter_compound_iterator {
  const sqlparser_node *node;
  sqlparser_id item;
  size_t position,remaining;
} parameter_compound_iterator;
typedef enum parameter_case_role {
  PARAMETER_CASE_COMPARISON,
  PARAMETER_CASE_PREDICATE,
  PARAMETER_CASE_RESULT
} parameter_case_role;
typedef enum parameter_case_phase {
  PARAMETER_CASE_OPERAND,
  PARAMETER_CASE_CONDITION,
  PARAMETER_CASE_BRANCH_RESULT,
  PARAMETER_CASE_OTHERWISE,
  PARAMETER_CASE_DONE
} parameter_case_phase;
typedef struct parameter_case_iterator {
  const sqlparser_node *node;
  sqlparser_id branch;
  size_t remaining;
  parameter_case_phase phase;
} parameter_case_iterator;
typedef struct parameter_call_iterator {
  const sqlparser_node *node;
  sqlparser_id item;
  size_t position,remaining;
  orm_sql_expr_function function;
} parameter_call_iterator;
static turbodb_status_t parameter_error(turbodb_error_t *error,
    turbodb_status_t status, const char *reason) {
  tdsql_error_set(error,status,reason); return status;
}
static turbodb_status_t parameter_steps(orm_tidesdb_sql_budget *budget,
    size_t count,turbodb_error_t *error) {
  orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=count;
  return orm_tidesdb_sql_budget_reserve(budget,&amount,error);
}
static turbodb_status_t parameter_at(parameter_inference *inference,const sqlparser_node *node,
    turbodb_status_t status,const char *reason) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message,sizeof(message),"TidesSQL parameter inference at byte %zu: %s",node->span.offset,reason);
  return parameter_error(inference->error,status,message);
}
static turbodb_status_t parameter_query(parameter_inference *inference,sqlparser_id node,
    const orm_sql_expr_query_binding **out) {
  size_t first=0,last=inference->scope->query_count;
  while(first<last) {
    const turbodb_status_t status=parameter_steps(inference->scope->budget,1,inference->error);
    if(status!=TURBODB_STATUS_OK) return status;
    const size_t middle=first+(last-first)/2;
    if(inference->scope->queries[middle].node<node) first=middle+1; else last=middle;
  }
  *out=first<inference->scope->query_count&&inference->scope->queries[first].node==node?
      &inference->scope->queries[first]:NULL;
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_parameters_close(orm_sql_parameters *parameters,turbodb_error_t *error) {
  if(!parameters) return TURBODB_STATUS_OK;
  vec_t *vectors[]={&parameters->offsets,&parameters->types,&parameters->resolved};
  const size_t bytes[]={parameters->offset_bytes,parameters->type_bytes,parameters->resolved_bytes};
  turbodb_status_t status=TURBODB_STATUS_OK;
  for(size_t i=0;i<sizeof(vectors)/sizeof(vectors[0]);++i) {
    const turbodb_status_t released=orm_sql_work_release(vectors[i],bytes[i],parameters->budget,
        status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  *parameters=(orm_sql_parameters){0}; return status;
}
turbodb_status_t orm_sql_parameters_open(const sqlparser_document *document,
    orm_tidesdb_sql_budget *budget,orm_sql_parameters *out,turbodb_error_t *error) {
  if(!document||!budget||!out||out->budget)
    return parameter_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid parameter metadata owner");
  if(sqlparser_get_dialect(document)!=SQLPARSER_MYSQL||sqlparser_statements(document).count!=1)
    return parameter_error(error,TURBODB_STATUS_UNSUPPORTED,"parameter inference requires one MySQL statement");
  const size_t nodes=sqlparser_node_count(document); size_t count=0;
  turbodb_status_t status=parameter_steps(budget,nodes,error);
  if(status!=TURBODB_STATUS_OK) return status;
  for(size_t i=1;i<=nodes;++i)
    if(sqlparser_get_node(document,(sqlparser_id)i)->kind==SQLPARSER_PARAMETER) ++count;
  *out=(orm_sql_parameters){.document=document,.budget=budget};
  status=orm_sql_bind_parameter_offsets(document,count,budget,&out->offsets,&out->offset_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->types,count,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),budget,&out->type_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->resolved,count,sizeof(bool),
      _Alignof(bool),budget,&out->resolved_bytes,error);
  if(status!=TURBODB_STATUS_OK) {
    const turbodb_status_t released=orm_sql_parameters_close(out,NULL);
    if(released!=TURBODB_STATUS_OK) return released;
  }
  return status;
}
turbodb_status_t orm_sql_parameters_types(const orm_sql_parameters *parameters,
    const orm_sql_type **out,size_t *count,turbodb_error_t *error) {
  if(!parameters||!parameters->budget||!out||!count)
    return parameter_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid parameter metadata view");
  for(size_t i=0;i<vec_size(&parameters->resolved);++i)
    if(!*(const bool *)vec_at_const(&parameters->resolved,i))
      return parameter_error(error,TURBODB_STATUS_INVALID_STATE,"parameter type has not been inferred");
  *out=vec_data_const(&parameters->types); *count=vec_size(&parameters->types);
  return TURBODB_STATUS_OK;
}
static bool parameter_arithmetic(sqlparser_operator op,orm_sql_arithmetic_op *out) {
  switch(op) {
    case SQLPARSER_OP_ADD: *out=ORM_SQL_ADD; break;
    case SQLPARSER_OP_SUBTRACT: *out=ORM_SQL_SUBTRACT; break;
    case SQLPARSER_OP_MULTIPLY: *out=ORM_SQL_MULTIPLY; break;
    case SQLPARSER_OP_DIVIDE: *out=ORM_SQL_DIVIDE; break;
    case SQLPARSER_OP_INTEGER_DIVIDE: *out=ORM_SQL_INTEGER_DIVIDE; break;
    case SQLPARSER_OP_MODULO: *out=ORM_SQL_MODULO; break;
    case SQLPARSER_OP_POSITIVE: *out=ORM_SQL_POSITIVE; break;
    case SQLPARSER_OP_NEGATE: *out=ORM_SQL_NEGATE; break;
    default: return false;
  }
  return true;
}
static bool parameter_comparison(sqlparser_operator op) {
  return op>=SQLPARSER_OP_EQ&&op<=SQLPARSER_OP_NULL_SAFE_EQ;
}
static bool parameter_like(sqlparser_operator op) {
  return op==SQLPARSER_OP_LIKE||op==SQLPARSER_OP_NOT_LIKE;
}
static bool parameter_logical(sqlparser_operator op) {
  return op==SQLPARSER_OP_NOT||op==SQLPARSER_OP_AND||op==SQLPARSER_OP_OR;
}
static bool parameter_compound(const sqlparser_node *node) {
  return node->kind==SQLPARSER_IN||node->kind==SQLPARSER_BETWEEN;
}
static turbodb_status_t parameter_compound_open(parameter_inference *inference,
    const sqlparser_node *node,parameter_compound_iterator *iterator) {
  if(!parameter_compound(node))
    return parameter_at(inference,node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid compound inference node");
  if(node->kind==SQLPARSER_IN&&(node->as.in.query||node->as.in.table||!node->as.in.items.count))
    return parameter_at(inference,node,TURBODB_STATUS_UNSUPPORTED,
        "parameter inference supports only a nonempty scalar IN list (#206)");
  if(node->kind==SQLPARSER_IN&&node->as.in.items.count>=inference->capacity)
    return parameter_at(inference,node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid IN list size");
  *iterator=(parameter_compound_iterator){.node=node,
    .item=node->kind==SQLPARSER_IN?node->as.in.items.first:0,
    .remaining=node->kind==SQLPARSER_IN?node->as.in.items.count+1:3};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t parameter_compound_next(parameter_inference *inference,
    parameter_compound_iterator *iterator,sqlparser_id *child,bool *available) {
  if(!iterator||!iterator->node||!child||!available)
    return parameter_error(inference->error,TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid compound inference iterator");
  *available=false;
  if(!iterator->remaining) {
    if(iterator->node->kind==SQLPARSER_IN&&iterator->item)
      return parameter_at(inference,iterator->node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid IN list");
    return TURBODB_STATUS_OK;
  }
  if(iterator->node->kind==SQLPARSER_BETWEEN) {
    *child=iterator->position==0?iterator->node->as.between.value:
        iterator->position==1?iterator->node->as.between.lower:iterator->node->as.between.upper;
  } else if(!iterator->position) *child=iterator->node->as.in.value;
  else {
    const sqlparser_node *item=sqlparser_get_node(inference->scope->document,iterator->item);
    if(!item) return parameter_at(inference,iterator->node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid IN list");
    *child=iterator->item; iterator->item=item->next;
  }
  if(!*child||*child>inference->capacity)
    return parameter_at(inference,iterator->node,TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid compound inference child");
  const turbodb_status_t status=parameter_steps(inference->scope->budget,1,inference->error);
  if(status!=TURBODB_STATUS_OK) return status;
  ++iterator->position; --iterator->remaining; *available=true;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t parameter_case_open(parameter_inference *inference,
    const sqlparser_node *node,parameter_case_iterator *iterator) {
  if(node->kind!=SQLPARSER_CASE)
    return parameter_at(inference,node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CASE inference node");
  if(!node->as.case_expr.branches.count||
      node->as.case_expr.branches.count>=inference->capacity)
    return parameter_at(inference,node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CASE branch count");
  *iterator=(parameter_case_iterator){.node=node,.branch=node->as.case_expr.branches.first,
    .remaining=node->as.case_expr.branches.count,
    .phase=node->as.case_expr.operand?PARAMETER_CASE_OPERAND:PARAMETER_CASE_CONDITION};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t parameter_case_next(parameter_inference *inference,
    parameter_case_iterator *iterator,sqlparser_id *child,parameter_case_role *role,bool *available) {
  if(!iterator||!iterator->node||!child||!role||!available)
    return parameter_error(inference->error,TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid CASE inference iterator");
  *available=false;
  while(iterator->phase!=PARAMETER_CASE_DONE) {
    const sqlparser_node *branch=NULL;
    if(iterator->phase==PARAMETER_CASE_OPERAND) {
      *child=iterator->node->as.case_expr.operand; *role=PARAMETER_CASE_COMPARISON;
      iterator->phase=PARAMETER_CASE_CONDITION;
    } else if(iterator->phase==PARAMETER_CASE_CONDITION) {
      if(!iterator->remaining) { iterator->phase=PARAMETER_CASE_OTHERWISE; continue; }
      branch=sqlparser_get_node(inference->scope->document,iterator->branch);
      if(!branch||branch->kind!=SQLPARSER_WHEN)
        return parameter_at(inference,iterator->node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CASE branch");
      *child=branch->as.when.condition;
      *role=iterator->node->as.case_expr.operand?PARAMETER_CASE_COMPARISON:PARAMETER_CASE_PREDICATE;
      iterator->phase=PARAMETER_CASE_BRANCH_RESULT;
    } else if(iterator->phase==PARAMETER_CASE_BRANCH_RESULT) {
      branch=sqlparser_get_node(inference->scope->document,iterator->branch);
      if(!branch||branch->kind!=SQLPARSER_WHEN)
        return parameter_at(inference,iterator->node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CASE branch");
      *child=branch->as.when.result; *role=PARAMETER_CASE_RESULT;
      if(!iterator->remaining||(--iterator->remaining==0)!=(branch->next==0))
        return parameter_at(inference,iterator->node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CASE branch list");
      iterator->branch=branch->next;
      iterator->phase=iterator->remaining?PARAMETER_CASE_CONDITION:PARAMETER_CASE_OTHERWISE;
    } else {
      iterator->phase=PARAMETER_CASE_DONE;
      if(!iterator->node->as.case_expr.otherwise) continue;
      *child=iterator->node->as.case_expr.otherwise; *role=PARAMETER_CASE_RESULT;
    }
    if(!*child||*child>inference->capacity)
      return parameter_at(inference,iterator->node,TURBODB_STATUS_INVALID_ARGUMENT,
          "invalid CASE inference child");
    const turbodb_status_t status=parameter_steps(inference->scope->budget,1,inference->error);
    if(status!=TURBODB_STATUS_OK) return status;
    *available=true; return TURBODB_STATUS_OK;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t parameter_call_open(parameter_inference *inference,
    const sqlparser_node *node,parameter_call_iterator *iterator) {
  if(node->kind!=SQLPARSER_CALL)
    return parameter_at(inference,node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid call inference node");
  orm_sql_expr_function function;
  turbodb_status_t status=orm_tidesdb_sql_expr_resolve_call(
      inference->scope->document,node,&function,inference->error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(node->as.call.arguments.count>=inference->capacity)
    return parameter_at(inference,node,TURBODB_STATUS_INVALID_ARGUMENT,"invalid function argument count");
  *iterator=(parameter_call_iterator){.node=node,.item=node->as.call.arguments.first,
    .remaining=node->as.call.arguments.count,.function=function};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t parameter_call_next(parameter_inference *inference,
    parameter_call_iterator *iterator,sqlparser_id *child,size_t *position,bool *available) {
  if(!iterator||!iterator->node||!child||!position||!available)
    return parameter_error(inference->error,TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid call inference iterator");
  *available=false;
  if(!iterator->remaining) {
    if(iterator->item)
      return parameter_at(inference,iterator->node,TURBODB_STATUS_INVALID_ARGUMENT,
          "invalid function argument list");
    return TURBODB_STATUS_OK;
  }
  const sqlparser_node *item=sqlparser_get_node(inference->scope->document,iterator->item);
  if(!item)
    return parameter_at(inference,iterator->node,TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid function argument list");
  *child=iterator->item; *position=iterator->position++;
  iterator->item=item->next; --iterator->remaining;
  const turbodb_status_t status=parameter_steps(inference->scope->budget,1,inference->error);
  if(status!=TURBODB_STATUS_OK) return status;
  *available=true; return TURBODB_STATUS_OK;
}
static turbodb_status_t parameter_compile(parameter_inference *inference,
    sqlparser_id root,const orm_sql_type *types,orm_sql_type *out) {
  orm_sql_binding_scope scope=*inference->scope;
  const size_t markers=vec_size(&inference->parameters->types);
  const size_t outer=scope.outer_schema?scope.outer_schema->count:0;
  if(markers>SIZE_MAX-outer)
    return parameter_error(inference->error,TURBODB_STATUS_LIMIT_EXCEEDED,
        "parameter and outer-frame width overflow");
  vec_t augmented={0}; size_t augmented_bytes=0;
  turbodb_status_t status=TURBODB_STATUS_OK;
  if(outer) {
    status=orm_sql_work_zero(&augmented,markers+outer,sizeof(orm_sql_type),
        _Alignof(orm_sql_type),scope.budget,&augmented_bytes,inference->error);
    if(status==TURBODB_STATUS_OK) {
      if(types&&markers) memcpy(vec_data(&augmented),types,markers*sizeof(orm_sql_type));
      for(size_t i=0;i<outer;++i)
        *(orm_sql_type *)vec_at(&augmented,markers+i)=scope.outer_schema->columns[i].type;
    }
  }
  scope.parameter_types=outer?vec_data_const(&augmented):types;
  scope.parameter_offsets=vec_data_const(&inference->parameters->offsets);
  scope.parameter_count=markers+outer; scope.parameter_marker_count=markers;
  orm_sql_expr expression={0}; vec_t slots={0},query_slots={0};
  size_t bytes=0,query_bytes=0;
  if(status==TURBODB_STATUS_OK) status=orm_sql_bind_expression(&scope,root,inference->max_depth,
      (orm_sql_expression_target){.program=&expression,.slots=&slots,.slot_bytes=&bytes,
        .query_slots=&query_slots,.query_slot_bytes=&query_bytes},false,inference->error);
  const orm_sql_type result=expression.result;
  turbodb_status_t released=orm_tidesdb_sql_expr_destroy(&expression,status==TURBODB_STATUS_OK?inference->error:NULL);
  if(released!=TURBODB_STATUS_OK) inference->cleanup_failed=true;
  if(status==TURBODB_STATUS_OK) status=released;
  released=orm_sql_work_release(&slots,bytes,scope.budget,status==TURBODB_STATUS_OK?inference->error:NULL);
  if(released!=TURBODB_STATUS_OK) inference->cleanup_failed=true;
  if(status==TURBODB_STATUS_OK) status=released;
  released=orm_sql_work_release(&query_slots,query_bytes,scope.budget,
      status==TURBODB_STATUS_OK?inference->error:NULL);
  if(released!=TURBODB_STATUS_OK) inference->cleanup_failed=true;
  if(status==TURBODB_STATUS_OK) status=released;
  released=orm_sql_work_release(&augmented,augmented_bytes,scope.budget,
      status==TURBODB_STATUS_OK?inference->error:NULL);
  if(released!=TURBODB_STATUS_OK) inference->cleanup_failed=true;
  if(status==TURBODB_STATUS_OK) status=released;
  if(status==TURBODB_STATUS_OK) *out=result;
  return status;
}
static turbodb_status_t parameter_push(parameter_inference *inference,parameter_visit visit) {
  if(!visit.node||visit.node>inference->capacity)
    return parameter_error(inference->error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid inference expression node");
  if(visit.depth>inference->max_depth||inference->pending==inference->capacity)
    return parameter_error(inference->error,TURBODB_STATUS_LIMIT_EXCEEDED,"parameter inference depth or workspace exceeded");
  *(parameter_visit *)vec_at(&inference->stack,inference->pending++)=visit;
  return TURBODB_STATUS_OK;
}
static bool parameter_known(const parameter_fact *fact) {
  return fact->known&&fact->type.kind!=TURBODB_VALUE_NULL;
}
static bool parameter_numeric_kind(turbodb_value_kind_t kind) {
  return kind==TURBODB_VALUE_INT64||kind==TURBODB_VALUE_UINT64||kind==TURBODB_VALUE_DOUBLE;
}
static void parameter_merge_domain(orm_sql_type type,bool *found,orm_sql_type *domain) {
  if(type.kind==TURBODB_VALUE_NULL) return;
  if(!*found) { *domain=type; *found=true; return; }
  if(domain->kind!=type.kind&&parameter_numeric_kind(domain->kind)&&parameter_numeric_kind(type.kind))
    domain->kind=TURBODB_VALUE_DOUBLE;
  domain->nullable=domain->nullable||type.nullable;
}
static bool parameter_peer(parameter_inference *inference,sqlparser_id id,
    const parameter_fact *fact,bool comparison,orm_sql_type *out) {
  if(parameter_known(fact)) { *out=fact->type; return true; }
  const sqlparser_node *node=sqlparser_get_node(inference->scope->document,id);
  orm_sql_arithmetic_op ignored;
  if(comparison&&((node->kind==SQLPARSER_UNARY&&parameter_arithmetic(node->as.unary.op,&ignored))||
      (node->kind==SQLPARSER_BINARY&&parameter_arithmetic(node->as.binary.op,&ignored)))) {
    *out=(orm_sql_type){TURBODB_VALUE_DOUBLE,true}; return true;
  }
  return false;
}
static bool parameter_selection_function(orm_sql_expr_function function) {
  return function==ORM_SQL_COALESCE||function==ORM_SQL_IFNULL;
}
static bool parameter_rounding_function(orm_sql_arithmetic_op operation) {
  return operation==ORM_SQL_ROUND||operation==ORM_SQL_TRUNCATE;
}
static turbodb_status_t parameter_call_fact(parameter_inference *inference,
    sqlparser_id id,const sqlparser_node *node,parameter_fact *fact) {
  parameter_call_iterator arguments;
  turbodb_status_t status=parameter_call_open(inference,node,&arguments);
  orm_sql_type domain={TURBODB_VALUE_NULL,true},operands[2]={{0}};
  bool found=false,known[2]={false,false},available=false;
  sqlparser_id child=0; size_t position=0;
  while(status==TURBODB_STATUS_OK&&
      (status=parameter_call_next(inference,&arguments,&child,&position,&available))==TURBODB_STATUS_OK&&available) {
    const parameter_fact *item=vec_at_const(&inference->facts,child-1);
    fact->markers=fact->markers||item->markers;
    orm_sql_type peer;
    const bool peer_known=parameter_peer(inference,child,item,true,&peer);
    if(parameter_selection_function(arguments.function)&&peer_known)
      parameter_merge_domain(peer,&found,&domain);
    else if(position<2&&peer_known) { operands[position]=peer; known[position]=true; }
  }
  if(status!=TURBODB_STATUS_OK) return status;
  if(!fact->markers) {
    status=parameter_compile(inference,id,NULL,&fact->type);
    fact->known=status==TURBODB_STATUS_OK; return status;
  }
  if(parameter_selection_function(arguments.function)) {
    if(found) { domain.nullable=true; fact->type=domain; fact->known=true; }
    return TURBODB_STATUS_OK;
  }
  if(arguments.function==ORM_SQL_NULLIF) {
    if(known[0]||known[1]) {
      fact->type=known[0]?operands[0]:operands[1]; fact->type.nullable=true; fact->known=true;
    }
    return TURBODB_STATUS_OK;
  }
  orm_sql_arithmetic_op operation;
  if(!orm_tidesdb_sql_expr_function_arithmetic(arguments.function,&operation))
    return parameter_at(inference,node,TURBODB_STATUS_INVALID_STATE,"invalid resolved function classification");
  if(operation==ORM_SQL_SIGN) {
    fact->type=(orm_sql_type){TURBODB_VALUE_INT64,true}; fact->known=true;
    return TURBODB_STATUS_OK;
  }
  orm_sql_arithmetic bound;
  if(!orm_tidesdb_sql_expr_arithmetic_binary(operation)) {
    if(!known[0]) return TURBODB_STATUS_OK;
    status=orm_tidesdb_sql_arithmetic_bind(operation,operands[0],NULL,&bound,inference->error);
  } else if(parameter_rounding_function(operation)) {
    if(!known[0]) return TURBODB_STATUS_OK;
    const orm_sql_type precision=known[1]?operands[1]:(orm_sql_type){TURBODB_VALUE_INT64,true};
    status=orm_tidesdb_sql_arithmetic_bind(operation,operands[0],&precision,&bound,inference->error);
  } else {
    if(!known[0]&&!known[1]) return TURBODB_STATUS_OK;
    const orm_sql_type left=known[0]?operands[0]:operands[1];
    const orm_sql_type right=known[1]?operands[1]:operands[0];
    status=orm_tidesdb_sql_arithmetic_bind(operation,left,&right,&bound,inference->error);
  }
  if(status==TURBODB_STATUS_OK) { fact->type=bound.result; fact->known=true; }
  return status;
}
static turbodb_status_t parameter_cast(parameter_inference *inference,const sqlparser_node *node,
    orm_sql_type *out) {
  orm_sql_cast_target target;
  turbodb_status_t status=orm_tidesdb_sql_expr_resolve_cast(inference->scope->document,node,
      inference->scope->budget,&target,inference->error);
  if(status==TURBODB_STATUS_OK) *out=(orm_sql_type){target==ORM_SQL_CAST_SIGNED?TURBODB_VALUE_INT64:
      target==ORM_SQL_CAST_UNSIGNED?TURBODB_VALUE_UINT64:TURBODB_VALUE_DOUBLE,true};
  return status;
}
static turbodb_status_t parameter_children(parameter_inference *inference,const sqlparser_node *node,
    sqlparser_id *left,sqlparser_id *right) {
  orm_sql_arithmetic_op ignored;
  *right=0;
  if(node->kind==SQLPARSER_CAST) *left=node->as.cast.expression;
  else if(node->kind==SQLPARSER_UNARY&&
      (parameter_arithmetic(node->as.unary.op,&ignored)||node->as.unary.op==SQLPARSER_OP_NOT))
    *left=node->as.unary.operand;
  else if(node->kind==SQLPARSER_BINARY&&
      (!node->as.binary.escape||parameter_like(node->as.binary.op))&&
      (parameter_comparison(node->as.binary.op)||parameter_arithmetic(node->as.binary.op,&ignored)||
       parameter_like(node->as.binary.op)||node->as.binary.op==SQLPARSER_OP_AND||node->as.binary.op==SQLPARSER_OP_OR)) {
    *left=node->as.binary.left; *right=node->as.binary.right;
  } else return parameter_at(inference,node,TURBODB_STATUS_UNSUPPORTED,
      "parameter inference supports scalar arithmetic, comparisons, scalar-list IN, BETWEEN, CASE, LIKE, boolean logic, numeric CAST and supported scalar functions only (#206)");
  return TURBODB_STATUS_OK;
}
static turbodb_status_t parameter_facts(parameter_inference *inference,sqlparser_id root) {
  turbodb_status_t status=parameter_push(inference,(parameter_visit){.node=root,.depth=1});
  while(status==TURBODB_STATUS_OK&&inference->pending) {
    const parameter_visit visit=*(const parameter_visit *)vec_at_const(&inference->stack,--inference->pending);
    const sqlparser_node *node=sqlparser_get_node(inference->scope->document,visit.node);
    parameter_fact *fact=vec_at(&inference->facts,visit.node-1);
    status=parameter_steps(inference->scope->budget,1,inference->error);
    if(status!=TURBODB_STATUS_OK) break;
    const orm_sql_expr_query_binding *query=NULL;
    status=parameter_query(inference,visit.node,&query);
    if(status!=TURBODB_STATUS_OK) break;
    if(!visit.after) {
      if(fact->entered) return parameter_error(inference->error,TURBODB_STATUS_INVALID_ARGUMENT,"inference expression is not a tree");
      fact->entered=true;
      if(node->kind==SQLPARSER_PARAMETER) { fact->markers=true; continue; }
      if(node->kind==SQLPARSER_NAME||node->kind==SQLPARSER_NULL||node->kind==SQLPARSER_NUMBER||
          node->kind==SQLPARSER_STRING||node->kind==SQLPARSER_BOOLEAN||node->kind==SQLPARSER_VARIABLE) {
        status=parameter_compile(inference,visit.node,NULL,&fact->type);
        fact->known=status==TURBODB_STATUS_OK; continue;
      }
      if(query) {
        if(query->type.kind!=ORM_SQL_SUBQUERY_IN&&query->type.kind!=ORM_SQL_SUBQUERY_NOT_IN) {
          fact->type=query->type.result; fact->known=true; continue;
        }
        if(node->kind!=SQLPARSER_IN||!node->as.in.value)
          return parameter_at(inference,node,TURBODB_STATUS_INVALID_ARGUMENT,
              "invalid bound IN query inference node");
        status=parameter_push(inference,(parameter_visit){.node=visit.node,.depth=visit.depth,.after=true});
        if(status==TURBODB_STATUS_OK)
          status=parameter_push(inference,(parameter_visit){.node=node->as.in.value,.depth=visit.depth+1});
        continue;
      }
      if(parameter_compound(node)) {
        parameter_compound_iterator children;
        status=parameter_compound_open(inference,node,&children);
        if(status==TURBODB_STATUS_OK)
          status=parameter_push(inference,(parameter_visit){.node=visit.node,.depth=visit.depth,.after=true});
        bool available=false; sqlparser_id child=0;
        while(status==TURBODB_STATUS_OK&&
            (status=parameter_compound_next(inference,&children,&child,&available))==TURBODB_STATUS_OK&&available)
          status=parameter_push(inference,(parameter_visit){.node=child,.depth=visit.depth+1});
        if(status!=TURBODB_STATUS_OK) break;
        continue;
      }
      if(node->kind==SQLPARSER_CASE) {
        parameter_case_iterator children;
        status=parameter_case_open(inference,node,&children);
        if(status==TURBODB_STATUS_OK)
          status=parameter_push(inference,(parameter_visit){.node=visit.node,.depth=visit.depth,.after=true});
        bool available=false; sqlparser_id child=0; parameter_case_role role;
        while(status==TURBODB_STATUS_OK&&
            (status=parameter_case_next(inference,&children,&child,&role,&available))==TURBODB_STATUS_OK&&available)
          status=parameter_push(inference,(parameter_visit){.node=child,.depth=visit.depth+1});
        if(status!=TURBODB_STATUS_OK) break;
        continue;
      }
      if(node->kind==SQLPARSER_CALL) {
        parameter_call_iterator arguments;
        status=parameter_call_open(inference,node,&arguments);
        if(status==TURBODB_STATUS_OK)
          status=parameter_push(inference,(parameter_visit){.node=visit.node,.depth=visit.depth,.after=true});
        bool available=false; sqlparser_id child=0; size_t position=0;
        while(status==TURBODB_STATUS_OK&&
            (status=parameter_call_next(inference,&arguments,&child,&position,&available))==TURBODB_STATUS_OK&&available)
          status=parameter_push(inference,(parameter_visit){.node=child,.depth=visit.depth+1});
        if(status!=TURBODB_STATUS_OK) break;
        continue;
      }
    }
    if(visit.after&&query) {
      const parameter_fact *probe=vec_at_const(&inference->facts,node->as.in.value-1);
      fact->markers=probe->markers; fact->type=query->type.result; fact->known=true;
      continue;
    }
    if(visit.after&&parameter_compound(node)) {
      parameter_compound_iterator children;
      status=parameter_compound_open(inference,node,&children);
      bool available=false; sqlparser_id child=0;
      while(status==TURBODB_STATUS_OK&&
          (status=parameter_compound_next(inference,&children,&child,&available))==TURBODB_STATUS_OK&&available)
        fact->markers=fact->markers||((const parameter_fact *)vec_at_const(&inference->facts,child-1))->markers;
      if(status!=TURBODB_STATUS_OK) break;
      if(!fact->markers) {
        status=parameter_compile(inference,visit.node,NULL,&fact->type); fact->known=status==TURBODB_STATUS_OK;
      } else {
        fact->type=(orm_sql_type){TURBODB_VALUE_BOOLEAN,true}; fact->known=true;
      }
      continue;
    }
    if(visit.after&&node->kind==SQLPARSER_CASE) {
      parameter_case_iterator children;
      status=parameter_case_open(inference,node,&children);
      bool available=false,found=false; sqlparser_id child=0; parameter_case_role role;
      orm_sql_type domain={TURBODB_VALUE_NULL,true};
      while(status==TURBODB_STATUS_OK&&
          (status=parameter_case_next(inference,&children,&child,&role,&available))==TURBODB_STATUS_OK&&available) {
        const parameter_fact *item=vec_at_const(&inference->facts,child-1);
        fact->markers=fact->markers||item->markers;
        if(role==PARAMETER_CASE_RESULT&&parameter_known(item))
          parameter_merge_domain(item->type,&found,&domain);
      }
      if(status!=TURBODB_STATUS_OK) break;
      if(!fact->markers) {
        status=parameter_compile(inference,visit.node,NULL,&fact->type); fact->known=status==TURBODB_STATUS_OK;
      } else if(found) {
        domain.nullable=true; fact->type=domain; fact->known=true;
      }
      continue;
    }
    if(visit.after&&node->kind==SQLPARSER_CALL) {
      status=parameter_call_fact(inference,visit.node,node,fact);
      if(status!=TURBODB_STATUS_OK) break;
      continue;
    }
    sqlparser_id left=0,right=0;
    status=parameter_children(inference,node,&left,&right);
    if(status!=TURBODB_STATUS_OK) break;
    if(!visit.after) {
      status=parameter_push(inference,(parameter_visit){.node=visit.node,.depth=visit.depth,.after=true});
      if(status==TURBODB_STATUS_OK&&right) status=parameter_push(inference,(parameter_visit){.node=right,.depth=visit.depth+1});
      if(status==TURBODB_STATUS_OK) status=parameter_push(inference,(parameter_visit){.node=left,.depth=visit.depth+1});
      continue;
    }
    const parameter_fact *a=vec_at_const(&inference->facts,left-1);
    const parameter_fact *b=right?vec_at_const(&inference->facts,right-1):NULL;
    fact->markers=a->markers||(b&&b->markers);
    if(!fact->markers) {
      status=parameter_compile(inference,visit.node,NULL,&fact->type); fact->known=status==TURBODB_STATUS_OK;
    } else if(node->kind==SQLPARSER_CAST) {
      status=parameter_cast(inference,node,&fact->type); fact->known=status==TURBODB_STATUS_OK;
    } else if((right&&(parameter_comparison(node->as.binary.op)||parameter_like(node->as.binary.op)||
        parameter_logical(node->as.binary.op)))||
        (node->kind==SQLPARSER_UNARY&&parameter_logical(node->as.unary.op))) {
      fact->type=(orm_sql_type){TURBODB_VALUE_BOOLEAN,true}; fact->known=true;
    } else if(parameter_known(a)||(b&&parameter_known(b))) {
      orm_sql_arithmetic_op operation; orm_sql_arithmetic bound;
      (void)parameter_arithmetic(right?node->as.binary.op:node->as.unary.op,&operation);
      const orm_sql_type at=a->known?a->type:b->type;
      const orm_sql_type bt=b?(b->known?b->type:a->type):(orm_sql_type){0};
      status=orm_tidesdb_sql_arithmetic_bind(operation,at,b?&bt:NULL,&bound,inference->error);
      if(status==TURBODB_STATUS_OK) { fact->type=bound.result; fact->known=true; }
    }
  }
  return status;
}
static turbodb_status_t parameter_slot(parameter_inference *inference,const sqlparser_node *node,
    orm_sql_type type) {
  const size_t count=vec_size(&inference->parameters->offsets); size_t first=0,last=count;
  while(first<last) {
    const turbodb_status_t status=parameter_steps(inference->scope->budget,1,inference->error);
    if(status!=TURBODB_STATUS_OK) return status;
    const size_t mid=first+(last-first)/2;
    if(*(const uint64_t *)vec_at_const(&inference->parameters->offsets,mid)<node->span.offset) first=mid+1;
    else last=mid;
  }
  if(first==count||*(const uint64_t *)vec_at_const(&inference->parameters->offsets,first)!=node->span.offset)
    return parameter_error(inference->error,TURBODB_STATUS_INVALID_ARGUMENT,"parameter occurrence is not indexed");
  type.nullable=true;
  if(*(const bool *)vec_at_const(&inference->parameters->resolved,first)&&
      ((const orm_sql_type *)vec_at_const(&inference->parameters->types,first))->kind!=type.kind)
    return parameter_at(inference,node,TURBODB_STATUS_SQL_ERROR,"parameter already has a different derived type");
  *(orm_sql_type *)vec_at(&inference->candidate,first)=type;
  *(bool *)vec_at(&inference->additions,first)=true;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t parameter_call_propagate(parameter_inference *inference,
    const sqlparser_node *node,parameter_visit visit) {
  parameter_call_iterator arguments;
  turbodb_status_t status=parameter_call_open(inference,node,&arguments);
  if(status!=TURBODB_STATUS_OK) return status;
  orm_sql_type expected={TURBODB_VALUE_TEXT,true}; bool found=false,available=false;
  sqlparser_id child=0; size_t position=0;
  if(parameter_selection_function(arguments.function)||arguments.function==ORM_SQL_NULLIF) {
    while(status==TURBODB_STATUS_OK&&
        (status=parameter_call_next(inference,&arguments,&child,&position,&available))==TURBODB_STATUS_OK&&available) {
      const parameter_fact *item=vec_at_const(&inference->facts,child-1);
      orm_sql_type peer;
      if(parameter_selection_function(arguments.function)) {
        if(parameter_peer(inference,child,item,true,&peer)) parameter_merge_domain(peer,&found,&expected);
      } else if(!found) found=parameter_peer(inference,child,item,true,&expected);
    }
    if(status!=TURBODB_STATUS_OK) return status;
    if(!found&&visit.has_context) expected=visit.context;
    status=parameter_call_open(inference,node,&arguments);
    while(status==TURBODB_STATUS_OK&&
        (status=parameter_call_next(inference,&arguments,&child,&position,&available))==TURBODB_STATUS_OK&&available) {
      parameter_visit item={.node=child,.depth=visit.depth+1,.has_context=true,.context=expected};
      const sqlparser_node *item_node=sqlparser_get_node(inference->scope->document,child);
      if(arguments.function==ORM_SQL_NULLIF&&
          (item_node->kind==SQLPARSER_UNARY||item_node->kind==SQLPARSER_BINARY)) item.has_context=false;
      status=parameter_push(inference,item);
    }
    return status;
  }
  orm_sql_arithmetic_op operation;
  if(!orm_tidesdb_sql_expr_function_arithmetic(arguments.function,&operation))
    return parameter_at(inference,node,TURBODB_STATUS_INVALID_STATE,"invalid resolved function classification");
  sqlparser_id children[2]={0,0}; const parameter_fact *facts[2]={NULL,NULL}; size_t count=0;
  while(status==TURBODB_STATUS_OK&&
      (status=parameter_call_next(inference,&arguments,&child,&position,&available))==TURBODB_STATUS_OK&&available) {
    if(position>=sizeof(children)/sizeof(children[0]))
      return parameter_at(inference,node,TURBODB_STATUS_INVALID_ARGUMENT,"numeric function has too many arguments");
    children[position]=child; facts[position]=vec_at_const(&inference->facts,child-1); count=position+1;
  }
  if(status!=TURBODB_STATUS_OK) return status;
  orm_sql_type contexts[2]={{TURBODB_VALUE_DOUBLE,true},{TURBODB_VALUE_INT64,true}};
  if(operation!=ORM_SQL_SIGN&&visit.has_context) contexts[0]=visit.context;
  if(operation==ORM_SQL_MODULO&&count==2) {
    (void)parameter_peer(inference,children[1],facts[1],false,&contexts[0]);
    (void)parameter_peer(inference,children[0],facts[0],false,&contexts[1]);
    if(!facts[0]->known&&!facts[1]->known) contexts[1]=contexts[0];
  }
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<count;++i)
    status=parameter_push(inference,(parameter_visit){.node=children[i],.depth=visit.depth+1,
        .has_context=true,.context=contexts[i]});
  return status;
}
static turbodb_status_t parameter_propagate(parameter_inference *inference,sqlparser_id root,
    const orm_sql_type *context) {
  turbodb_status_t status=parameter_push(inference,(parameter_visit){.node=root,.depth=1,
      .has_context=context!=NULL,.context=context?*context:(orm_sql_type){0}});
  while(status==TURBODB_STATUS_OK&&inference->pending) {
    const parameter_visit visit=*(const parameter_visit *)vec_at_const(&inference->stack,--inference->pending);
    const parameter_fact *fact=vec_at_const(&inference->facts,visit.node-1);
    if(!fact->markers) continue;
    status=parameter_steps(inference->scope->budget,1,inference->error);
    if(status!=TURBODB_STATUS_OK) break;
    const sqlparser_node *node=sqlparser_get_node(inference->scope->document,visit.node);
    if(node->kind==SQLPARSER_PARAMETER) {
      status=parameter_slot(inference,node,visit.has_context?visit.context:(orm_sql_type){TURBODB_VALUE_TEXT,true}); continue;
    }
    const orm_sql_expr_query_binding *query=NULL;
    status=parameter_query(inference,visit.node,&query);
    if(status!=TURBODB_STATUS_OK) break;
    if(query) {
      if(query->type.kind==ORM_SQL_SUBQUERY_IN||query->type.kind==ORM_SQL_SUBQUERY_NOT_IN)
        status=parameter_push(inference,(parameter_visit){.node=node->as.in.value,
            .depth=visit.depth+1,.has_context=true,.context=query->type.element});
      continue;
    }
    if(parameter_compound(node)) {
      parameter_compound_iterator children;
      status=parameter_compound_open(inference,node,&children);
      orm_sql_type expected={TURBODB_VALUE_TEXT,true}; bool found=false,available=false; sqlparser_id child=0;
      while(status==TURBODB_STATUS_OK&&
          (status=parameter_compound_next(inference,&children,&child,&available))==TURBODB_STATUS_OK&&available) {
        const parameter_fact *item=vec_at_const(&inference->facts,child-1);
        if(!found) found=parameter_peer(inference,child,item,true,&expected);
      }
      if(status!=TURBODB_STATUS_OK) break;
      status=parameter_compound_open(inference,node,&children);
      while(status==TURBODB_STATUS_OK&&
          (status=parameter_compound_next(inference,&children,&child,&available))==TURBODB_STATUS_OK&&available) {
        parameter_visit item={.node=child,.depth=visit.depth+1,.has_context=true,.context=expected};
        const sqlparser_node *item_node=sqlparser_get_node(inference->scope->document,child);
        if(item_node->kind==SQLPARSER_UNARY||item_node->kind==SQLPARSER_BINARY) item.has_context=false;
        status=parameter_push(inference,item);
      }
      if(status!=TURBODB_STATUS_OK) break;
      continue;
    }
    if(node->kind==SQLPARSER_CASE) {
      parameter_case_iterator children;
      status=parameter_case_open(inference,node,&children);
      orm_sql_type comparison={TURBODB_VALUE_TEXT,true}; bool comparison_found=false;
      orm_sql_type result={TURBODB_VALUE_TEXT,true}; bool result_found=false,available=false;
      sqlparser_id child=0; parameter_case_role role;
      while(status==TURBODB_STATUS_OK&&
          (status=parameter_case_next(inference,&children,&child,&role,&available))==TURBODB_STATUS_OK&&available) {
        const parameter_fact *item=vec_at_const(&inference->facts,child-1);
        orm_sql_type peer;
        if(role==PARAMETER_CASE_COMPARISON&&!comparison_found)
          comparison_found=parameter_peer(inference,child,item,true,&comparison);
        else if(role==PARAMETER_CASE_RESULT&&parameter_peer(inference,child,item,true,&peer))
          parameter_merge_domain(peer,&result_found,&result);
      }
      if(status!=TURBODB_STATUS_OK) break;
      if(!result_found&&visit.has_context) result=visit.context;
      status=parameter_case_open(inference,node,&children);
      while(status==TURBODB_STATUS_OK&&
          (status=parameter_case_next(inference,&children,&child,&role,&available))==TURBODB_STATUS_OK&&available) {
        const orm_sql_type expected=role==PARAMETER_CASE_COMPARISON?comparison:
            role==PARAMETER_CASE_PREDICATE?(orm_sql_type){TURBODB_VALUE_BOOLEAN,true}:result;
        parameter_visit item={.node=child,.depth=visit.depth+1,.has_context=true,.context=expected};
        const sqlparser_node *item_node=sqlparser_get_node(inference->scope->document,child);
        if(role==PARAMETER_CASE_COMPARISON&&
            (item_node->kind==SQLPARSER_UNARY||item_node->kind==SQLPARSER_BINARY)) item.has_context=false;
        status=parameter_push(inference,item);
      }
      if(status!=TURBODB_STATUS_OK) break;
      continue;
    }
    if(node->kind==SQLPARSER_CALL) {
      status=parameter_call_propagate(inference,node,visit);
      if(status!=TURBODB_STATUS_OK) break;
      continue;
    }
    sqlparser_id left=0,right=0; status=parameter_children(inference,node,&left,&right);
    if(status!=TURBODB_STATUS_OK) break;
    const parameter_fact *a=vec_at_const(&inference->facts,left-1);
    const parameter_fact *b=right?vec_at_const(&inference->facts,right-1):NULL;
    orm_sql_type expected={TURBODB_VALUE_DOUBLE,true};
    const bool comparison=right&&parameter_comparison(node->as.binary.op);
    const bool like=right&&parameter_like(node->as.binary.op);
    const bool logical=(right&&parameter_logical(node->as.binary.op))||
        (node->kind==SQLPARSER_UNARY&&parameter_logical(node->as.unary.op));
    if(node->kind==SQLPARSER_CAST) expected=fact->type;
    else if(like) expected=(orm_sql_type){TURBODB_VALUE_TEXT,true};
    else if(logical) expected=(orm_sql_type){TURBODB_VALUE_BOOLEAN,true};
    else if(comparison) expected=(orm_sql_type){TURBODB_VALUE_TEXT,true};
    else if(visit.has_context) expected=visit.context;
    parameter_visit children[]={{.node=left,.depth=visit.depth+1,.has_context=true,.context=expected},
      {.node=right,.depth=visit.depth+1,.has_context=true,.context=expected}};
    if(right) {
      (void)parameter_peer(inference,right,b,comparison,&children[0].context);
      (void)parameter_peer(inference,left,a,comparison,&children[1].context);
      /* Comparisons determine direct marker types, not arithmetic subtrees. */
      if(comparison) for(size_t i=0;i<sizeof(children)/sizeof(children[0]);++i) {
        const sqlparser_node *child=sqlparser_get_node(inference->scope->document,children[i].node);
        if(child->kind==SQLPARSER_UNARY||child->kind==SQLPARSER_BINARY) children[i].has_context=false;
      }
      status=parameter_push(inference,children[1]);
    }
    if(status==TURBODB_STATUS_OK) status=parameter_push(inference,children[0]);
  }
  return status;
}
turbodb_status_t orm_sql_parameters_infer(orm_sql_parameters *parameters,
    const orm_sql_binding_scope *scope,sqlparser_id root,const orm_sql_type *context,
    size_t max_depth,turbodb_error_t *error) {
  if(!parameters||!parameters->budget||!scope||scope->document!=parameters->document||
      scope->budget!=parameters->budget||!scope->schema||(!scope->schema->columns&&scope->schema->count)||
      (scope->outer_schema&&!scope->outer_schema->columns&&scope->outer_schema->count)||
      !root||root>sqlparser_node_count(scope->document)||!max_depth||max_depth==SIZE_MAX||
      scope->parameter_count||scope->parameter_types||scope->parameter_offsets||
      (scope->query_count&&!scope->queries)||scope->substitutions||scope->substitution_count||
      scope->capture_slots||scope->capture_count||scope->correlated)
    return parameter_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid scalar inference scope");
  const size_t nodes=sqlparser_node_count(scope->document);
  turbodb_status_t status=parameter_steps(scope->budget,scope->query_count,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<scope->query_count;++i) {
    const orm_sql_expr_query_binding *query=&scope->queries[i];
    if(!query->node||query->node>nodes||(i&&scope->queries[i-1].node>=query->node))
      status=parameter_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
          "parameter inference query dependencies must have distinct ordered node ids");
  }
  if(status!=TURBODB_STATUS_OK) return status;
  if(context) {
    orm_sql_predicate validator;
    const turbodb_status_t validated=orm_tidesdb_sql_predicate_bind(
        ORM_SQL_IS_NULL,*context,NULL,&validator,error);
    if(validated!=TURBODB_STATUS_OK) return validated;
    if(context->kind==TURBODB_VALUE_NULL)
      return parameter_error(error,TURBODB_STATUS_TYPE_ERROR,"NULL is not a parameter inference context");
  }
  parameter_inference inference={.parameters=parameters,.scope=scope,.error=error,
    .capacity=nodes,.max_depth=max_depth};
  const size_t count=vec_size(&parameters->types);
  status=orm_sql_work_zero(&inference.facts,inference.capacity,sizeof(parameter_fact),
      _Alignof(parameter_fact),scope->budget,&inference.fact_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&inference.stack,inference.capacity,sizeof(parameter_visit),
      _Alignof(parameter_visit),scope->budget,&inference.stack_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&inference.candidate,count,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),scope->budget,&inference.candidate_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&inference.additions,count,sizeof(bool),
      _Alignof(bool),scope->budget,&inference.addition_bytes,error);
  if(status==TURBODB_STATUS_OK) status=parameter_facts(&inference,root);
  if(status==TURBODB_STATUS_OK) status=parameter_propagate(&inference,root,context);
  orm_sql_type ignored;
  if(status==TURBODB_STATUS_OK) status=parameter_compile(&inference,root,vec_data_const(&inference.candidate),&ignored);
  if(status==TURBODB_STATUS_OK) status=parameter_steps(scope->budget,count,error);
  if(status==TURBODB_STATUS_OK) for(size_t i=0;i<count;++i) {
    if(!*(const bool *)vec_at_const(&inference.additions,i)) continue;
    const orm_sql_type *type=vec_at_const(&inference.candidate,i);
    *(orm_sql_type *)vec_at(&parameters->types,i)=*type;
    *(bool *)vec_at(&parameters->resolved,i)=true;
  }
  vec_t *vectors[]={&inference.facts,&inference.stack,&inference.candidate,&inference.additions};
  const size_t bytes[]={inference.fact_bytes,inference.stack_bytes,inference.candidate_bytes,inference.addition_bytes};
  for(size_t i=0;i<sizeof(vectors)/sizeof(vectors[0]);++i) {
    const turbodb_status_t released=orm_sql_work_release(vectors[i],bytes[i],scope->budget,status==TURBODB_STATUS_OK?error:NULL);
    if(released!=TURBODB_STATUS_OK) inference.cleanup_failed=true;
    if(status==TURBODB_STATUS_OK) status=released;
  }
  if(inference.cleanup_failed||status==TURBODB_STATUS_INVALID_STATE||status==TURBODB_STATUS_CLEANUP_FAILED) {
    const turbodb_status_t released=orm_sql_parameters_close(parameters,NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  return status;
}
