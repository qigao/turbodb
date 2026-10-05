#include "expr.h"
#include "work.h"
#include "error.h"
#include <stdio.h>
#include <string.h>

typedef enum expr_opcode { EXPR_CONSTANT, EXPR_INPUT, EXPR_PREDICATE, EXPR_BRANCH,
  EXPR_CASE_SKIP, EXPR_MOVE_JUMP, EXPR_COALESCE, EXPR_NULLIF, EXPR_ARITHMETIC, EXPR_LIKE, EXPR_QUERY, EXPR_CAST, EXPR_SESSION } expr_opcode;
typedef enum expr_phase { EXPR_ENTER, EXPR_AFTER_LEFT, EXPR_AFTER_RIGHT,
  EXPR_AFTER_VALUE, EXPR_AFTER_ITEM, EXPR_AFTER_LOWER, EXPR_AFTER_UPPER,
  EXPR_CASE_VALUE, EXPR_CASE_CONDITION, EXPR_CASE_RESULT, EXPR_CASE_ELSE,
  EXPR_CALL_ARGUMENT, EXPR_NULLIF_FIRST, EXPR_NULLIF_SECOND, EXPR_QUERY_PROBE, EXPR_CAST_VALUE } expr_phase;
/* Each list edge adds at most comparison, OR and branch; each AST node supplies
 * at most one other instruction. BETWEEN/negation and CASE's comparison, skip
 * and copy-jump per WHEN fit the same tree bound. ROUND's default-precision
 * constant adds at most one instruction per CALL within that bound. */
enum { EXPR_INSTRUCTIONS_PER_NODE = 4, EXPR_QUOTE_BYTES = 2, EXPR_CTRL_Z = 0x1a };
typedef struct expr_instruction {
  expr_opcode opcode;
  size_t offset, destination, left, right, target, slot;
  bool binary;
  union {
    orm_sql_predicate predicate;
    orm_sql_arithmetic arithmetic;
    orm_sql_cast cast;
    orm_sql_like like;
    turbodb_value_kind_t selection_kind;
    orm_sql_session_variable variable;
    struct { orm_sql_type result_type; orm_sql_predicate comparison;
      orm_sql_subquery_kind kind; sqlparser_id node; } query;
  };
  turbodb_value_t constant;
} expr_instruction;

typedef struct expr_query_run { orm_sql_expr_query_source *source; orm_sql_predicate validator; } expr_query_run;

typedef struct expr_frame {
  sqlparser_id node;
  expr_phase phase;
  size_t destination, left, branch, scratch, remaining, skip;
  sqlparser_id item;
  orm_sql_type left_type, accumulated_type;
  orm_sql_predicate_op op;
  orm_sql_arithmetic_op arithmetic_op;
  bool arithmetic;
  bool like;
  union { int escape; unsigned result_kinds; orm_sql_cast_target cast_target; };
} expr_frame;

enum { EXPR_RESULT_SIGNED = 1, EXPR_RESULT_UNSIGNED = 2, EXPR_RESULT_REAL = 4 };

typedef struct expr_compiler {
  const sqlparser_document *document;
  const orm_sql_expr_input *inputs;
  size_t input_count, used_inputs, offset, last_register;
  const orm_sql_expr_query_binding *queries;
  size_t query_count, used_queries;
  size_t literal_capacity, literal_used;
  orm_sql_type last_type;
  vec_t stack;
  orm_sql_expr program;
  turbodb_error_t *error;
} expr_compiler;

static turbodb_status_t expr_error(turbodb_error_t *error, turbodb_status_t status,
                               size_t offset, const char *reason) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB SQL expression at byte %zu: %s", offset, reason);
  tdsql_error_set(error, status, message);
  return status;
}

static turbodb_status_t storage_status(stl_status status, turbodb_error_t *error) {
  if (status == STL_OK) return TURBODB_STATUS_OK;
  return expr_error(error, status == STL_OUT_OF_MEMORY ? TURBODB_STATUS_OUT_OF_MEMORY :
      status == STL_CAPACITY_EXCEEDED ? TURBODB_STATUS_LIMIT_EXCEEDED : TURBODB_STATUS_INTERNAL_ERROR,
      0, "expression container allocation/capacity failure");
}

static bool function_name_is(const sqlparser_document *document,
    const sqlparser_node *name, const char *expected) {
  const size_t length = strlen(expected);
  const char *text = sqlparser_text(document, name->span);
  if (!text || name->span.length != length) return false;
  for (size_t i = 0; i < length; ++i) {
    const char upper = text[i] >= 'a' && text[i] <= 'z' ? text[i] - ('a' - 'A') : text[i];
    if (upper != expected[i]) return false;
  }
  return true;
}

static bool expr_numeric_function(orm_sql_expr_function function, orm_sql_arithmetic_op *out) {
  switch (function) {
    case ORM_SQL_FUNCTION_ABS: *out = ORM_SQL_ABSOLUTE; break;
    case ORM_SQL_FUNCTION_SIGN: *out = ORM_SQL_SIGN; break;
    case ORM_SQL_FUNCTION_FLOOR: *out = ORM_SQL_FLOOR; break;
    case ORM_SQL_FUNCTION_CEIL: *out = ORM_SQL_CEIL; break;
    case ORM_SQL_FUNCTION_MOD: *out = ORM_SQL_MODULO; break;
    case ORM_SQL_FUNCTION_ROUND: *out = ORM_SQL_ROUND; break;
    case ORM_SQL_FUNCTION_TRUNCATE: *out = ORM_SQL_TRUNCATE; break;
    default: return false;
  }
  return true;
}
static bool expr_numeric_binary(orm_sql_arithmetic_op op) {
  return op == ORM_SQL_MODULO || op == ORM_SQL_ROUND || op == ORM_SQL_TRUNCATE;
}

static turbodb_status_t expr_float_precision(const sqlparser_document *document,
    const sqlparser_node *type, orm_tidesdb_sql_budget *budget,
    orm_sql_cast_target *target, turbodb_error_t *error) {
  const sqlparser_list arguments=type->as.type.arguments;
  if (!arguments.count) { *target=ORM_SQL_CAST_FLOAT; return TURBODB_STATUS_OK; }
  const sqlparser_node *argument=sqlparser_get_node(document,arguments.first);
  if (arguments.count!=1 || !argument || arguments.last!=arguments.first ||
      argument->kind!=SQLPARSER_NUMBER || argument->next)
    return expr_error(error,TURBODB_STATUS_SQL_ERROR,type->span.offset,"FLOAT CAST precision requires one unsigned integer");
  const char *text=sqlparser_text(document,argument->span);
  if (!text || !argument->span.length || argument->span.length==UINT64_MAX)
    return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,argument->span.offset,"invalid FLOAT CAST precision span");
  orm_sql_budget_amount amount={0};
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=(uint64_t)argument->span.length+1u;
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve(budget,&amount,error);
  if (status!=TURBODB_STATUS_OK) return status;
  enum { DECIMAL_RADIX=10 };
  unsigned precision=0;
  for(size_t i=0;i<argument->span.length;++i) {
    if (text[i]<'0' || text[i]>'9')
      return expr_error(error,TURBODB_STATUS_SQL_ERROR,argument->span.offset,"FLOAT CAST precision requires an unsigned integer");
    const unsigned digit=(unsigned)(text[i]-'0');
    if (precision>(ORM_SQL_CAST_DOUBLE_PRECISION-digit)/DECIMAL_RADIX)
      return expr_error(error,TURBODB_STATUS_SQL_ERROR,argument->span.offset,"FLOAT CAST precision exceeds 53");
    precision=precision*DECIMAL_RADIX+digit;
  }
  *target=precision<=ORM_SQL_CAST_FLOAT_PRECISION ? ORM_SQL_CAST_FLOAT : ORM_SQL_CAST_DOUBLE;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_expr_resolve_cast(const sqlparser_document *document,
    const sqlparser_node *cast, orm_tidesdb_sql_budget *budget,
    orm_sql_cast_target *target, turbodb_error_t *error) {
  if (!document || !cast || cast->kind!=SQLPARSER_CAST || !budget || !target)
    return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"invalid CAST expression");
  const sqlparser_node *type=sqlparser_get_node(document,cast->as.cast.type);
  const sqlparser_node *name=type && type->kind==SQLPARSER_TYPE ?
      sqlparser_get_node(document,type->as.type.name) : NULL;
  if (!name || name->kind!=SQLPARSER_NAME || !cast->as.cast.expression)
    return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,cast->span.offset,"invalid CAST target");
  if (type->as.type.is_unsigned || type->as.type.zerofill)
    return expr_error(error,TURBODB_STATUS_UNSUPPORTED,cast->span.offset,"unsupported CAST target modifiers");
  if (function_name_is(document,name,"FLOAT"))
    return expr_float_precision(document,type,budget,target,error);
  if (type->as.type.arguments.count)
    return expr_error(error,TURBODB_STATUS_UNSUPPORTED,cast->span.offset,"unsupported CAST target modifiers");
  orm_sql_cast_target kind;
  if (function_name_is(document,name,"SIGNED")) kind=ORM_SQL_CAST_SIGNED;
  else if (function_name_is(document,name,"UNSIGNED")) kind=ORM_SQL_CAST_UNSIGNED;
  else if (function_name_is(document,name,"DOUBLE") || function_name_is(document,name,"REAL")) kind=ORM_SQL_CAST_DOUBLE;
  else return expr_error(error,TURBODB_STATUS_UNSUPPORTED,cast->span.offset,"unsupported numeric CAST target");
  *target=kind; return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_expr_resolve_call(const sqlparser_document *document,
    const sqlparser_node *call, orm_sql_expr_function *out, turbodb_error_t *error) {
  if (!document || !call || call->kind != SQLPARSER_CALL || !out)
    return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid function call");
  const sqlparser_node *name = sqlparser_get_node(document, call->as.call.name);
  if (!name || name->kind != SQLPARSER_NAME)
    return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, call->span.offset, "invalid function name");
  orm_sql_expr_function function;
  if (function_name_is(document, name, "COALESCE")) function = ORM_SQL_COALESCE;
  else if (function_name_is(document, name, "IFNULL")) function = ORM_SQL_IFNULL;
  else if (function_name_is(document, name, "NULLIF")) function = ORM_SQL_NULLIF;
  else if (function_name_is(document, name, "ABS")) function = ORM_SQL_FUNCTION_ABS;
  else if (function_name_is(document, name, "SIGN")) function = ORM_SQL_FUNCTION_SIGN;
  else if (function_name_is(document, name, "FLOOR")) function = ORM_SQL_FUNCTION_FLOOR;
  else if (function_name_is(document, name, "CEIL") || function_name_is(document, name, "CEILING")) function = ORM_SQL_FUNCTION_CEIL;
  else if (function_name_is(document, name, "MOD")) function = ORM_SQL_FUNCTION_MOD;
  else if (function_name_is(document, name, "ROUND")) function = ORM_SQL_FUNCTION_ROUND;
  else if (function_name_is(document, name, "TRUNCATE")) function = ORM_SQL_FUNCTION_TRUNCATE;
  else return expr_error(error, TURBODB_STATUS_UNSUPPORTED, call->span.offset, "unsupported function");
  if (call->as.call.distinct)
    return expr_error(error, TURBODB_STATUS_UNSUPPORTED, call->span.offset, "function DISTINCT is unsupported");
  enum { BINARY_FUNCTION_ARGUMENTS = 2, UNARY_FUNCTION_ARGUMENTS = 1 };
  orm_sql_arithmetic_op operation;
  const size_t arity = expr_numeric_function(function,&operation) && (!expr_numeric_binary(operation) || function == ORM_SQL_FUNCTION_ROUND) ?
      UNARY_FUNCTION_ARGUMENTS : BINARY_FUNCTION_ARGUMENTS;
  const size_t maximum = function == ORM_SQL_FUNCTION_ROUND ? BINARY_FUNCTION_ARGUMENTS : arity;
  if (!call->as.call.arguments.count || (function != ORM_SQL_COALESCE &&
      (call->as.call.arguments.count < arity || call->as.call.arguments.count > maximum)))
    return expr_error(error, TURBODB_STATUS_SQL_ERROR, call->span.offset, "invalid function argument count");
  *out = function;
  return TURBODB_STATUS_OK;
}

