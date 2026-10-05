#include "parameters.h"
#include "work.h"
#include "error.h"
#include <stdio.h>

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
static turbodb_status_t parameter_compile(parameter_inference *inference,
    sqlparser_id root,const orm_sql_type *types,orm_sql_type *out) {
  orm_sql_binding_scope scope=*inference->scope;
  scope.parameter_types=types; scope.parameter_offsets=vec_data_const(&inference->parameters->offsets);
  scope.parameter_count=vec_size(&inference->parameters->types);
  orm_sql_expr expression={0}; vec_t slots={0}; size_t bytes=0;
  turbodb_status_t status=orm_sql_bind_expression(&scope,root,inference->max_depth,
      (orm_sql_expression_target){.program=&expression,.slots=&slots,.slot_bytes=&bytes},false,inference->error);
  const orm_sql_type result=expression.result;
  turbodb_status_t released=orm_tidesdb_sql_expr_destroy(&expression,status==TURBODB_STATUS_OK?inference->error:NULL);
  if(released!=TURBODB_STATUS_OK) inference->cleanup_failed=true;
  if(status==TURBODB_STATUS_OK) status=released;
  released=orm_sql_work_release(&slots,bytes,scope.budget,status==TURBODB_STATUS_OK?inference->error:NULL);
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
  else if(node->kind==SQLPARSER_UNARY&&parameter_arithmetic(node->as.unary.op,&ignored))
    *left=node->as.unary.operand;
  else if(node->kind==SQLPARSER_BINARY&&!node->as.binary.escape&&
      (parameter_comparison(node->as.binary.op)||parameter_arithmetic(node->as.binary.op,&ignored))) {
    *left=node->as.binary.left; *right=node->as.binary.right;
  } else return parameter_at(inference,node,TURBODB_STATUS_UNSUPPORTED,
      "parameter inference supports scalar arithmetic comparisons and numeric CAST only (#206)");
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
    if(!visit.after) {
      if(fact->entered) return parameter_error(inference->error,TURBODB_STATUS_INVALID_ARGUMENT,"inference expression is not a tree");
      fact->entered=true;
      if(node->kind==SQLPARSER_PARAMETER) { fact->markers=true; continue; }
      if(node->kind==SQLPARSER_NAME||node->kind==SQLPARSER_NULL||node->kind==SQLPARSER_NUMBER||
          node->kind==SQLPARSER_STRING||node->kind==SQLPARSER_BOOLEAN||node->kind==SQLPARSER_VARIABLE) {
        status=parameter_compile(inference,visit.node,NULL,&fact->type);
        fact->known=status==TURBODB_STATUS_OK; continue;
      }
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
    } else if(right&&parameter_comparison(node->as.binary.op)) {
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
    sqlparser_id left=0,right=0; status=parameter_children(inference,node,&left,&right);
    if(status!=TURBODB_STATUS_OK) break;
    const parameter_fact *a=vec_at_const(&inference->facts,left-1);
    const parameter_fact *b=right?vec_at_const(&inference->facts,right-1):NULL;
    orm_sql_type expected={TURBODB_VALUE_DOUBLE,true};
    const bool comparison=right&&parameter_comparison(node->as.binary.op);
    if(node->kind==SQLPARSER_CAST) expected=fact->type;
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
      !root||root>sqlparser_node_count(scope->document)||!max_depth||max_depth==SIZE_MAX||
      scope->parameter_count||scope->parameter_types||scope->parameter_offsets||scope->outer_schema||
      scope->queries||scope->query_count||scope->substitutions||scope->substitution_count||
      scope->capture_slots||scope->capture_count||scope->correlated)
    return parameter_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid scalar inference scope");
  if(context) {
    orm_sql_predicate validator;
    const turbodb_status_t status=orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,*context,NULL,&validator,error);
    if(status!=TURBODB_STATUS_OK) return status;
    if(context->kind==TURBODB_VALUE_NULL)
      return parameter_error(error,TURBODB_STATUS_TYPE_ERROR,"NULL is not a parameter inference context");
  }
  parameter_inference inference={.parameters=parameters,.scope=scope,.error=error,
    .capacity=sqlparser_node_count(scope->document),.max_depth=max_depth};
  const size_t count=vec_size(&parameters->types);
  turbodb_status_t status=orm_sql_work_zero(&inference.facts,inference.capacity,sizeof(parameter_fact),
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