static turbodb_status_t charge(orm_tidesdb_sql_budget *budget,
    orm_sql_budget_resource resource, uint64_t count, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0};
  amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(budget, &amount, error);
}

static bool expr_same_type(orm_sql_type a, orm_sql_type b) {
  return a.kind == b.kind && a.nullable == b.nullable;
}
static bool expr_same_predicate(const orm_sql_predicate *a, const orm_sql_predicate *b) {
  return a->op == b->op && expr_same_type(a->left,b->left) &&
      expr_same_type(a->right,b->right) && expr_same_type(a->result,b->result) &&
      a->real_comparison == b->real_comparison;
}
turbodb_status_t orm_tidesdb_sql_expr_same(const orm_sql_expr *left, const size_t *left_slots,
    const orm_sql_expr *right, const size_t *right_slots, bool *out, turbodb_error_t *error) {
  if (!left || !right || !out || !left->budget || left->budget != right->budget ||
      (left->input_count && !left_slots) || (right->input_count && !right_slots))
    return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid expression comparison");
  bool same = vec_size(&left->code) == vec_size(&right->code) &&
      left->register_count == right->register_count && left->result_register == right->result_register &&
      expr_same_type(left->result, right->result);
  for (size_t i = 0; same && i < vec_size(&left->code); ++i) {
    const expr_instruction *a = vec_at_const(&left->code,i), *b = vec_at_const(&right->code,i);
    turbodb_status_t status = charge(left->budget, ORM_SQL_BUDGET_EXECUTION_STEPS, 1, error);
    if (status != TURBODB_STATUS_OK) return status;
    same = a->opcode == b->opcode && a->destination == b->destination && a->left == b->left &&
        a->right == b->right && a->target == b->target && a->binary == b->binary;
    if (!same) break;
    switch (a->opcode) {
      case EXPR_CONSTANT:
        same = a->constant.kind == b->constant.kind;
        if (!same || a->constant.kind == TURBODB_VALUE_NULL) break;
        if (a->constant.kind == TURBODB_VALUE_TEXT) {
          const vstr av = a->constant.data.text_value, bv = b->constant.data.text_value;
          same = av.len == bv.len;
          if (same) {
            status = charge(left->budget, ORM_SQL_BUDGET_EXECUTION_STEPS, av.len, error);
            if (status != TURBODB_STATUS_OK) return status;
            same = !av.len || !memcmp(av.data,bv.data,av.len);
          }
        } else same = orm_sql_value_order(&a->constant,&b->constant) == 0;
        break;
      case EXPR_INPUT:
        same = left_slots[a->slot] == right_slots[b->slot] && expr_same_predicate(&a->predicate,&b->predicate);
        break;
      case EXPR_SESSION:
        same = a->variable == b->variable; break;
      case EXPR_QUERY:
        same = a->query.node == b->query.node && a->query.kind == b->query.kind &&
            (!a->binary || expr_same_predicate(&a->query.comparison,&b->query.comparison)) &&
            expr_same_type(a->query.result_type,b->query.result_type);
        break;
      case EXPR_BRANCH: case EXPR_PREDICATE:
        same = expr_same_predicate(&a->predicate,&b->predicate); break;
      case EXPR_ARITHMETIC:
        same = a->arithmetic.op == b->arithmetic.op && expr_same_type(a->arithmetic.left,b->arithmetic.left) &&
            expr_same_type(a->arithmetic.right,b->arithmetic.right) && expr_same_type(a->arithmetic.result,b->arithmetic.result);
        break;
      case EXPR_CAST:
        same=a->cast.target==b->cast.target && expr_same_type(a->cast.source,b->cast.source) && expr_same_type(a->cast.result,b->cast.result);
        break;
      case EXPR_LIKE:
        same = a->like.escape == b->like.escape && a->like.negated == b->like.negated &&
            a->like.ascii_insensitive == b->like.ascii_insensitive &&
            expr_same_type(a->like.left,b->like.left) && expr_same_type(a->like.right,b->like.right) &&
            expr_same_type(a->like.result,b->like.result);
        break;
      case EXPR_MOVE_JUMP: case EXPR_COALESCE:
        same = a->selection_kind == b->selection_kind; break;
      case EXPR_CASE_SKIP: case EXPR_NULLIF: break;
      default: return expr_error(error, TURBODB_STATUS_INTERNAL_ERROR, a->offset, "invalid expression opcode");
    }
  }
  *out = same;
  return TURBODB_STATUS_OK;
}

static turbodb_status_t emit(expr_compiler *c, expr_instruction instruction) {
  turbodb_status_t status = charge(c->program.budget, ORM_SQL_BUDGET_PLAN_NODES, 1, c->error);
  /* Raw trivial records need no alias-safe staging allocation from vec_push. */
  const size_t index = vec_size(&c->program.code);
  if (status == TURBODB_STATUS_OK) status = storage_status(vec_resize(&c->program.code, index + 1), c->error);
  if (status == TURBODB_STATUS_OK) *(expr_instruction *)vec_at(&c->program.code, index) = instruction;
  return status;
}

static turbodb_status_t push_frame(expr_compiler *c, sqlparser_id id) {
  const size_t index = vec_size(&c->stack);
  const turbodb_status_t status = storage_status(vec_resize(&c->stack, index + 1), c->error);
  if (status == TURBODB_STATUS_OK) *(expr_frame *)vec_at(&c->stack, index) = (expr_frame){.node = id};
  return status;
}

static turbodb_status_t finish_frame(expr_compiler *c, size_t reg, orm_sql_type type) {
  expr_frame ignored;
  const turbodb_status_t status = storage_status(vec_pop(&c->stack, &ignored), c->error);
  if (status == TURBODB_STATUS_OK) { c->last_register = reg; c->last_type = type; }
  return status;
}

static bool operator_of(sqlparser_operator op, orm_sql_predicate_op *out) {
  switch (op) {
    case SQLPARSER_OP_EQ: *out = ORM_SQL_EQUAL; break;
    case SQLPARSER_OP_NE: *out = ORM_SQL_NOT_EQUAL; break;
    case SQLPARSER_OP_LT: *out = ORM_SQL_LESS; break;
    case SQLPARSER_OP_LE: *out = ORM_SQL_LESS_EQUAL; break;
    case SQLPARSER_OP_GT: *out = ORM_SQL_GREATER; break;
    case SQLPARSER_OP_GE: *out = ORM_SQL_GREATER_EQUAL; break;
    case SQLPARSER_OP_NULL_SAFE_EQ: *out = ORM_SQL_NULL_SAFE_EQUAL; break;
    case SQLPARSER_OP_IS_NULL: *out = ORM_SQL_IS_NULL; break;
    case SQLPARSER_OP_IS_NOT_NULL: *out = ORM_SQL_IS_NOT_NULL; break;
    case SQLPARSER_OP_NOT: *out = ORM_SQL_NOT; break;
    case SQLPARSER_OP_AND: *out = ORM_SQL_AND; break;
    case SQLPARSER_OP_OR: *out = ORM_SQL_OR; break;
    default: return false;
  }
  return true;
}

static bool arithmetic_of(sqlparser_operator op, orm_sql_arithmetic_op *out) {
  switch (op) {
    case SQLPARSER_OP_ADD: *out = ORM_SQL_ADD; break;
    case SQLPARSER_OP_SUBTRACT: *out = ORM_SQL_SUBTRACT; break;
    case SQLPARSER_OP_MULTIPLY: *out = ORM_SQL_MULTIPLY; break;
    case SQLPARSER_OP_DIVIDE: *out = ORM_SQL_DIVIDE; break;
    case SQLPARSER_OP_INTEGER_DIVIDE: *out = ORM_SQL_INTEGER_DIVIDE; break;
    case SQLPARSER_OP_MODULO: *out = ORM_SQL_MODULO; break;
    case SQLPARSER_OP_POSITIVE: *out = ORM_SQL_POSITIVE; break;
    case SQLPARSER_OP_NEGATE: *out = ORM_SQL_NEGATE; break;
    default: return false;
  }
  return true;
}

static size_t find_input(const expr_compiler *c, sqlparser_id node) {
  size_t first = 0, last = c->input_count;
  while (first < last) {
    const size_t mid = first + (last - first) / 2;
    if (c->inputs[mid].node < node) first = mid + 1;
    else last = mid;
  }
  return first;
}

static size_t find_query(const expr_compiler *c, sqlparser_id node) {
  size_t first = 0, last = c->query_count;
  while (first < last) {
    const size_t mid = first + (last-first)/2;
    if (c->queries[mid].node < node) first = mid+1; else last = mid;
  }
  return first;
}
static bool expr_type_accepts(orm_sql_type expected, orm_sql_type actual) {
  return expected.kind == actual.kind && (!actual.nullable || expected.nullable);
}
static turbodb_status_t compile_query(expr_compiler *c, const sqlparser_node *node, expr_frame *frame, size_t slot) {
  const orm_sql_expr_query_binding *query = &c->queries[slot];
  const bool probe = query->type.kind == ORM_SQL_SUBQUERY_IN || query->type.kind == ORM_SQL_SUBQUERY_NOT_IN;
  if (probe && frame->phase == EXPR_ENTER) {
    frame->phase = EXPR_QUERY_PROBE; return push_frame(c,node->as.in.value);
  }
  expr_instruction instruction = {.opcode=EXPR_QUERY,.offset=c->offset,.destination=frame->destination,
      .slot=slot,.left=probe ? c->last_register : 0,.binary=probe,
      .query={.kind=query->type.kind,.node=query->node}};
  turbodb_status_t status = probe ? orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL,c->last_type,
      &query->type.element,&instruction.query.comparison,c->error) : TURBODB_STATUS_OK;
  orm_sql_predicate validator = {0};
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,query->type.result,NULL,&validator,c->error);
  if (status == TURBODB_STATUS_OK) instruction.query.result_type = validator.left;
  if (status == TURBODB_STATUS_OK) status = emit(c,instruction);
  if (status == TURBODB_STATUS_OK) { ++c->used_queries; status = finish_frame(c,frame->destination,query->type.result); }
  return status;
}
static turbodb_status_t compile_text(expr_compiler *c, const sqlparser_node *node, turbodb_value_t *out) {
  const char *raw = sqlparser_text(c->document, node->span);
  if (!raw || node->span.length < EXPR_QUOTE_BYTES || (raw[0] != '\'' && raw[0] != '"') ||
      raw[node->span.length - 1] != raw[0])
    return expr_error(c->error, TURBODB_STATUS_INVALID_ARGUMENT, c->offset, "invalid STRING span");
  turbodb_status_t status = charge(c->program.budget, ORM_SQL_BUDGET_EXECUTION_STEPS, node->span.length, c->error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!c->program.literals.initialized) {
    status = orm_sql_work_allocate(&c->program.literals, c->literal_capacity, sizeof(char), _Alignof(char),
        0, c->program.budget, &c->program.literal_work_bytes, c->error);
    if (status == TURBODB_STATUS_OK) status = storage_status(vec_resize(&c->program.literals, c->literal_capacity), c->error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  const size_t end = node->span.length - 1;
  if (end - 1 > c->literal_capacity - c->literal_used)
    return expr_error(c->error, TURBODB_STATUS_LIMIT_EXCEEDED, c->offset, "literal byte capacity exceeded");
  char *decoded = (char *)vec_data(&c->program.literals) + c->literal_used;
  size_t size = 0;
  const bool backslash = !sqlparser_get_options(c->document).mysql_no_backslash_escapes;
  for (size_t i = 1; i < end; ++i) {
    char ch = raw[i];
    if (ch == raw[0]) {
      if (i + 1 >= end || raw[i + 1] != ch)
        return expr_error(c->error, TURBODB_STATUS_INVALID_ARGUMENT, c->offset, "unpaired STRING delimiter");
      ++i;
    } else if (backslash && ch == '\\') {
      if (++i >= end)
        return expr_error(c->error, TURBODB_STATUS_INVALID_ARGUMENT, c->offset, "incomplete STRING escape");
      ch = raw[i];
      switch (ch) {
        case '0': ch = '\0'; break;
        case 'b': ch = '\b'; break;
        case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break;
        case 't': ch = '\t'; break;
        case 'Z': ch = EXPR_CTRL_Z; break;
        /* These remain escaped outside pattern matching. Unknown escapes drop
         * the slash, as do escaped quotes and backslash itself. */
        case '%': case '_': decoded[size++] = '\\'; break;
        default: break;
      }
    }
    decoded[size++] = ch;
  }
  const turbodb_value_t value = turbodb_text_v((vstr){decoded, size});
  orm_sql_predicate validator; turbodb_value_t ignored;
  status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, (orm_sql_type){TURBODB_VALUE_TEXT, false},
      NULL, &validator, c->error);
  if (status == TURBODB_STATUS_OK)
    status = orm_tidesdb_sql_predicate_eval(&validator, &value, NULL, c->program.budget, &ignored, c->error);
  if (status == TURBODB_STATUS_OK) { c->literal_used += size; *out = value; }
  return status;
}

static turbodb_status_t compile_escape(expr_compiler *c, sqlparser_id id, int *out) {
  const bool no_backslash = sqlparser_get_options(c->document).mysql_no_backslash_escapes;
  *out = no_backslash ? ORM_SQL_LIKE_NO_ESCAPE : '\\';
  if (!id) return TURBODB_STATUS_OK;
  const sqlparser_node *node = sqlparser_get_node(c->document, id);
  if (!node || node->kind != SQLPARSER_STRING)
    return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset, "LIKE ESCAPE requires a string literal");
  c->offset = node->span.offset;
  turbodb_status_t status = charge(c->program.budget, ORM_SQL_BUDGET_AST_NODES, 1, c->error);
  if (status != TURBODB_STATUS_OK) return status;
  turbodb_value_t value = turbodb_null();
  status = compile_text(c, node, &value);
  if (status != TURBODB_STATUS_OK) return status;
  const vstr text = value.data.text_value;
  if (text.len > 1 || (!text.len && no_backslash))
    return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset, "LIKE ESCAPE requires one ASCII byte, or empty without NO_BACKSLASH_ESCAPES");
  if (text.len && (unsigned char)text.data[0] > ORM_SQL_ASCII_MAX)
    return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset, "LIKE ESCAPE requires ASCII");
  *out = text.len ? (unsigned char)text.data[0] : ORM_SQL_LIKE_NO_ESCAPE;
  return TURBODB_STATUS_OK;
}

static turbodb_status_t compile_leaf(expr_compiler *c, const sqlparser_node *node,
                                 expr_frame *frame) {
  expr_instruction instruction = {.opcode = EXPR_CONSTANT, .offset = node->span.offset,
                                  .destination = frame->destination};
  turbodb_status_t status = TURBODB_STATUS_OK;
  orm_sql_type type;
  const size_t slot = find_input(c, frame->node);
  const bool replaced = slot < c->input_count && c->inputs[slot].node == frame->node &&
      c->inputs[slot].replace_expression;
  if (replaced || node->kind == SQLPARSER_NAME || node->kind == SQLPARSER_PARAMETER) {
    if (slot == c->input_count || c->inputs[slot].node != frame->node)
      return expr_error(c->error, TURBODB_STATUS_SQL_ERROR, c->offset, "unresolved expression input");
    instruction.opcode = EXPR_INPUT;
    instruction.slot = slot;
    type = c->inputs[slot].type;
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, type, NULL,
                                            &instruction.predicate, c->error);
    ++c->used_inputs;
  } else if (node->kind == SQLPARSER_VARIABLE) {
    instruction.opcode = EXPR_SESSION;
    status = orm_sql_session_resolve(c->document,node,c->program.budget,&instruction.variable,c->error);
    type = (orm_sql_type){instruction.variable == ORM_SQL_SESSION_TRANSACTION_ISOLATION ? TURBODB_VALUE_TEXT : TURBODB_VALUE_INT64,true};
    if (status == TURBODB_STATUS_OK) c->program.uses_session = true;
  } else {
    if (node->kind == SQLPARSER_NULL) instruction.constant = turbodb_null();
    else if (node->kind == SQLPARSER_BOOLEAN) {
      const char *text = sqlparser_text(c->document, node->span);
      instruction.constant = turbodb_bool(text[0] == 't' || text[0] == 'T');
    } else if (node->kind == SQLPARSER_STRING) {
      status = compile_text(c, node, &instruction.constant);
    } else {
      bool negative = false;
      if (node->kind == SQLPARSER_UNARY) {
        negative = node->as.unary.op == SQLPARSER_OP_NEGATE;
        node = sqlparser_get_node(c->document, node->as.unary.operand);
        if (!node || node->kind != SQLPARSER_NUMBER)
          return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset, "unary sign requires a numeric literal");
        if (vec_size(&c->stack) == c->stack.element_limit)
          return expr_error(c->error, TURBODB_STATUS_LIMIT_EXCEEDED, c->offset, "expression depth exceeded");
        status = charge(c->program.budget, ORM_SQL_BUDGET_AST_NODES, 1, c->error);
      }
      if (status == TURBODB_STATUS_OK)
        status = orm_tidesdb_sql_number_literal(
            (vstr){sqlparser_text(c->document, node->span), node->span.length},
            negative, &instruction.constant, c->error);
    }
    type = (orm_sql_type){instruction.constant.kind, instruction.constant.kind == TURBODB_VALUE_NULL};
  }
  if (status == TURBODB_STATUS_OK) status = emit(c, instruction);
  if (status == TURBODB_STATUS_OK) status = finish_frame(c, frame->destination, type);
  return status;
}

static turbodb_status_t emit_predicate(expr_compiler *c, expr_instruction instruction,
    orm_sql_type left, const orm_sql_type *right, orm_sql_type *result) {
  instruction.opcode = EXPR_PREDICATE;
  instruction.offset = c->offset;
  instruction.binary = right != NULL;
  turbodb_status_t status = orm_tidesdb_sql_predicate_bind(instruction.predicate.op,
      left, right, &instruction.predicate, c->error);
  if (status == TURBODB_STATUS_OK) status = emit(c, instruction);
  if (status == TURBODB_STATUS_OK) *result = instruction.predicate.result;
  return status;
}

/* The unfinished targets form a per-frame chain (index + 1, zero terminates).
 * Patching before NOT ensures every short-circuit path still applies negation. */
static turbodb_status_t emit_compound_branch(expr_compiler *c, expr_frame *frame,
    orm_sql_predicate_op op) {
  const size_t index = vec_size(&c->program.code);
  const turbodb_status_t status = emit(c, (expr_instruction){.opcode = EXPR_BRANCH,
      .offset = c->offset, .destination = frame->destination, .left = frame->destination,
      .target = frame->branch, .predicate = {.op = op}});
  if (status == TURBODB_STATUS_OK) frame->branch = index + 1;
  return status;
}

static void patch_branch_chain(expr_compiler *c, expr_frame *frame) {
  while (frame->branch) {
    expr_instruction *branch = vec_at(&c->program.code, frame->branch - 1);
    frame->branch = branch->target;
    branch->target = vec_size(&c->program.code);
    if (branch->opcode == EXPR_MOVE_JUMP || branch->opcode == EXPR_COALESCE)
      branch->selection_kind = frame->accumulated_type.kind;
  }
}

static turbodb_status_t finish_compound(expr_compiler *c, expr_frame *frame, bool negated) {
  patch_branch_chain(c, frame);
  turbodb_status_t status = TURBODB_STATUS_OK;
  if (negated) status = emit_predicate(c, (expr_instruction){.destination = frame->destination,
      .left = frame->destination, .predicate = {.op = ORM_SQL_NOT}},
      frame->accumulated_type, NULL, &frame->accumulated_type);
  if (status == TURBODB_STATUS_OK)
    status = finish_frame(c, frame->destination, frame->accumulated_type);
  return status;
}

static turbodb_status_t compile_compound(expr_compiler *c, const sqlparser_node *node,
    expr_frame *frame) {
  const bool list = node->kind == SQLPARSER_IN;
  turbodb_status_t status;
  if (frame->phase == EXPR_ENTER) {
    if (list && (node->as.in.query || node->as.in.table || !node->as.in.items.count))
      return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset, "IN requires a nonempty scalar list");
    frame->scratch = c->program.register_count++;
    frame->phase = EXPR_AFTER_VALUE;
    return push_frame(c, list ? node->as.in.value : node->as.between.value);
  }
  if (frame->phase == EXPR_AFTER_VALUE) {
    frame->left = c->last_register;
    frame->left_type = c->last_type;
    if (!list) {
      frame->phase = EXPR_AFTER_LOWER;
      return push_frame(c, node->as.between.lower);
    }
    frame->item = node->as.in.items.first;
    frame->remaining = node->as.in.items.count;
    frame->accumulated_type = (orm_sql_type){TURBODB_VALUE_BOOLEAN, false};
    status = emit(c, (expr_instruction){.opcode = EXPR_CONSTANT, .offset = c->offset,
        .destination = frame->destination, .constant = turbodb_bool(false)});
    if (status != TURBODB_STATUS_OK) return status;
    frame->phase = EXPR_AFTER_ITEM;
    return push_frame(c, frame->item);
  }
  const bool lower = frame->phase == EXPR_AFTER_LOWER;
  bool real_between = false;
  if (!list && !lower && frame->left_type.kind != TURBODB_VALUE_NULL) {
    const expr_instruction *first = vec_at_const(&c->program.code,frame->skip);
    const turbodb_value_kind_t kinds[] = {frame->left_type.kind,
      first->predicate.right.kind,c->last_type.kind};
    for (size_t i=0;i<sizeof(kinds)/sizeof(kinds[0]);++i)
      real_between = real_between || kinds[i] == TURBODB_VALUE_DOUBLE;
    if (real_between)
      for (size_t i=0;i<sizeof(kinds)/sizeof(kinds[0]);++i)
        if (kinds[i] != TURBODB_VALUE_NULL && kinds[i] != TURBODB_VALUE_INT64 &&
            kinds[i] != TURBODB_VALUE_UINT64 && kinds[i] != TURBODB_VALUE_DOUBLE)
          return expr_error(c->error,TURBODB_STATUS_UNSUPPORTED,c->offset,
              "real BETWEEN requires three numeric or NULL operands");
  }
  const size_t comparison_index = vec_size(&c->program.code);
  if (!list && lower) frame->skip = comparison_index;
  orm_sql_type comparison;
  status = emit_predicate(c, (expr_instruction){.destination = lower ? frame->destination : frame->scratch,
      .left = frame->left, .right = c->last_register,
      .predicate = {.op = list ? ORM_SQL_EQUAL : lower ? ORM_SQL_GREATER_EQUAL : ORM_SQL_LESS_EQUAL}},
      frame->left_type, &c->last_type, &comparison);
  if (status != TURBODB_STATUS_OK) return status;
  /* BETWEEN aggregates all three types. Keep indices across emit; code storage
   * may move, and an integer-only bound must use real comparison too. */
  if (real_between) {
    ((expr_instruction *)vec_at(&c->program.code,frame->skip))->predicate.real_comparison = true;
    ((expr_instruction *)vec_at(&c->program.code,comparison_index))->predicate.real_comparison = true;
  }
  if (lower) {
    frame->accumulated_type = comparison;
    status = emit_compound_branch(c, frame, ORM_SQL_AND);
    if (status != TURBODB_STATUS_OK) return status;
    frame->phase = EXPR_AFTER_UPPER;
    return push_frame(c, node->as.between.upper);
  }
  status = emit_predicate(c, (expr_instruction){.destination = frame->destination,
      .left = frame->destination, .right = frame->scratch,
      .predicate = {.op = list ? ORM_SQL_OR : ORM_SQL_AND}},
      frame->accumulated_type, &comparison, &frame->accumulated_type);
  if (status != TURBODB_STATUS_OK) return status;
  if (list) {
    const sqlparser_node *item = sqlparser_get_node(c->document, frame->item);
    if (!item || !frame->remaining || (--frame->remaining == 0) != (item->next == 0))
      return expr_error(c->error, TURBODB_STATUS_INVALID_ARGUMENT, c->offset, "invalid IN list");
    frame->item = item->next;
    if (frame->remaining) {
      status = emit_compound_branch(c, frame, ORM_SQL_OR);
      return status == TURBODB_STATUS_OK ? push_frame(c, frame->item) : status;
    }
  }
  return finish_compound(c, frame, list ? node->as.in.negated : node->as.between.negated);
}

static unsigned result_numeric_kind(turbodb_value_kind_t kind) {
  return kind == TURBODB_VALUE_INT64 ? EXPR_RESULT_SIGNED :
      kind == TURBODB_VALUE_UINT64 ? EXPR_RESULT_UNSIGNED :
      kind == TURBODB_VALUE_DOUBLE ? EXPR_RESULT_REAL : 0;
}

/* Defer the DECIMAL requirement: a later DOUBLE branch makes a preceding
 * I64/U64 mixture representable in the common real result domain. */
static turbodb_status_t merge_result_type(expr_compiler *c, expr_frame *frame) {
  orm_sql_type *type = &frame->accumulated_type;
  if (type->kind != TURBODB_VALUE_NULL && c->last_type.kind != TURBODB_VALUE_NULL &&
      type->kind != c->last_type.kind &&
      (!result_numeric_kind(type->kind) || !result_numeric_kind(c->last_type.kind)))
    return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset, "conditional result conversion is unsupported");
  frame->result_kinds |= result_numeric_kind(c->last_type.kind);
  if (type->kind == TURBODB_VALUE_NULL) type->kind = c->last_type.kind;
  if (frame->result_kinds & EXPR_RESULT_REAL) type->kind = TURBODB_VALUE_DOUBLE;
  type->nullable = type->nullable || c->last_type.nullable;
  return TURBODB_STATUS_OK;
}

static turbodb_status_t finish_selection(expr_compiler *c, expr_frame *frame) {
  if (frame->result_kinds == (EXPR_RESULT_SIGNED | EXPR_RESULT_UNSIGNED))
    return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset,
        "mixed signed/unsigned conditional results require DECIMAL");
  patch_branch_chain(c, frame);
  return finish_frame(c, frame->destination, frame->accumulated_type);
}

static turbodb_status_t emit_case_result(expr_compiler *c, expr_frame *frame) {
  turbodb_status_t status = merge_result_type(c, frame);
  const size_t index = vec_size(&c->program.code);
  if (status == TURBODB_STATUS_OK)
    status = emit(c, (expr_instruction){.opcode = EXPR_MOVE_JUMP, .offset = c->offset,
        .destination = frame->destination, .left = c->last_register, .target = frame->branch});
  if (status == TURBODB_STATUS_OK) frame->branch = index + 1;
  return status;
}

static turbodb_status_t next_case_branch(expr_compiler *c, const sqlparser_node *node,
    expr_frame *frame) {
  if (frame->remaining) {
    const sqlparser_node *branch = sqlparser_get_node(c->document, frame->item);
    if (!branch || branch->kind != SQLPARSER_WHEN)
      return expr_error(c->error, TURBODB_STATUS_INVALID_ARGUMENT, c->offset, "invalid CASE branch");
    /* WHEN is structural: charge it once, but only expressions occupy frames. */
    const turbodb_status_t status = charge(c->program.budget, ORM_SQL_BUDGET_AST_NODES, 1, c->error);
    if (status != TURBODB_STATUS_OK) return status;
    frame->phase = EXPR_CASE_CONDITION;
    return push_frame(c, branch->as.when.condition);
  }
  if (node->as.case_expr.otherwise) {
    frame->phase = EXPR_CASE_ELSE;
    return push_frame(c, node->as.case_expr.otherwise);
  }
  const turbodb_status_t status = emit(c, (expr_instruction){.opcode = EXPR_CONSTANT,
      .offset = c->offset, .destination = frame->destination, .constant = turbodb_null()});
  if (status != TURBODB_STATUS_OK) return status;
  frame->accumulated_type.nullable = true;
  return finish_selection(c, frame);
}

static turbodb_status_t compile_case(expr_compiler *c, const sqlparser_node *node,
    expr_frame *frame) {
  const bool simple = node->as.case_expr.operand != 0;
  if (frame->phase == EXPR_ENTER) {
    if (!node->as.case_expr.branches.count)
      return expr_error(c->error, TURBODB_STATUS_INVALID_ARGUMENT, c->offset, "CASE requires WHEN branches");
    frame->item = node->as.case_expr.branches.first;
    frame->remaining = node->as.case_expr.branches.count;
    if (simple) {
      frame->scratch = c->program.register_count++;
      frame->phase = EXPR_CASE_VALUE;
      return push_frame(c, node->as.case_expr.operand);
    }
    return next_case_branch(c, node, frame);
  }
  if (frame->phase == EXPR_CASE_VALUE) {
    frame->left = c->last_register; frame->left_type = c->last_type;
    return next_case_branch(c, node, frame);
  }
  if (frame->phase == EXPR_CASE_ELSE) {
    const turbodb_status_t status = emit_case_result(c, frame);
    if (status != TURBODB_STATUS_OK) return status;
    return finish_selection(c, frame);
  }
  const sqlparser_node *branch = sqlparser_get_node(c->document, frame->item);
  if (!branch || branch->kind != SQLPARSER_WHEN)
    return expr_error(c->error, TURBODB_STATUS_INVALID_ARGUMENT, c->offset, "invalid CASE branch");
  c->offset = branch->span.offset;
  if (frame->phase == EXPR_CASE_CONDITION) {
    size_t condition = c->last_register;
    if (simple) {
      orm_sql_type comparison;
      const turbodb_status_t status = emit_predicate(c, (expr_instruction){.destination = frame->scratch,
          .left = frame->left, .right = condition, .predicate = {.op = ORM_SQL_EQUAL}},
          frame->left_type, &c->last_type, &comparison);
      if (status != TURBODB_STATUS_OK) return status;
      condition = frame->scratch;
    } else if (c->last_type.kind != TURBODB_VALUE_BOOLEAN && c->last_type.kind != TURBODB_VALUE_NULL) {
      return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset, "CASE condition requires BOOL or NULL");
    }
    frame->skip = vec_size(&c->program.code);
    const turbodb_status_t status = emit(c, (expr_instruction){.opcode = EXPR_CASE_SKIP,
        .offset = c->offset, .left = condition});
    if (status != TURBODB_STATUS_OK) return status;
    frame->phase = EXPR_CASE_RESULT;
    return push_frame(c, branch->as.when.result);
  }
  const turbodb_status_t status = emit_case_result(c, frame);
  if (status != TURBODB_STATUS_OK) return status;
  ((expr_instruction *)vec_at(&c->program.code, frame->skip))->target = vec_size(&c->program.code);
  if (!frame->remaining || (--frame->remaining == 0) != (branch->next == 0))
    return expr_error(c->error, TURBODB_STATUS_INVALID_ARGUMENT, c->offset, "invalid CASE branch list");
  frame->item = branch->next;
  return next_case_branch(c, node, frame);
}

static turbodb_status_t advance_argument(expr_compiler *c, expr_frame *frame) {
  const sqlparser_node *argument = sqlparser_get_node(c->document, frame->item);
  if (!argument || !frame->remaining || (--frame->remaining == 0) != (argument->next == 0))
    return expr_error(c->error, TURBODB_STATUS_INVALID_ARGUMENT, c->offset, "invalid function argument list");
  frame->item = argument->next;
  return TURBODB_STATUS_OK;
}

static turbodb_status_t compile_call(expr_compiler *c, const sqlparser_node *node,
    expr_frame *frame) {
  turbodb_status_t status;
  if (frame->phase == EXPR_ENTER) {
    orm_sql_expr_function function;
    status = orm_tidesdb_sql_expr_resolve_call(c->document, node, &function, c->error);
    if (status == TURBODB_STATUS_OK) status = charge(c->program.budget, ORM_SQL_BUDGET_AST_NODES, 1, c->error);
    if (status != TURBODB_STATUS_OK) return status;
    frame->item = node->as.call.arguments.first;
    frame->remaining = node->as.call.arguments.count;
    frame->accumulated_type = (orm_sql_type){TURBODB_VALUE_NULL, true};
    frame->arithmetic = expr_numeric_function(function,&frame->arithmetic_op);
    frame->phase = function == ORM_SQL_NULLIF ? EXPR_NULLIF_FIRST : EXPR_CALL_ARGUMENT;
    return push_frame(c, frame->item);
  }
  status = advance_argument(c, frame);
  if (status != TURBODB_STATUS_OK) return status;
  if (frame->arithmetic) {
    if (expr_numeric_binary(frame->arithmetic_op) && frame->remaining) {
      frame->left = c->last_register; frame->left_type = c->last_type;
      return push_frame(c,frame->item);
    }
    const bool binary = expr_numeric_binary(frame->arithmetic_op);
    if (frame->arithmetic_op == ORM_SQL_ROUND && node->as.call.arguments.count == 1) {
      frame->left = c->last_register; frame->left_type = c->last_type;
      c->last_register = c->program.register_count++;
      c->last_type = (orm_sql_type){TURBODB_VALUE_INT64,false};
      status = emit(c,(expr_instruction){.opcode=EXPR_CONSTANT,.offset=c->offset,
          .destination=c->last_register,.constant=turbodb_i64(0)});
      if (status != TURBODB_STATUS_OK) return status;
    }
    expr_instruction instruction = {.opcode=EXPR_ARITHMETIC,.offset=c->offset,
        .destination=frame->destination,.left=binary ? frame->left : c->last_register,
        .right=c->last_register,.binary=binary};
    status = orm_tidesdb_sql_arithmetic_bind(frame->arithmetic_op,binary ? frame->left_type : c->last_type,
        binary ? &c->last_type : NULL,&instruction.arithmetic,c->error);
    if (status == TURBODB_STATUS_OK) status = emit(c,instruction);
    if (status == TURBODB_STATUS_OK) status = finish_frame(c,frame->destination,instruction.arithmetic.result);
    return status;
  }
  if (frame->phase == EXPR_NULLIF_FIRST) {
    frame->left = c->last_register; frame->left_type = c->last_type;
    frame->scratch = c->program.register_count++;
    frame->phase = EXPR_NULLIF_SECOND;
    return push_frame(c, frame->item);
  }
  if (frame->phase == EXPR_NULLIF_SECOND) {
    orm_sql_type comparison;
    status = emit_predicate(c, (expr_instruction){.destination = frame->scratch,
        .left = frame->left, .right = c->last_register, .predicate = {.op = ORM_SQL_EQUAL}},
        frame->left_type, &c->last_type, &comparison);
    if (status == TURBODB_STATUS_OK) status = emit(c, (expr_instruction){.opcode = EXPR_NULLIF,
        .offset = c->offset, .destination = frame->destination, .left = frame->left, .right = frame->scratch});
    if (status == TURBODB_STATUS_OK) status = finish_frame(c, frame->destination,
        (orm_sql_type){frame->left_type.kind, true});
    return status;
  }
  const bool nullable = frame->accumulated_type.nullable && c->last_type.nullable;
  status = merge_result_type(c, frame);
  if (status != TURBODB_STATUS_OK) return status;
  frame->accumulated_type.nullable = nullable;
  const size_t index = vec_size(&c->program.code);
  status = emit(c, (expr_instruction){.opcode = EXPR_COALESCE, .offset = c->offset,
      .destination = frame->destination, .left = c->last_register, .target = frame->branch});
  if (status != TURBODB_STATUS_OK) return status;
  frame->branch = index + 1;
  if (frame->remaining) return push_frame(c, frame->item);
  return finish_selection(c, frame);
}

static turbodb_status_t compile_cast(expr_compiler *c, const sqlparser_node *node,
    expr_frame *frame) {
  if (frame->phase==EXPR_ENTER) {
    turbodb_status_t status=orm_tidesdb_sql_expr_resolve_cast(c->document,node,c->program.budget,&frame->cast_target,c->error);
    if (status!=TURBODB_STATUS_OK) return status;
    frame->phase=EXPR_CAST_VALUE;
    return push_frame(c,node->as.cast.expression);
  }
  expr_instruction instruction={.opcode=EXPR_CAST,.offset=c->offset,
      .destination=frame->destination,.left=c->last_register};
  turbodb_status_t status=orm_tidesdb_sql_cast_bind(c->last_type,frame->cast_target,&instruction.cast,c->error);
  if (status==TURBODB_STATUS_OK) status=emit(c,instruction);
  if (status==TURBODB_STATUS_OK) status=finish_frame(c,frame->destination,instruction.cast.result);
  return status;
}

static turbodb_status_t compile_step(expr_compiler *c) {
  expr_frame *frame = vec_at(&c->stack, vec_size(&c->stack) - 1);
  const sqlparser_node *node = sqlparser_get_node(c->document, frame->node);
  if (!node) return expr_error(c->error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid expression node");
  c->offset = node->span.offset;
  const bool binary = node->kind == SQLPARSER_BINARY;
  const size_t query_slot = find_query(c,frame->node);
  const bool query = query_slot < c->query_count && c->queries[query_slot].node == frame->node;
  if (frame->phase == EXPR_ENTER) {
    turbodb_status_t status = charge(c->program.budget, ORM_SQL_BUDGET_AST_NODES, 1, c->error);
    if (status != TURBODB_STATUS_OK) return status;
    frame->destination = c->program.register_count++;
    const size_t slot = find_input(c,frame->node);
    if (slot < c->input_count && c->inputs[slot].node == frame->node && c->inputs[slot].replace_expression)
      return compile_leaf(c,node,frame);
    if (query) return compile_query(c,node,frame,query_slot);
    if (node->kind == SQLPARSER_CAST) return compile_cast(c,node,frame);
    if (node->kind == SQLPARSER_CALL) return compile_call(c, node, frame);
    if (node->kind == SQLPARSER_CASE) return compile_case(c, node, frame);
    if (node->kind == SQLPARSER_IN || node->kind == SQLPARSER_BETWEEN)
      return compile_compound(c, node, frame);
    if (node->kind == SQLPARSER_NAME || node->kind == SQLPARSER_PARAMETER || node->kind == SQLPARSER_VARIABLE ||
        node->kind == SQLPARSER_NULL || node->kind == SQLPARSER_BOOLEAN || node->kind == SQLPARSER_STRING ||
        node->kind == SQLPARSER_NUMBER || (node->kind == SQLPARSER_UNARY &&
        (node->as.unary.op == SQLPARSER_OP_NEGATE || node->as.unary.op == SQLPARSER_OP_POSITIVE) &&
        sqlparser_get_node(c->document, node->as.unary.operand) &&
        sqlparser_get_node(c->document, node->as.unary.operand)->kind == SQLPARSER_NUMBER))
      return compile_leaf(c, node, frame);
    if (!binary && node->kind != SQLPARSER_UNARY)
      return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset, "unsupported predicate AST");
    const sqlparser_operator op = binary ? node->as.binary.op : node->as.unary.op;
    frame->like = binary && (op == SQLPARSER_OP_LIKE || op == SQLPARSER_OP_NOT_LIKE);
    if (binary && node->as.binary.escape && !frame->like)
      return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset, "unsupported predicate escape");
    if (frame->like) {
      status = compile_escape(c, node->as.binary.escape, &frame->escape);
      if (status != TURBODB_STATUS_OK) return status;
    }
    frame->arithmetic = arithmetic_of(op, &frame->arithmetic_op);
    if (!frame->arithmetic && !frame->like && !operator_of(op, &frame->op))
      return expr_error(c->error, TURBODB_STATUS_UNSUPPORTED, c->offset, "unsupported predicate AST");
    frame->phase = EXPR_AFTER_LEFT;
    return push_frame(c, binary ? node->as.binary.left : node->as.unary.operand);
  }
  if (query) return compile_query(c,node,frame,query_slot);
  if (node->kind == SQLPARSER_CAST) return compile_cast(c,node,frame);
  if (node->kind == SQLPARSER_CALL) return compile_call(c, node, frame);
  if (node->kind == SQLPARSER_CASE) return compile_case(c, node, frame);
  if (node->kind == SQLPARSER_IN || node->kind == SQLPARSER_BETWEEN)
    return compile_compound(c, node, frame);
  if (frame->phase == EXPR_AFTER_LEFT && binary) {
    frame->left = c->last_register;
    frame->left_type = c->last_type;
    if (!frame->arithmetic && !frame->like && (frame->op == ORM_SQL_AND || frame->op == ORM_SQL_OR)) {
      frame->branch = vec_size(&c->program.code);
      const expr_instruction branch = {.opcode = EXPR_BRANCH, .offset = c->offset,
          .destination = frame->destination, .left = frame->left, .predicate = {.op = frame->op}};
      const turbodb_status_t status = emit(c, branch);
      if (status != TURBODB_STATUS_OK) return status;
    }
    frame->phase = EXPR_AFTER_RIGHT;
    return push_frame(c, node->as.binary.right);
  }
  expr_instruction instruction = {.opcode = EXPR_PREDICATE, .offset = c->offset,
      .destination = frame->destination, .left = binary ? frame->left : c->last_register,
      .right = c->last_register, .binary = binary};
  if (frame->like) {
    instruction.opcode = EXPR_LIKE;
    turbodb_status_t status = orm_tidesdb_sql_like_bind(frame->left_type, c->last_type, frame->escape,
        node->as.binary.op == SQLPARSER_OP_NOT_LIKE, &instruction.like, c->error);
    if (status == TURBODB_STATUS_OK) status = emit(c, instruction);
    if (status == TURBODB_STATUS_OK) status = finish_frame(c, frame->destination, instruction.like.result);
    return status;
  }
  if (frame->arithmetic) {
    instruction.opcode = EXPR_ARITHMETIC;
    turbodb_status_t status = orm_tidesdb_sql_arithmetic_bind(frame->arithmetic_op,
        binary ? frame->left_type : c->last_type, binary ? &c->last_type : NULL,
        &instruction.arithmetic, c->error);
    if (status == TURBODB_STATUS_OK) status = emit(c, instruction);
    if (status == TURBODB_STATUS_OK) status = finish_frame(c, frame->destination, instruction.arithmetic.result);
    return status;
  }
  turbodb_status_t status = orm_tidesdb_sql_predicate_bind(frame->op,
      binary ? frame->left_type : c->last_type, binary ? &c->last_type : NULL,
      &instruction.predicate, c->error);
  if (status == TURBODB_STATUS_OK) status = emit(c, instruction);
  if (status == TURBODB_STATUS_OK && binary && (frame->op == ORM_SQL_AND || frame->op == ORM_SQL_OR)) {
    expr_instruction *branch = vec_at(&c->program.code, frame->branch);
    branch->target = vec_size(&c->program.code);
  }
  if (status == TURBODB_STATUS_OK)
    status = finish_frame(c, frame->destination, instruction.predicate.result);
  return status;
}

turbodb_status_t orm_tidesdb_sql_expr_destroy(orm_sql_expr *program, turbodb_error_t *error) {
  if (!program || !program->budget) return TURBODB_STATUS_OK;
  if (program->active_runs)
    return expr_error(error, TURBODB_STATUS_BUSY, 0, "expression still has active runs");
  turbodb_status_t status = orm_sql_work_release(&program->code, program->work_bytes, program->budget, error);
  const turbodb_status_t released = orm_sql_work_release(&program->literals, program->literal_work_bytes,
      program->budget, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  *program = (orm_sql_expr){0};
  return status;
}

static bool expr_substitution_kind(sqlparser_kind kind) {
  switch (kind) {
    case SQLPARSER_CALL: case SQLPARSER_WINDOW: case SQLPARSER_UNARY: case SQLPARSER_BINARY:
    case SQLPARSER_CASE: case SQLPARSER_IN: case SQLPARSER_BETWEEN: case SQLPARSER_CAST:
    case SQLPARSER_NULL: case SQLPARSER_NUMBER: case SQLPARSER_BOOLEAN: case SQLPARSER_STRING: case SQLPARSER_VARIABLE:
      return true;
    default: return false;
  }
}
static turbodb_status_t expr_validate_query(const sqlparser_document *document,
    const orm_sql_expr_query_binding *binding, turbodb_error_t *error) {
  const sqlparser_node *node = sqlparser_get_node(document,binding->node);
  const orm_sql_subquery_kind kind = binding->type.kind;
  sqlparser_id query = 0;
  if (node && kind == ORM_SQL_SUBQUERY_SCALAR && node->kind == SQLPARSER_SUBQUERY) query = node->as.subquery.query;
  else if (node && (kind == ORM_SQL_SUBQUERY_EXISTS || kind == ORM_SQL_SUBQUERY_NOT_EXISTS)) {
    if (kind == ORM_SQL_SUBQUERY_NOT_EXISTS && node->kind == SQLPARSER_UNARY && node->as.unary.op == SQLPARSER_OP_NOT)
      node = sqlparser_get_node(document,node->as.unary.operand);
    else if (kind == ORM_SQL_SUBQUERY_NOT_EXISTS) node = NULL;
    if (node && node->kind == SQLPARSER_UNARY && node->as.unary.op == SQLPARSER_OP_EXISTS) query = node->as.unary.operand;
  } else if (node && node->kind == SQLPARSER_IN && !node->as.in.items.count && !node->as.in.table &&
      ((kind == ORM_SQL_SUBQUERY_IN && !node->as.in.negated) || (kind == ORM_SQL_SUBQUERY_NOT_IN && node->as.in.negated)))
    query = node->as.in.query;
  const sqlparser_node *inner = sqlparser_get_node(document,query);
  if (!inner || (inner->kind != SQLPARSER_SELECT && inner->kind != SQLPARSER_UNION && inner->kind != SQLPARSER_QUERY_GROUP && inner->kind != SQLPARSER_WITH))
    return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"query binding does not match its AST operation");
  if (((kind == ORM_SQL_SUBQUERY_SCALAR || kind == ORM_SQL_SUBQUERY_IN || kind == ORM_SQL_SUBQUERY_NOT_IN) && !binding->type.result.nullable) ||
      (kind != ORM_SQL_SUBQUERY_SCALAR && binding->type.result.kind != TURBODB_VALUE_BOOLEAN) ||
      ((kind == ORM_SQL_SUBQUERY_EXISTS || kind == ORM_SQL_SUBQUERY_NOT_EXISTS) && binding->type.result.nullable))
    return expr_error(error,TURBODB_STATUS_TYPE_ERROR,node->span.offset,"invalid subquery result declaration");
  orm_sql_predicate validator;
  turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,binding->type.result,NULL,&validator,error);
  if (status == TURBODB_STATUS_OK && (kind == ORM_SQL_SUBQUERY_IN || kind == ORM_SQL_SUBQUERY_NOT_IN))
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,binding->type.element,NULL,&validator,error);
  return status;
}
static turbodb_status_t expr_compile(const sqlparser_document *document,
    sqlparser_id root, const orm_sql_expr_input *inputs, size_t input_count,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_expr *out,
    turbodb_error_t *error, bool predicate, const orm_sql_expr_query_binding *queries, size_t query_count) {
  if (!document || !root || !budget || !out || out->budget || out->code.initialized || out->literals.initialized ||
      !max_depth || (input_count && !inputs) || (query_count && !queries))
    return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid compile arguments");
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL)
    return expr_error(error, TURBODB_STATUS_UNSUPPORTED, 0, "expression requires MySQL AST");
  const sqlparser_node *root_node = sqlparser_get_node(document, root);
  if (!root_node) return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid expression root");
  const size_t nodes = sqlparser_node_count(document);
  if (!nodes || nodes > SIZE_MAX / EXPR_INSTRUCTIONS_PER_NODE || input_count > nodes || query_count > nodes - input_count)
    return expr_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, 0, "expression size exceeded");
  for (size_t i = 0; i < input_count; ++i) {
    const sqlparser_node *node = sqlparser_get_node(document, inputs[i].node);
    if (!node || (inputs[i].replace_expression ? !expr_substitution_kind(node->kind) :
        (node->kind != SQLPARSER_NAME && node->kind != SQLPARSER_PARAMETER)) ||
        (i && inputs[i - 1].node >= inputs[i].node))
      return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid or unordered input bindings");
  }
  turbodb_status_t checked = charge(budget,ORM_SQL_BUDGET_EXECUTION_STEPS,query_count,error);
  for (size_t i = 0; checked == TURBODB_STATUS_OK && i < query_count; ++i) {
    if (i && queries[i-1].node >= queries[i].node)
      return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"query bindings must be strictly ordered");
    checked = expr_validate_query(document,&queries[i],error);
  }
  if (checked != TURBODB_STATUS_OK) return checked;
  expr_compiler c = {.document = document, .inputs = inputs, .input_count = input_count,
                     .queries=queries,.query_count=query_count,.literal_capacity = root_node->span.length,
                     .program = {.budget = budget}, .error = error};
  size_t stack_bytes = 0;
  const size_t depth = max_depth < nodes ? max_depth : nodes;
  turbodb_status_t status = orm_sql_work_allocate(&c.stack, depth, sizeof(expr_frame), _Alignof(expr_frame),
      sizeof(c), budget, &stack_bytes, error);
  if (status == TURBODB_STATUS_OK)
    status = orm_sql_work_allocate(&c.program.code, nodes * EXPR_INSTRUCTIONS_PER_NODE, sizeof(expr_instruction), _Alignof(expr_instruction),
        sizeof(*out), budget, &c.program.work_bytes, error);
  if (status == TURBODB_STATUS_OK) status = push_frame(&c, root);
  while (status == TURBODB_STATUS_OK && vec_size(&c.stack)) status = compile_step(&c);
  if (status == TURBODB_STATUS_OK && (c.used_inputs != input_count || c.used_queries != query_count))
    status = expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, c.offset, "binding outside expression");
  if (status == TURBODB_STATUS_OK && predicate && c.last_type.kind != TURBODB_VALUE_BOOLEAN && c.last_type.kind != TURBODB_VALUE_NULL)
    status = expr_error(error, TURBODB_STATUS_UNSUPPORTED, c.offset, "predicate root must be BOOL or NULL");
  if (status != TURBODB_STATUS_OK && error)
    status = expr_error(error, status, c.offset, error->message);
  const turbodb_status_t cleanup = orm_sql_work_release(&c.stack, stack_bytes, budget, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = cleanup;
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t destroyed = orm_tidesdb_sql_expr_destroy(&c.program, NULL);
    return destroyed == TURBODB_STATUS_OK ? status : destroyed;
  }
  c.program.result_register = c.last_register;
  c.program.result = c.last_type;
  c.program.input_count = input_count; c.program.query_count = query_count;
  *out = c.program;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_expr_compile(const sqlparser_document *document,
    sqlparser_id root, const orm_sql_expr_input *inputs, size_t input_count,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_expr *out, turbodb_error_t *error) {
  return expr_compile(document, root, inputs, input_count, max_depth, budget, out, error, true, NULL, 0);
}

turbodb_status_t orm_tidesdb_sql_expr_compile_value(const sqlparser_document *document,
    sqlparser_id root, const orm_sql_expr_input *inputs, size_t input_count,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_expr *out, turbodb_error_t *error) {
  return expr_compile(document, root, inputs, input_count, max_depth, budget, out, error, false, NULL, 0);
}

turbodb_status_t orm_tidesdb_sql_expr_compile_queries(const sqlparser_document *document,
    sqlparser_id root, const orm_sql_expr_bindings *bindings, bool predicate,
    size_t max_depth, orm_tidesdb_sql_budget *budget, orm_sql_expr *out, turbodb_error_t *error) {
  if (!bindings) return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"expression bindings required");
  return expr_compile(document,root,bindings->inputs,bindings->input_count,max_depth,budget,out,error,predicate,
      bindings->queries,bindings->query_count);
}
static turbodb_status_t expr_check_queries(const orm_sql_expr *program,
    orm_sql_expr_query_source *const *sources, size_t count, turbodb_error_t *error) {
  if (count != program->query_count || (count && !sources))
    return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"query source count differs from program");
  for (size_t i = 0; i < count; ++i)
    if (!sources[i] || sources[i]->budget != program->budget || !sources[i]->eval)
      return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"query source requires matching budget and evaluator");
  if (!count) return TURBODB_STATUS_OK;
  turbodb_status_t status = charge(program->budget,ORM_SQL_BUDGET_EXECUTION_STEPS,vec_size(&program->code),error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&program->code); ++i) {
    const expr_instruction *instruction = vec_at_const(&program->code,i);
    if (instruction->opcode != EXPR_QUERY) continue;
    const orm_sql_expr_query_type *type = &sources[instruction->slot]->type;
    if (type->kind != instruction->query.kind || !expr_type_accepts(instruction->query.result_type,type->result) ||
        (instruction->binary && !expr_same_type(instruction->query.comparison.right,type->element)))
      return expr_error(error,TURBODB_STATUS_TYPE_ERROR,instruction->offset,"query source schema differs from binding");
    orm_sql_predicate validator;
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,type->result,NULL,&validator,error);
  }
  return status;
}
static turbodb_status_t expr_release_queries(vec_t *queries, size_t bytes,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  for (size_t i = 0; i < vec_size(queries); ++i) {
    orm_sql_expr_query_source *source = ((expr_query_run *)vec_at(queries,i))->source;
    if (source && !source->active_runs)
      return expr_error(error,TURBODB_STATUS_INTERNAL_ERROR,0,"query source lease count underflow");
  }
  for (size_t i = 0; i < vec_size(queries); ++i) {
    orm_sql_expr_query_source *source = ((expr_query_run *)vec_at(queries,i))->source;
    if (source) --source->active_runs;
  }
  return orm_sql_work_release(queries,bytes,budget,error);
}
static turbodb_status_t expr_run_open(orm_sql_expr *program,
    orm_sql_expr_query_source *const *queries, size_t count, orm_sql_evaluation evaluation,
    orm_sql_expr_run *out, turbodb_error_t *error) {
  if (!program || !program->budget || !out || out->program || out->registers.initialized || out->queries.initialized)
    return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid expression run output");
  if (program->active_runs == SIZE_MAX)
    return expr_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, 0, "expression run count exceeded");
  if (evaluation.mode < ORM_SQL_EVALUATION_QUERY || evaluation.mode > ORM_SQL_EVALUATION_IGNORE_WRITE ||
      (evaluation.diagnostics && !evaluation.diagnostics->max_records))
    return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"invalid SQL evaluation context");
  if (program->uses_session && !evaluation.session.valid)
    return expr_error(error,TURBODB_STATUS_UNSUPPORTED,0,"SQL session variable requires connection context");
  orm_sql_expr_run run = {.evaluation=evaluation};
  turbodb_status_t status = expr_check_queries(program,queries,count,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_allocate(&run.registers, program->register_count,
      sizeof(turbodb_value_t), _Alignof(turbodb_value_t), sizeof(run), program->budget, &run.work_bytes, error);
  if (status == TURBODB_STATUS_OK) status = storage_status(vec_resize(&run.registers, program->register_count), error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.queries,count,sizeof(expr_query_run),
      _Alignof(expr_query_run),program->budget,&run.query_bytes,error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
    if (queries[i]->active_runs == SIZE_MAX) status = expr_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,0,"query source lease capacity exceeded");
    else {
      expr_query_run *bound = vec_at(&run.queries,i);
      status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,queries[i]->type.result,NULL,&bound->validator,error);
      if (status == TURBODB_STATUS_OK) { ++queries[i]->active_runs; bound->source = queries[i]; }
    }
  }
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = expr_release_queries(&run.queries,run.query_bytes,program->budget,NULL);
    const turbodb_status_t cleanup = orm_sql_work_release(&run.registers, run.work_bytes, program->budget, NULL);
    if (released != TURBODB_STATUS_OK) return released;
    return cleanup == TURBODB_STATUS_OK ? status : cleanup;
  }
  run.program = program;
  ++program->active_runs;
  *out = run;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_expr_run_open_queries(orm_sql_expr *program,
    orm_sql_expr_query_source *const *queries, size_t count, orm_sql_expr_run *out, turbodb_error_t *error) {
  return expr_run_open(program,queries,count,(orm_sql_evaluation){0},out,error);
}
turbodb_status_t orm_tidesdb_sql_expr_run_open_evaluation(orm_sql_expr *program,
    orm_sql_evaluation evaluation, orm_sql_expr_run *out, turbodb_error_t *error) {
  return expr_run_open(program,NULL,0,evaluation,out,error);
}
turbodb_status_t orm_tidesdb_sql_expr_run_open(orm_sql_expr *program,
    orm_sql_expr_run *out, turbodb_error_t *error) {
  return orm_tidesdb_sql_expr_run_open_evaluation(program,(orm_sql_evaluation){0},out,error);
}

turbodb_status_t orm_tidesdb_sql_expr_run_open_mapped(orm_sql_expr *program,
    const size_t *slots, size_t count, const orm_sql_expr_query_sources *sources,
    orm_sql_expr_run *out, turbodb_error_t *error) {
  const size_t total = sources ? sources->count : 0;
  if (!program || !program->budget || count != program->query_count || (count && !slots) ||
      (total && !sources->items))
    return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"invalid query source mapping");
  if (total > program->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return expr_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,0,"query source registry exceeds bounds");
  turbodb_status_t status = charge(program->budget,ORM_SQL_BUDGET_EXECUTION_STEPS,count,error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < count; ++i)
    if (slots[i] >= total)
      return expr_error(error,TURBODB_STATUS_INVALID_ARGUMENT,0,"query mapping outside source registry");
  const orm_sql_evaluation evaluation = sources ? sources->evaluation : (orm_sql_evaluation){0};
  if (!count) return orm_tidesdb_sql_expr_run_open_evaluation(program,evaluation,out,error);
  vec_t queries = {0}; size_t bytes = 0;
  status = orm_sql_work_zero(&queries,count,sizeof(orm_sql_expr_query_source *),
      _Alignof(orm_sql_expr_query_source *),program->budget,&bytes,error);
  if (status == TURBODB_STATUS_OK) {
    for (size_t i = 0; i < count; ++i)
      *(orm_sql_expr_query_source **)vec_at(&queries,i) = sources->items[slots[i]];
    status = expr_run_open(program,vec_data_const(&queries),count,evaluation,out,error);
  }
  const turbodb_status_t released = orm_sql_work_release(&queries,bytes,program->budget,status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? released : status;
}

turbodb_status_t orm_tidesdb_sql_expr_run_close(orm_sql_expr_run *run, turbodb_error_t *error) {
  if (!run || !run->program) return TURBODB_STATUS_OK;
  if (run->evaluating) return expr_error(error,TURBODB_STATUS_BUSY,0,"expression is evaluating");
  turbodb_status_t status = expr_release_queries(&run->queries,run->query_bytes,run->program->budget,error);
  const turbodb_status_t released = orm_sql_work_release(&run->registers, run->work_bytes, run->program->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  --run->program->active_runs;
  *run = (orm_sql_expr_run){0};
  return status;
}

turbodb_status_t orm_tidesdb_sql_expr_check_inputs(const orm_sql_expr *program,
    const orm_sql_expr_input_layout *layout, const size_t *slots,
    size_t count, turbodb_error_t *error) {
  if (!program || !program->budget || !layout || count != program->input_count ||
      (count && !slots) || (layout->column_count && !layout->columns) ||
      (layout->parameter_count && !layout->parameters) ||
      layout->parameter_count > SIZE_MAX - layout->column_count)
    return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid expression input schema");
  for (size_t i = 0; i < count; ++i)
    if (slots[i] >= layout->column_count + layout->parameter_count)
      return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "input slot outside schema");
  for (size_t i = 0; i < vec_size(&program->code); ++i) {
    const expr_instruction *instruction = vec_at_const(&program->code, i);
    if (instruction->opcode != EXPR_INPUT) continue;
    const size_t slot = slots[instruction->slot];
    const orm_sql_type actual = slot < layout->column_count ? layout->columns[slot] :
        layout->parameters[slot - layout->column_count], expected = instruction->predicate.left;
    if (actual.kind != expected.kind || (actual.nullable && !expected.nullable) ||
        (actual.kind == TURBODB_VALUE_NULL && !actual.nullable))
      return expr_error(error, TURBODB_STATUS_TYPE_ERROR, instruction->offset, "input schema does not match binding");
  }
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_expr_run_eval(orm_sql_expr_run *run,
    const turbodb_value_t *inputs, size_t input_count, turbodb_value_t *out, turbodb_error_t *error) {
  return orm_tidesdb_sql_expr_run_eval_row(run,inputs,input_count,NULL,0,out,error);
}

turbodb_status_t orm_tidesdb_sql_expr_run_eval_row(orm_sql_expr_run *run,
    const turbodb_value_t *inputs, size_t input_count, const turbodb_value_t *outer_row,
    size_t outer_count, turbodb_value_t *out, turbodb_error_t *error) {
  const orm_sql_expr *program = run ? run->program : NULL;
  if (!program || !program->budget || !out || input_count != program->input_count ||
      (input_count && !inputs) || (outer_count && !outer_row))
    return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid expression execution arguments");
  if (run->evaluating) return expr_error(error,TURBODB_STATUS_BUSY,0,"expression evaluation is not reentrant");
  run->evaluating = true;
  vec_t *registers = &run->registers;
  size_t offset = 0;
  turbodb_status_t status = TURBODB_STATUS_OK;
  for (size_t pc = 0; status == TURBODB_STATUS_OK && pc < vec_size(&program->code); ) {
    const expr_instruction *instruction = vec_at_const(&program->code, pc);
    offset = instruction->offset;
    turbodb_value_t *result = vec_at(registers, instruction->destination);
    const turbodb_value_t *left = vec_at_const(registers, instruction->left);
    ++pc;
    if (instruction->opcode == EXPR_CONSTANT || instruction->opcode == EXPR_BRANCH ||
        instruction->opcode == EXPR_CASE_SKIP || instruction->opcode == EXPR_MOVE_JUMP ||
        instruction->opcode == EXPR_COALESCE || instruction->opcode == EXPR_NULLIF) {
      status = charge(program->budget, ORM_SQL_BUDGET_EXECUTION_STEPS, 1, error);
      if (status != TURBODB_STATUS_OK) break;
      if (instruction->opcode == EXPR_CONSTANT) *result = instruction->constant;
      else if (instruction->opcode == EXPR_COALESCE) {
        if (instruction->selection_kind == TURBODB_VALUE_DOUBLE &&
            (left->kind == TURBODB_VALUE_INT64 || left->kind == TURBODB_VALUE_UINT64))
          status = orm_tidesdb_sql_real_promote(left,program->budget,result,error);
        else *result = *left;
        if (status == TURBODB_STATUS_OK && left->kind != TURBODB_VALUE_NULL) pc = instruction->target;
      } else if (instruction->opcode == EXPR_NULLIF) {
        const turbodb_value_t *equal = vec_at_const(registers, instruction->right);
        *result = equal->kind == TURBODB_VALUE_BOOLEAN && equal->data.boolean_value ? turbodb_null() : *left;
      }
      else if (instruction->opcode == EXPR_MOVE_JUMP) {
        if (instruction->selection_kind == TURBODB_VALUE_DOUBLE &&
            (left->kind == TURBODB_VALUE_INT64 || left->kind == TURBODB_VALUE_UINT64))
          status = orm_tidesdb_sql_real_promote(left,program->budget,result,error);
        else *result = *left;
        if (status == TURBODB_STATUS_OK) pc = instruction->target;
      } else if (instruction->opcode == EXPR_CASE_SKIP) {
        if (left->kind != TURBODB_VALUE_BOOLEAN || !left->data.boolean_value) pc = instruction->target;
      }
      else if (left->kind == TURBODB_VALUE_BOOLEAN &&
          (left->data.boolean_value != 0) == (instruction->predicate.op == ORM_SQL_OR)) {
        *result = *left;
        pc = instruction->target;
      }
    } else if (instruction->opcode == EXPR_SESSION) {
      status = charge(program->budget,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
      if (status == TURBODB_STATUS_OK) status = orm_sql_session_value(run->evaluation.session,
          instruction->variable,false,result,error);
    } else if (instruction->opcode == EXPR_QUERY) {
      const expr_query_run *bound = vec_at_const(&run->queries,instruction->slot);
      orm_sql_expr_query_source *query = bound->source;
      turbodb_value_t value = turbodb_null(), ignored;
      status = charge(program->budget,ORM_SQL_BUDGET_EXECUTION_STEPS,1,error);
      if (status == TURBODB_STATUS_OK) status = query->eval(query->context,instruction->binary ? left : NULL,
          instruction->binary ? &instruction->query.comparison : NULL,outer_row,outer_count,&value,error);
      if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_predicate_eval(&bound->validator,&value,NULL,program->budget,&ignored,error);
      if (status == TURBODB_STATUS_OK) *result = value;
    } else if (instruction->opcode == EXPR_INPUT) {
      turbodb_value_t ignored;
      status = orm_tidesdb_sql_predicate_eval(&instruction->predicate, &inputs[instruction->slot],
          NULL, program->budget, &ignored, error);
      if (status == TURBODB_STATUS_OK) *result = inputs[instruction->slot];
    } else {
      const turbodb_value_t *right = instruction->binary ? vec_at_const(registers, instruction->right) : NULL;
      if (instruction->opcode == EXPR_LIKE)
        status = orm_tidesdb_sql_like_eval(&instruction->like, left, right, program->budget, result, error);
      else if (instruction->opcode == EXPR_CAST) {
        turbodb_value_t converted=turbodb_null(); orm_sql_cast_condition condition;
        status=orm_tidesdb_sql_cast_eval(&instruction->cast,left,program->budget,&converted,&condition,error);
        if (status==TURBODB_STATUS_OK) status=orm_sql_evaluation_cast(run->evaluation,(unsigned)condition,
            instruction->cast.result.kind==TURBODB_VALUE_UINT64,error);
        if (status==TURBODB_STATUS_OK) *result=converted;
      } else if (instruction->opcode == EXPR_ARITHMETIC) {
        orm_sql_numeric_condition condition;
        status = orm_tidesdb_sql_arithmetic_eval_condition(&instruction->arithmetic,left,right,program->budget,result,&condition,error);
        if (status == TURBODB_STATUS_OK && condition == ORM_SQL_NUMERIC_DIVISION_BY_ZERO)
          status = orm_sql_evaluation_division_by_zero(run->evaluation,error);
      } else status = orm_tidesdb_sql_predicate_eval(&instruction->predicate,left,right,program->budget,result,error);
    }
  }
  turbodb_value_t result = turbodb_null();
  if (status == TURBODB_STATUS_OK) result = *(const turbodb_value_t *)vec_at_const(registers, program->result_register);
  else if (error) status = expr_error(error, status, offset, error->message);
  memset(vec_data(registers), 0, program->register_count * sizeof(turbodb_value_t));
  run->evaluating = false;
  if (status == TURBODB_STATUS_OK) *out = result;
  return status;
}

turbodb_status_t orm_tidesdb_sql_expr_eval_evaluation(orm_sql_expr *program,
    const turbodb_value_t *inputs, size_t input_count, orm_sql_evaluation evaluation,
    turbodb_value_t *out, turbodb_error_t *error) {
  if (!program || !out || input_count != program->input_count || (input_count && !inputs))
    return expr_error(error, TURBODB_STATUS_INVALID_ARGUMENT, 0, "invalid expression execution arguments");
  orm_sql_expr_run run = {0};
  turbodb_value_t result = turbodb_null();
  turbodb_status_t status = orm_tidesdb_sql_expr_run_open_evaluation(program,evaluation,&run,error);
  if (status == TURBODB_STATUS_OK)
    status = orm_tidesdb_sql_expr_run_eval(&run, inputs, input_count, &result, error);
  const turbodb_status_t cleanup = orm_tidesdb_sql_expr_run_close(&run, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = cleanup;
  if (status == TURBODB_STATUS_OK) *out = result;
  return status;
}
turbodb_status_t orm_tidesdb_sql_expr_eval(orm_sql_expr *program,
    const turbodb_value_t *inputs, size_t input_count, turbodb_value_t *out, turbodb_error_t *error) {
  return orm_tidesdb_sql_expr_eval_evaluation(program,inputs,input_count,(orm_sql_evaluation){0},out,error);
}
