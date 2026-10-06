%ifdef SQLITE
%name SqliteParser
%else
%name SqlParser
%endif
%token_prefix SQLTK_
// Both generated engines share this token ABI with the re2c lexer.
%token OR AND NOT BETWEEN IS IN LIKE EQ NE LT LE GT GE NULL_SAFE_EQ
 BITOR BITAND LSHIFT RSHIFT PLUS MINUS STAR SLASH MOD NEGATE BITNOT ESCAPE
 SEMI ID QID DOT COMMA ALL DISTINCT UNION SELECT AS FROM NATURAL JOIN LP RP
 INNER CROSS LEFT RIGHT OUTER ON USING WHERE HAVING GROUP BY ORDER ASC DESC
 LIMIT OFFSET NUMBER STRING NULL BOOLEAN PARAMETER VARIABLE EXISTS CASE END
 WHEN THEN ELSE INSERT REPLACE INTO VALUES SET UPDATE DELETE DUPLICATE WORK BEGIN START
 TRANSACTION COMMIT ROLLBACK SESSION GLOBAL LOCAL NAMES CHARACTER FULL SHOW
 DATABASES TABLES TABLE STATUS VARIABLES COLLATION TEMPORARY IF CREATE DROP
 UNSIGNED PRIMARY KEY UNIQUE AUTO_INCREMENT DEFAULT REFERENCES CHECK CONSTRAINT
 INDEX FOREIGN ENGINE COLLATE CHARSET CONCAT CAST BLOB PRAGMA DEFERRED IMMEDIATE
 EXCLUSIVE SAVEPOINT RELEASE TO VIEW ATTACH DETACH DATABASE INTERSECT EXCEPT
 WITH RECURSIVE GLOB REGEXP MATCH ISNULL NOTNULL AUTOINCREMENT CONFLICT ABORT
 FAIL IGNORE WITHOUT ROWID STRICT VIRTUAL ANY TRIGGER BEFORE AFTER INSTEAD OF
 FOR EACH ROW RAISE EXPLAIN QUERY PLAN ANALYZE REINDEX VACUUM ALTER RENAME COLUMN
 ADD CASCADE RESTRICT NO ACTION PARTIAL SIMPLE DEFERRABLE INITIALLY INDEXED MYSQL_FUNCTION MYSQL_CALC_FOUND_ROWS
 SCALAR_QUERY PAREN_QUERY MYSQL_EMPTY_JOIN TRIGGERS EVENTS OPEN COLUMNS FIELDS KEYS INDEXES
 PROCEDURE FUNCTION EXTENDED CHAIN READ WRITE ONLY CONSISTENT SNAPSHOT
 ISOLATION LEVEL REPEATABLE COMMITTED UNCOMMITTED SERIALIZABLE TRUNCATE ZEROFILL CURRENT_TIMESTAMP
 PREPARE EXECUTE DEALLOCATE XOR ASSIGN LOW_PRIORITY LOCK UNLOCK WARNINGS COUNT
 OPTION CASCADED FORMAT TRADITIONAL JSON TREE OVER PARTITION FIRST LATERAL
 WINDOW ROWS RANGE CURRENT UNBOUNDED PRECEDING FOLLOWING INTERVAL DIV MYSQL_MOD.
%token_type {sqlparser_span}
%default_type {sqlparser_id}
%extra_context {sqlp_context *ctx}
%stack_size 0
%stack_size_limit sqlp_stack_limit
%realloc sqlp_stack_realloc
%free sqlp_stack_free
%token_destructor { (void)ctx; (void)$$; }
%default_destructor { (void)ctx; (void)$$; }
%include {
#include "internal.h"
#include <stdlib.h>
#define sqlp_stack_limit(C) ((int)(C)->limits.max_stack_entries)
#define YYNOERRORRECOVERY 1
#define NODE(K,...) sqlp_add(ctx, (sqlparser_node){.kind=SQLPARSER_##K, __VA_ARGS__})
#define AT(K,T) sqlp_atom(ctx, SQLPARSER_##K,T)
#define EMPTY ((sqlparser_list){0})
#define LIST(L,N) sqlp_append(ctx,L,N)
#define SP(N) sqlp_span(ctx,N)
#define COVER(A,B) sqlp_cover(A,B)
#define FINISH(N,T) sqlp_finish(ctx,N,T)
static inline bool sqlp_cast_word(sqlp_context *ctx, sqlparser_span span,
    const char *word, size_t length) {
  const char *text=sqlparser_text(ctx->document,span);
  if (!text || span.length!=length) return false;
  for (size_t i=0;i<length;++i) {
    const char upper=text[i]>='a' && text[i]<='z' ? text[i]-('a'-'A') : text[i];
    if (upper!=word[i]) return false;
  }
  return true;
}
}
%syntax_error {
  sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR,"unexpected SQL token or end of input");
}
%parse_failure { sqlp_error(ctx, SQLPARSER_SYNTAX_ERROR, "incomplete SQL statement"); }
%stack_overflow { sqlp_error(ctx, SQLPARSER_LIMIT_EXCEEDED, "SQL parser stack limit exceeded"); }
%parse_accept { ctx->accepted = true; }

%ifdef SQLITE
// SQLite's contextual keywords are identifiers where their keyword production
// cannot apply. This is a grammar rule, never a retry with another dialect.
%fallback ID ABORT ASC ATTACH BEGIN BY CAST CONFLICT DEFERRED DESC DETACH END
 EXCLUSIVE FAIL IGNORE IMMEDIATE KEY LIKE GLOB REGEXP MATCH OFFSET PRAGMA RECURSIVE RELEASE
 REPLACE ROLLBACK SAVEPOINT TEMPORARY VIEW WITH WITHOUT IF STRICT ROWID VIRTUAL
 TRIGGER BEFORE AFTER INSTEAD OF FOR EACH ROW EXPLAIN QUERY PLAN ANALYZE REINDEX
 VACUUM RENAME COLUMN CASCADE RESTRICT NO ACTION INITIALLY RAISE PARTIAL SIMPLE.
%wildcard ANY.
%else
// MySQL SAVEPOINT and VIEW are nonreserved in identifier positions.
%fallback ID SAVEPOINT VIEW TRIGGERS EVENTS OPEN COLUMNS FIELDS INDEXES
 FUNCTION EXTENDED CHAIN NO ONLY CONSISTENT SNAPSHOT ISOLATION LEVEL
 REPEATABLE COMMITTED UNCOMMITTED SERIALIZABLE TRUNCATE PREPARE EXECUTE DEALLOCATE
 FORMAT TRADITIONAL JSON TREE FIRST AFTER CASCADE RESTRICT MATCH ACTION FULL PARTIAL SIMPLE DUPLICATE WARNINGS.
%endif

%ifndef SQLITE
%right ASSIGN.
// Prefer UNIQUE KEY as one column attribute over UNIQUE followed by bare KEY.
%right UNIQUE KEY.
// MySQL resolves SELECT ... JOIN t ON as a join condition. Consequently an
// INSERT SELECT with ON DUPLICATE after an unconditioned JOIN needs a WHERE
// clause, matching the ambiguity documented by MySQL.
%nonassoc MYSQL_EMPTY_JOIN.
%nonassoc ON.
%endif
%left OR.
%ifndef SQLITE
%left XOR.
%endif
%left AND.
%right NOT.
%ifdef SQLITE
%left IS LIKE BETWEEN IN ISNULL NOTNULL NE EQ GLOB REGEXP MATCH.
%left LT LE GT GE.
%right ESCAPE.
%left BITAND BITOR LSHIFT RSHIFT.
%left PLUS MINUS.
%left STAR SLASH MOD.
%left CONCAT.
%left COLLATE.
%right NEGATE BITNOT.
%else
%left BETWEEN.
%left IS IN LIKE EQ NE LT LE GT GE NULL_SAFE_EQ.
%left BITOR.
%left BITAND.
%left LSHIFT RSHIFT.
%left PLUS MINUS.
%left STAR SLASH MOD DIV MYSQL_MOD.
%right NEGATE BITNOT.
%right ESCAPE.
%left COLLATE.
%endif

// On a closing parenthesis, keep nested queries as queries (not a scalar IN
// list). Other expression lookaheads still reduce a scalar subquery normally.
%left SCALAR_QUERY.
%left PAREN_QUERY.

input ::= statements.
statements ::= .
statements ::= statements SEMI.
statements ::= statements statement(A) SEMI. { sqlp_publish(ctx,A); }
statement(A) ::= command(B). { A=B; }
command(A) ::= dml(B). { A=B; }
command(A) ::= query(B). { A=B; }
%ifdef SQLITE
statement(A) ::= EXPLAIN(T) command(B). { A=FINISH(NODE(EXPLAIN,.as.explain={B,false}),T); }
statement(A) ::= EXPLAIN(T) QUERY PLAN command(B). { A=FINISH(NODE(EXPLAIN,.as.explain={B,true}),T); }
%else
%type mysql_explain_keyword {sqlparser_span}
mysql_explain_keyword(A) ::= EXPLAIN(B). { A=B; }
mysql_explain_keyword(A) ::= DESC(B). { A=B; }
statement(A) ::= mysql_explain_keyword(T) explain_format(F) mysql_explainable(B). {
  A=FINISH(NODE(EXPLAIN,.as.explain={B,false,F}),T);
}
%type explain_format {sqlparser_explain_format}
explain_format(A) ::= . { A=SQLPARSER_EXPLAIN_DEFAULT; }
explain_format(A) ::= FORMAT EQ TRADITIONAL. { A=SQLPARSER_EXPLAIN_TRADITIONAL; }
explain_format(A) ::= FORMAT EQ JSON. { A=SQLPARSER_EXPLAIN_JSON; }
explain_format(A) ::= FORMAT EQ TREE. { A=SQLPARSER_EXPLAIN_TREE; }
mysql_explainable(A) ::= query(B). { A=B; }
mysql_explainable(A) ::= dml(B). { A=B; }
mysql_explainable(A) ::= mysql_with_write(B). { A=B; }
%endif

%type ident {sqlparser_span}
ident(A) ::= ID(B). { A=B; }
ident(A) ::= QID(B). { A=B; }
ident(A) ::= STATUS(B). { A=B; }
ident(A) ::= VARIABLES(B). { A=B; }
ident(A) ::= ENGINE(B). { A=B; }
ident(A) ::= CHARSET(B). { A=B; }
name(A) ::= ident(B). { A=AT(NAME,B); }
name(A) ::= name(B) DOT ident(C). { A=sqlp_qualified_name(ctx,B,C); }
object_name(A) ::= name(B). { A=B; }
%ifdef SQLITE
// INDEXED may name an object, but cannot consume the start of an index hint as
// an implicit alias. SQLite likewise distinguishes this token from plain IDs.
name(A) ::= INDEXED(B). { A=AT(NAME,B); }
name(A) ::= join_word(B). { A=AT(NAME,B); }
%type join_word {sqlparser_span}
join_word(A) ::= CROSS(B). { A=B; }
join_word(A) ::= INNER(B). { A=B; }
join_word(A) ::= LEFT(B). { A=B; }
join_word(A) ::= RIGHT(B). { A=B; }
join_word(A) ::= FULL(B). { A=B; }
join_word(A) ::= OUTER(B). { A=B; }
join_word(A) ::= NATURAL(B). { A=B; }
name(A) ::= STRING(B) DOT ident(C). { A=sqlp_qualified_name(ctx,AT(NAME,B),C); }
name(A) ::= STRING(B) DOT STRING(C). { A=sqlp_qualified_name(ctx,AT(NAME,B),C); }
projection(A) ::= STRING(B) DOT STAR(C). {
  sqlparser_span span=COVER(B,C);
  A=NODE(PROJECTION,.span=span,.as.projection={AT(STAR,span),0});
}
alias(A) ::= AS INDEXED(B). { A=AT(NAME,B); }
object_name(A) ::= STRING(B). { A=AT(NAME,B); }
name(A) ::= name(B) DOT STRING(C). { A=sqlp_qualified_name(ctx,B,C); }
%endif
%type names {sqlparser_list}
names(A) ::= object_name(B). { A=LIST(EMPTY,B); }
names(A) ::= names(B) COMMA object_name(C). { A=LIST(B,C); }

%type distinct {bool}
distinct(A) ::= . { A=false; }
distinct(A) ::= ALL. { A=false; }
distinct(A) ::= DISTINCT. { A=true; }
query(A) ::= compound(B) order_by(O) limit(L). { A=sqlp_query_tail(ctx,B,O,L); }
%type recursive_opt {bool}
recursive_opt(A) ::= . { A=false; }
recursive_opt(A) ::= RECURSIVE. { A=true; }
%type ctes {sqlparser_list}
ctes(A) ::= cte(B). { A=LIST(EMPTY,B); }
ctes(A) ::= ctes(B) COMMA cte(C). { A=LIST(B,C); }
cte(A) ::= ident(N) cte_columns(C) AS LP query(Q) RP. { A=FINISH(NODE(CTE,.as.cte={AT(NAME,N),Q,C}),N); }
%type cte_columns {sqlparser_list}
cte_columns(A) ::= . { A=EMPTY; }
cte_columns(A) ::= LP names(B) RP. { A=B; }
query(A) ::= WITH(T) recursive_opt(R) ctes(C) compound(B) order_by(O) limit(L). {
  A=FINISH(NODE(WITH,.as.with={C,sqlp_query_tail(ctx,B,O,L),R}),T);
}

%ifdef SQLITE
compound(A) ::= select(B). { A=B; }
compound(A) ::= compound(B) UNION union_all(U) select(C). {
  A=NODE(UNION,.span=COVER(SP(B),SP(C)),.as.compound={B,C,U});
}
%else
compound(A) ::= intersect(B). { A=B; }
intersect(A) ::= select(B). { A=B; }
intersect(A) ::= intersect(B) INTERSECT union_all(U) select(C). {
  A=NODE(UNION,.span=COVER(SP(B),SP(C)),.as.compound={B,C,U,{0},0,SQLPARSER_COMPOUND_INTERSECT});
}
compound(A) ::= compound(B) UNION union_all(U) intersect(C). {
  A=NODE(UNION,.span=COVER(SP(B),SP(C)),.as.compound={B,C,U});
}
compound(A) ::= compound(B) EXCEPT union_all(U) intersect(C). {
  A=NODE(UNION,.span=COVER(SP(B),SP(C)),.as.compound={B,C,U,{0},0,SQLPARSER_COMPOUND_EXCEPT});
}
%endif
%ifndef SQLITE
select(A) ::= LP(T) query(B) RP(E). [PAREN_QUERY] {
  A=NODE(QUERY_GROUP,.span=COVER(T,E),.as.query_group={B,{0},0});
}
%endif
%type union_all {bool}
union_all(A) ::= . { A=false; }
%ifndef SQLITE
union_all(A) ::= DISTINCT. { A=false; }
%endif
union_all(A) ::= ALL. { A=true; }
%ifdef SQLITE
select(A) ::= SELECT(T) distinct(D) projections(P) from(F) where(W)
              group_by(G) having(H). {
  A=FINISH(NODE(SELECT,.as.select={P,G,{0},F,W,H,0,D}),T);
}
%else
%type select_options {sqlp_select_options}
select_options(A) ::= . { A=(sqlp_select_options){0}; }
select_options(A) ::= select_options(B) DISTINCT. {
  if (B.all) sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"SELECT cannot combine ALL and DISTINCT");
  A=B; A.distinct=true;
}
select_options(A) ::= select_options(B) ALL. {
  if (B.distinct) sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"SELECT cannot combine ALL and DISTINCT");
  A=B; A.all=true;
}
select_options(A) ::= select_options(B) MYSQL_CALC_FOUND_ROWS. { A=B; A.calc_found_rows=true; }
select(A) ::= SELECT(T) select_options(D) projections(P) from(F) where(W) group_by(G) having(H) window_definitions(N). {
  A=FINISH(NODE(SELECT,.as.select={P,G,{0},F,W,H,0,D.distinct,D.calc_found_rows,N}),T);
}
%endif
%type projections {sqlparser_list}
projections(A) ::= projection(B). { A=LIST(EMPTY,B); }
projections(A) ::= projections(B) COMMA projection(C). { A=LIST(B,C); }
projection(A) ::= expr(B) alias(C). {
  A=NODE(PROJECTION,.span=C?COVER(SP(B),SP(C)):SP(B),.as.projection={B,C});
}
projection(A) ::= STAR(B). { A=NODE(PROJECTION,.span=B,.as.projection={AT(STAR,B),0}); }
projection(A) ::= name(B) DOT STAR(C). {
  sqlparser_span span=COVER(SP(B),C);
  A=NODE(PROJECTION,.span=span,.as.projection={AT(STAR,span),0});
}
alias(A) ::= . { A=0; }
alias(A) ::= AS ident(B). { A=AT(NAME,B); }
alias(A) ::= ident(B). { A=AT(NAME,B); }
alias(A) ::= AS STRING(B). { A=AT(NAME,B); }
alias(A) ::= STRING(B). { A=AT(NAME,B); }
from(A) ::= . { A=0; }
from(A) ::= FROM tables(B). { A=B; }
%ifndef SQLITE
tables(A) ::= joined_tables(B). { A=B; }
tables(A) ::= tables(B) COMMA joined_tables(C). {
  A=NODE(JOIN,.span=COVER(SP(B),SP(C)),.as.join={SQLPARSER_JOIN_CROSS,B,C,0,{0}});
}
joined_tables(A) ::= table_ref(B). { A=B; }
joined_tables(A) ::= joined_tables(B) inner_join_op(J) table_ref(C) join_condition(K). {
  A=FINISH(NODE(JOIN,.as.join={J,B,C,K.on,K.using_columns}),SP(B));
}
joined_tables(A) ::= joined_tables(B) outer_join_op(J) table_ref(C) required_join_condition(K). {
  A=FINISH(NODE(JOIN,.as.join={J,B,C,K.on,K.using_columns}),SP(B));
}
joined_tables(A) ::= joined_tables(B) NATURAL JOIN table_ref(C). {
  A=NODE(JOIN,.span=COVER(SP(B),SP(C)),.as.join={SQLPARSER_JOIN_NATURAL,B,C,0,{0},true});
}
joined_tables(A) ::= joined_tables(B) NATURAL INNER JOIN table_ref(C). {
  A=NODE(JOIN,.span=COVER(SP(B),SP(C)),.as.join={SQLPARSER_JOIN_NATURAL,B,C,0,{0},true});
}
joined_tables(A) ::= joined_tables(B) NATURAL outer_join_op(J) table_ref(C). {
  A=NODE(JOIN,.span=COVER(SP(B),SP(C)),.as.join={.kind=J,.left=B,.right=C,.natural=true});
}
%else
// SQLite joins, including commas, bind left-to-right.
tables(A) ::= table_ref(B). { A=B; }
tables(A) ::= tables(B) COMMA table_ref(C) join_condition(K). {
  A=FINISH(NODE(JOIN,.as.join={SQLPARSER_JOIN_CROSS,B,C,K.on,K.using_columns}),SP(B));
}
tables(A) ::= tables(B) inner_join_op(J) table_ref(C) join_condition(K). {
  A=FINISH(NODE(JOIN,.as.join={J,B,C,K.on,K.using_columns}),SP(B));
}
tables(A) ::= tables(B) outer_join_op(J) table_ref(C) join_condition(K). {
  A=FINISH(NODE(JOIN,.as.join={J,B,C,K.on,K.using_columns}),SP(B));
}
tables(A) ::= tables(B) NATURAL inner_join_op(J) table_ref(C). {
  A=NODE(JOIN,.span=COVER(SP(B),SP(C)),.as.join={J==SQLPARSER_JOIN_CROSS?J:SQLPARSER_JOIN_NATURAL,B,C,0,{0},true});
}
outer_join_op(A) ::= FULL outer JOIN. { A=SQLPARSER_JOIN_FULL; }
%endif
%ifndef SQLITE
table_ref(A) ::= object_name(B) alias(C). { A=NODE(TABLE,.span=C?COVER(SP(B),SP(C)):SP(B),.as.table={B,0,C}); }
%else
table_ref(A) ::= object_name(B) alias(C) index_hint(I). {
  A=FINISH(NODE(TABLE,.as.table={.name=B,.alias=C,.indexed_by=I.name,.not_indexed=I.not_indexed}),SP(B));
}
table_ref(A) ::= object_name(B) LP arguments(D) RP alias(C). {
  A=FINISH(NODE(TABLE,.as.table={.name=B,.alias=C,.arguments=D,.table_function=true}),SP(B));
}
table_ref(A) ::= LP(T) tables(B) RP alias(C). {
  A=FINISH(NODE(TABLE,.as.table={.alias=C,.group=B}),T);
}
%type index_hint {sqlp_index_hint}
%type index_hint_name {sqlparser_span}
index_hint_name(A) ::= ident(B). { A=B; }
index_hint_name(A) ::= INDEXED(B). { A=B; }
index_hint_name(A) ::= STRING(B). { A=B; }
index_hint(A) ::= . { A=(sqlp_index_hint){0}; }
index_hint(A) ::= INDEXED BY index_hint_name(B). { A=(sqlp_index_hint){AT(NAME,B),false}; }
index_hint(A) ::= NOT INDEXED. { A=(sqlp_index_hint){0,true}; }
tables(A) ::= tables(B) NATURAL outer_join_op(J) table_ref(C). {
  A=NODE(JOIN,.span=COVER(SP(B),SP(C)),.as.join={.kind=J,.left=B,.right=C,.natural=true});
}
expr(A) ::= expr(B) in_op(N) name(C). [IN] {
  A=NODE(IN,.span=COVER(SP(B),SP(C)),.as.in={.value=B,.negated=N,.table=NODE(TABLE,.span=SP(C),.as.table={.name=C})});
}
expr(A) ::= expr(B) in_op(N) name(C) LP arguments(D) RP(E). [IN] {
  A=NODE(IN,.span=COVER(SP(B),E),.as.in={.value=B,.negated=N,.table=NODE(TABLE,.span=COVER(SP(C),E),.as.table={.name=C,.arguments=D,.table_function=true})});
}
%endif
%ifdef SQLITE
table_ref(A) ::= LP(T) query(B) RP alias(C). { A=FINISH(NODE(TABLE,.as.table={0,B,C}),T); }
%else
table_ref(A) ::= LP(T) query(B) RP alias(C) derived_columns(D). {
  if(D.count && !C) sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"derived column list requires a table alias");
  A=FINISH(NODE(TABLE,.as.table={.query=B,.alias=C,.column_aliases=D}),T);
}
%type derived_columns {sqlparser_list}
%type derived_column_names {sqlparser_list}
derived_columns(A) ::= . { A=EMPTY; }
derived_columns(A) ::= LP derived_column_names(B) RP. { A=B; }
derived_column_names(A) ::= ident(B). { A=LIST(EMPTY,AT(NAME,B)); }
derived_column_names(A) ::= derived_column_names(B) COMMA ident(C). { A=LIST(B,AT(NAME,C)); }
%endif
%ifndef SQLITE
table_ref(A) ::= LATERAL(T) LP query(B) RP lateral_alias(C) derived_columns(D). {
  A=FINISH(NODE(TABLE,.as.table={.query=B,.alias=C,.lateral=true,.column_aliases=D}),T);
}
lateral_alias(A) ::= ident(B). { A=AT(NAME,B); }
lateral_alias(A) ::= AS ident(B). { A=AT(NAME,B); }
%endif
%type inner_join_op {sqlparser_join_kind}
%type outer_join_op {sqlparser_join_kind}
inner_join_op(A) ::= JOIN. { A=SQLPARSER_JOIN_INNER; }
inner_join_op(A) ::= INNER JOIN. { A=SQLPARSER_JOIN_INNER; }
inner_join_op(A) ::= CROSS JOIN. { A=SQLPARSER_JOIN_CROSS; }
outer_join_op(A) ::= LEFT outer JOIN. { A=SQLPARSER_JOIN_LEFT; }
outer_join_op(A) ::= RIGHT outer JOIN. { A=SQLPARSER_JOIN_RIGHT; }
outer ::= .
outer ::= OUTER.
%type join_condition {sqlp_join_condition}
%type required_join_condition {sqlp_join_condition}
%ifdef SQLITE
join_condition(A) ::= . { A=(sqlp_join_condition){0}; }
%else
join_condition(A) ::= . [MYSQL_EMPTY_JOIN] { A=(sqlp_join_condition){0}; }
%endif
join_condition(A) ::= required_join_condition(B). { A=B; }
required_join_condition(A) ::= ON expr(B). { A=(sqlp_join_condition){.on=B}; }
required_join_condition(A) ::= USING LP names(B) RP. { A=(sqlp_join_condition){.using_columns=B}; }
where(A) ::= . { A=0; }
where(A) ::= WHERE expr(B). { A=B; }
having(A) ::= . { A=0; }
having(A) ::= HAVING expr(B). { A=B; }
%type group_by {sqlparser_list}
group_by(A) ::= . { A=EMPTY; }
group_by(A) ::= GROUP BY exprs(B). { A=B; }
%type order_by {sqlparser_list}
%type orders {sqlparser_list}
order_by(A) ::= . { A=EMPTY; }
order_by(A) ::= ORDER BY orders(B). { A=B; }
orders(A) ::= order(B). { A=LIST(EMPTY,B); }
orders(A) ::= orders(B) COMMA order(C). { A=LIST(B,C); }
order(A) ::= expr(B) direction(C). { A=NODE(ORDER,.span=SP(B),.as.order={B,C}); }
%type direction {bool}
direction(A) ::= . { A=false; }
direction(A) ::= ASC. { A=false; }
direction(A) ::= DESC. { A=true; }
limit(A) ::= . { A=0; }
limit(A) ::= LIMIT(T) expr(B). { A=FINISH(NODE(LIMIT,.as.limit={B,0}),T); }
limit(A) ::= LIMIT(T) expr(B) OFFSET expr(C). { A=FINISH(NODE(LIMIT,.as.limit={B,C}),T); }
limit(A) ::= LIMIT(T) expr(B) COMMA expr(C). { A=FINISH(NODE(LIMIT,.as.limit={C,B}),T); }

%type exprs {sqlparser_list}
%type arguments {sqlparser_list}
exprs(A) ::= expr(B). { A=LIST(EMPTY,B); }
exprs(A) ::= exprs(B) COMMA expr(C). { A=LIST(B,C); }
arguments(A) ::= . { A=EMPTY; }
arguments(A) ::= exprs(B). { A=B; }
literal(A) ::= NUMBER(B). { A=AT(NUMBER,B); }
literal(A) ::= STRING(B). { A=AT(STRING,B); }
literal(A) ::= NULL(B). { A=AT(NULL,B); }
literal(A) ::= BOOLEAN(B). { A=AT(BOOLEAN,B); }
literal(A) ::= BLOB(B). { A=AT(BLOB,B); }
expr(A) ::= literal(B). { A=B; }
expr(A) ::= name(B). { A=B; }
expr(A) ::= PARAMETER(B). { A=AT(PARAMETER,B); }
%ifndef SQLITE
expr(A) ::= VARIABLE(B). { A=AT(VARIABLE,B); }
expr(A) ::= VARIABLE(B) DOT ident(C). { A=AT(VARIABLE,COVER(B,C)); }
expr(A) ::= DEFAULT(T) LP name(B) RP(E). {
  A=NODE(CALL,.span=COVER(T,E),.as.call={AT(NAME,T),LIST(EMPTY,B),false});
}
expr(A) ::= mysql_timestamp(B). { A=B; }
expr(A) ::= expr(B) XOR expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_XOR,B,C,0}); }
expr(A) ::= mysql_user_variable(B) ASSIGN expr(C). { A=NODE(ASSIGNMENT,.span=COVER(SP(B),SP(C)),.as.assignment={B,C,SQLPARSER_SCOPE_DEFAULT}); }
mysql_timestamp(A) ::= CURRENT_TIMESTAMP(T). { A=NODE(CALL,.span=T,.as.call={AT(NAME,T),{0},false}); }
mysql_timestamp(A) ::= CURRENT_TIMESTAMP(T) LP RP(E). { A=NODE(CALL,.span=COVER(T,E),.as.call={AT(NAME,T),{0},false}); }
mysql_timestamp(A) ::= CURRENT_TIMESTAMP(T) LP NUMBER(N) RP(E). {
  const char *text=sqlparser_text(ctx->document,N);
  for (size_t i=0;i<N.length;++i) {
    if (text[i]<'0' || text[i]>'9') {
      sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"timestamp precision must be an unsigned integer");
      break;
    }
  }
  A=NODE(CALL,.span=COVER(T,E),.as.call={AT(NAME,T),LIST(EMPTY,AT(NUMBER,N)),false});
}
%endif
%ifdef SQLITE
expr(A) ::= CAST(T) LP expr(B) AS type(C) RP(E). {
  A=NODE(CAST,.span=COVER(T,E),.as.cast={B,C});
}
%else
expr(A) ::= CAST(T) LP expr(B) AS mysql_cast_type(C) RP(E). {
  A=NODE(CAST,.span=COVER(T,E),.as.cast={B,C});
}
mysql_cast_type(A) ::= type(B). { A=B; }
mysql_cast_type(A) ::= UNSIGNED(T). {
  A=NODE(TYPE,.span=T,.as.type={AT(NAME,T),EMPTY,false,false});
}
mysql_cast_type(A) ::= UNSIGNED(T) ident(I). {
  if (!sqlp_cast_word(ctx,I,"INTEGER",sizeof("INTEGER")-1))
    sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"UNSIGNED CAST permits only INTEGER suffix");
  A=NODE(TYPE,.span=COVER(T,I),.as.type={AT(NAME,T),EMPTY,false,false});
}
mysql_cast_type(A) ::= ident(T) ident(I). {
  const bool signed_type=sqlp_cast_word(ctx,T,"SIGNED",sizeof("SIGNED")-1) &&
      sqlp_cast_word(ctx,I,"INTEGER",sizeof("INTEGER")-1);
  const bool double_type=sqlp_cast_word(ctx,T,"DOUBLE",sizeof("DOUBLE")-1) &&
      sqlp_cast_word(ctx,I,"PRECISION",sizeof("PRECISION")-1);
  if (!signed_type && !double_type)
    sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"numeric CAST target has invalid suffix");
  A=NODE(TYPE,.span=COVER(T,I),.as.type={AT(NAME,T),EMPTY,false,false});
}
%endif
%type collation_name {sqlparser_span}
collation_name(A) ::= ident(B). { A=B; }
%ifdef SQLITE
collation_name(A) ::= STRING(B). { A=B; }
%endif
expr(A) ::= expr(B) COLLATE collation_name(C). {
  A=NODE(COLLATE,.span=COVER(SP(B),C),.as.collate={B,AT(NAME,C)});
}
expr(A) ::= LP expr(B) RP. { A=B; }
expr(A) ::= LP(T) query(B) RP(E). [SCALAR_QUERY] { A=NODE(SUBQUERY,.span=COVER(T,E),.as.subquery={B}); }
call_name(A) ::= name(B). { A=B; }
%ifndef SQLITE
// A function token is callable but cannot name an unqualified table or column.
call_name(A) ::= MYSQL_FUNCTION(B). { A=AT(NAME,B); }
call_name(A) ::= COUNT(B). { A=AT(NAME,B); }
// MySQL retains the deprecated VALUES(column) function for duplicate-key
// assignments. It is represented as an ordinary CALL so consumers can choose
// whether to implement or reject its context-dependent value semantics.
call_name(A) ::= VALUES(B). { A=AT(NAME,B); }
// MOD is a reserved operator keyword with a two-expression function form.
// Keep it separate from '%' and generic calls to reject malformed arguments.
expr(A) ::= MYSQL_MOD(T) LP expr(B) COMMA expr(C) RP(E). {
  sqlparser_id name=AT(NAME,T);
  sqlparser_list arguments=LIST(LIST(EMPTY,B),C);
  A=NODE(CALL,.span=COVER(T,E),.as.call={name,arguments,false});
}
%endif
expr(A) ::= call_expression(B). { A=B; }
call_expression(A) ::= call_name(B) LP distinct(D) arguments(C) RP(E). {
  A=NODE(CALL,.span=COVER(SP(B),E),.as.call={B,C,D});
}
call_expression(A) ::= call_name(B) LP STAR(C) RP(E). {
  A=NODE(CALL,.span=COVER(SP(B),E),.as.call={B,LIST(EMPTY,AT(STAR,C)),false});
}
%ifndef SQLITE
%type window_partition {sqlparser_list}
window_partition(A) ::= . { A=EMPTY; }
window_partition(A) ::= PARTITION BY exprs(B). { A=B; }
%type window_base {sqlparser_id}
window_base(A) ::= . { A=0; }
window_base(A) ::= ident(B). { A=AT(NAME,B); }
%type window_definitions {sqlparser_list}
window_definitions(A) ::= . { A=EMPTY; }
window_definitions(A) ::= WINDOW window_declarations(B). { A=B; }
%type window_declarations {sqlparser_list}
window_declarations(A) ::= window_declaration(B). { A=LIST(EMPTY,B); }
window_declarations(A) ::= window_declarations(B) COMMA window_declaration(C). { A=LIST(B,C); }
window_declaration(A) ::= ident(N) AS LP window_base(B) window_partition(P) order_by(O) window_frame(F) RP(E). {
  A=NODE(WINDOW_DEFINITION,.span=COVER(N,E),.as.window_definition={AT(NAME,N),B,P,O,F});
}
expr(A) ::= call_expression(B) OVER LP window_base(N) window_partition(P) order_by(O) window_frame(F) RP(E). {
  sqlp_check_window_call(ctx,B);
  A=NODE(WINDOW,.span=COVER(SP(B),E),.as.window={B,0,P,O,N,F});
}
expr(A) ::= call_expression(B) OVER ident(N). {
  sqlp_check_window_call(ctx,B);
  A=NODE(WINDOW,.span=COVER(SP(B),N),.as.window={B,AT(NAME,N),{0},{0}});
}
%type window_units {sqlp_frame_units}
window_units(A) ::= ROWS(T). { A=(sqlp_frame_units){SQLPARSER_FRAME_ROWS,T}; }
window_units(A) ::= RANGE(T). { A=(sqlp_frame_units){SQLPARSER_FRAME_RANGE,T}; }
window_frame(A) ::= . { A=0; }
window_frame(A) ::= window_units(U) window_extent(B). {
  A=NODE(WINDOW_FRAME,.span=COVER(U.span,SP(B.end?B.end:B.start)),.as.frame={U.unit,B.start,B.end});
}
%type window_extent {sqlp_frame_extent}
window_extent(A) ::= window_start(B). { A=(sqlp_frame_extent){B,0}; }
window_extent(A) ::= BETWEEN window_boundary(B) AND window_boundary(C). { A=(sqlp_frame_extent){B,C}; }
window_start(A) ::= UNBOUNDED(T) PRECEDING(E). {
  A=NODE(WINDOW_BOUNDARY,.span=COVER(T,E),.as.boundary={SQLPARSER_BOUND_UNBOUNDED_PRECEDING,0,0});
}
window_start(A) ::= CURRENT(T) ROW(E). {
  A=NODE(WINDOW_BOUNDARY,.span=COVER(T,E),.as.boundary={SQLPARSER_BOUND_CURRENT_ROW,0,0});
}
window_start(A) ::= window_distance(B) PRECEDING(E). {
  A=NODE(WINDOW_BOUNDARY,.span=COVER(SP(B),E),.as.boundary={SQLPARSER_BOUND_PRECEDING,B,0});
}
window_start(A) ::= INTERVAL(T) expr(B) ident(U) PRECEDING(E). {
  sqlp_check_interval_unit(ctx,U);
  A=NODE(WINDOW_BOUNDARY,.span=COVER(T,E),.as.boundary={SQLPARSER_BOUND_PRECEDING,B,AT(NAME,U)});
}
window_boundary(A) ::= window_start(B). { A=B; }
window_boundary(A) ::= UNBOUNDED(T) FOLLOWING(E). {
  A=NODE(WINDOW_BOUNDARY,.span=COVER(T,E),.as.boundary={SQLPARSER_BOUND_UNBOUNDED_FOLLOWING,0,0});
}
window_boundary(A) ::= window_distance(B) FOLLOWING(E). {
  A=NODE(WINDOW_BOUNDARY,.span=COVER(SP(B),E),.as.boundary={SQLPARSER_BOUND_FOLLOWING,B,0});
}
window_boundary(A) ::= INTERVAL(T) expr(B) ident(U) FOLLOWING(E). {
  sqlp_check_interval_unit(ctx,U);
  A=NODE(WINDOW_BOUNDARY,.span=COVER(T,E),.as.boundary={SQLPARSER_BOUND_FOLLOWING,B,AT(NAME,U)});
}
window_distance(A) ::= NUMBER(B). { A=AT(NUMBER,B); }
window_distance(A) ::= PARAMETER(B). { A=AT(PARAMETER,B); }
ident(A) ::= CURRENT(B). { A=B; }
ident(A) ::= UNBOUNDED(B). { A=B; }
ident(A) ::= PRECEDING(B). { A=B; }
ident(A) ::= FOLLOWING(B). { A=B; }
// Preserve the existing MySQL identifier admission while sharing the frame token.
ident(A) ::= ROW(B). { A=B; }
%endif
expr(A) ::= NOT(T) expr(B). { A=NODE(UNARY,.span=COVER(T,SP(B)),.as.unary={SQLPARSER_OP_NOT,B}); }
expr(A) ::= MINUS(T) expr(B). [NEGATE] { A=NODE(UNARY,.span=COVER(T,SP(B)),.as.unary={SQLPARSER_OP_NEGATE,B}); }
expr(A) ::= PLUS(T) expr(B). [NEGATE] { A=NODE(UNARY,.span=COVER(T,SP(B)),.as.unary={SQLPARSER_OP_POSITIVE,B}); }
expr(A) ::= BITNOT(T) expr(B). { A=NODE(UNARY,.span=COVER(T,SP(B)),.as.unary={SQLPARSER_OP_BIT_NOT,B}); }
expr(A) ::= EXISTS(T) LP query(B) RP(E). { A=NODE(UNARY,.span=COVER(T,E),.as.unary={SQLPARSER_OP_EXISTS,B}); }
expr(A) ::= expr(B) OR expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_OR,B,C,0}); }
expr(A) ::= expr(B) AND expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_AND,B,C,0}); }
expr(A) ::= expr(B) EQ expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_EQ,B,C,0}); }
expr(A) ::= expr(B) NE expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_NE,B,C,0}); }
expr(A) ::= expr(B) LT expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_LT,B,C,0}); }
expr(A) ::= expr(B) LE expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_LE,B,C,0}); }
expr(A) ::= expr(B) GT expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_GT,B,C,0}); }
expr(A) ::= expr(B) GE expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_GE,B,C,0}); }
%ifndef SQLITE
expr(A) ::= expr(B) NULL_SAFE_EQ expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_NULL_SAFE_EQ,B,C,0}); }
%endif
expr(A) ::= expr(B) PLUS expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_ADD,B,C,0}); }
expr(A) ::= expr(B) MINUS expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_SUBTRACT,B,C,0}); }
expr(A) ::= expr(B) STAR expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_MULTIPLY,B,C,0}); }
expr(A) ::= expr(B) SLASH expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_DIVIDE,B,C,0}); }
expr(A) ::= expr(B) MOD expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_MODULO,B,C,0}); }
%ifndef SQLITE
expr(A) ::= expr(B) MYSQL_MOD expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_MODULO,B,C,0}); }
expr(A) ::= expr(B) DIV expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_INTEGER_DIVIDE,B,C,0}); }
%endif
expr(A) ::= expr(B) BITAND expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_BIT_AND,B,C,0}); }
expr(A) ::= expr(B) BITOR expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_BIT_OR,B,C,0}); }
expr(A) ::= expr(B) LSHIFT expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_SHIFT_LEFT,B,C,0}); }
expr(A) ::= expr(B) RSHIFT expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_SHIFT_RIGHT,B,C,0}); }
%ifndef SQLITE
expr(A) ::= expr(B) IS NULL(E). { A=NODE(UNARY,.span=COVER(SP(B),E),.as.unary={SQLPARSER_OP_IS_NULL,B}); }
expr(A) ::= expr(B) IS NOT NULL(E). { A=NODE(UNARY,.span=COVER(SP(B),E),.as.unary={SQLPARSER_OP_IS_NOT_NULL,B}); }
%else
expr(A) ::= expr(B) CONCAT expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_CONCAT,B,C,0}); }
expr(A) ::= expr(B) IS expr(C). { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_IS,B,C,0}); }
expr(A) ::= expr(B) IS NOT expr(C). [IS] { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_IS_NOT,B,C,0}); }
expr(A) ::= expr(B) IS DISTINCT FROM expr(C). [IS] { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_IS_NOT,B,C,0}); }
expr(A) ::= expr(B) IS NOT DISTINCT FROM expr(C). [IS] { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={SQLPARSER_OP_IS,B,C,0}); }
expr(A) ::= expr(B) ISNULL(E). { A=NODE(UNARY,.span=COVER(SP(B),E),.as.unary={SQLPARSER_OP_IS_NULL,B}); }
expr(A) ::= expr(B) NOTNULL(E). { A=NODE(UNARY,.span=COVER(SP(B),E),.as.unary={SQLPARSER_OP_IS_NOT_NULL,B}); }
expr(A) ::= expr(B) NOT NULL(E). [ISNULL] { A=NODE(UNARY,.span=COVER(SP(B),E),.as.unary={SQLPARSER_OP_IS_NOT_NULL,B}); }
%endif
%type between_op {bool}
%type in_op {bool}
%type like_op {bool}
between_op(A) ::= BETWEEN. { A=false; }
between_op(A) ::= NOT BETWEEN. { A=true; }
in_op(A) ::= IN. { A=false; }
in_op(A) ::= NOT IN. { A=true; }
like_op(A) ::= LIKE. { A=false; }
like_op(A) ::= NOT LIKE. { A=true; }
expr(A) ::= expr(B) between_op(N) expr(C) AND expr(D). [BETWEEN] {
  A=NODE(BETWEEN,.span=COVER(SP(B),SP(D)),.as.between={B,C,D,N});
}
expr(A) ::= expr(B) in_op(N) LP exprs(C) RP(E). [IN] {
  A=NODE(IN,.span=COVER(SP(B),E),.as.in={B,0,C,N});
}
expr(A) ::= expr(B) in_op(N) LP query(C) RP(E). [IN] {
  A=NODE(IN,.span=COVER(SP(B),E),.as.in={B,C,{0},N});
}
expr(A) ::= expr(B) like_op(N) expr(C) escape(E). [LIKE] {
  A=NODE(BINARY,.span=COVER(SP(B),SP(E?E:C)),.as.binary={N?SQLPARSER_OP_NOT_LIKE:SQLPARSER_OP_LIKE,B,C,E});
}
escape(A) ::= . [ESCAPE] { A=0; }
escape(A) ::= ESCAPE expr(B). [ESCAPE] { A=B; }
expr(A) ::= CASE(T) case_operand(B) branches(C) otherwise(D) END(E). {
  A=NODE(CASE,.span=COVER(T,E),.as.case_expr={B,D,C});
}
case_operand(A) ::= . { A=0; }
case_operand(A) ::= expr(B). { A=B; }
%type branches {sqlparser_list}
branches(A) ::= branch(B). { A=LIST(EMPTY,B); }
branches(A) ::= branches(B) branch(C). { A=LIST(B,C); }
branch(A) ::= WHEN(T) expr(B) THEN expr(C). { A=NODE(WHEN,.span=COVER(T,SP(C)),.as.when={B,C}); }
otherwise(A) ::= . { A=0; }
otherwise(A) ::= ELSE expr(B). { A=B; }

%type insert_op {sqlp_insert_head}
%ifdef SQLITE
insert_op(A) ::= INSERT(T). { A=(sqlp_insert_head){T,false}; }
insert_op(A) ::= REPLACE(T). { A=(sqlp_insert_head){T,true}; }
%else
%type low_priority {bool}
low_priority(A) ::= . { A=false; }
low_priority(A) ::= LOW_PRIORITY. { A=true; }
%type mysql_ignore {bool}
mysql_ignore(A) ::= . { A=false; }
mysql_ignore(A) ::= IGNORE. { A=true; }
insert_op(A) ::= INSERT(T) low_priority(P) mysql_ignore(I). {
  A=(sqlp_insert_head){T,false,I?SQLPARSER_CONFLICT_IGNORE:SQLPARSER_CONFLICT_DEFAULT,P};
}
insert_op(A) ::= REPLACE(T) low_priority(P). { A=(sqlp_insert_head){T,true,0,P}; }
%endif
%ifndef SQLITE
into ::= .
%endif
into ::= INTO.
%ifdef SQLITE
%type columns {sqlparser_list}
columns(A) ::= . { A=EMPTY; }
columns(A) ::= LP names(B) RP. { A=B; }
%endif
%type insert_columns {sqlp_insert_columns}
insert_columns(A) ::= . { A=(sqlp_insert_columns){EMPTY,false}; }
insert_columns(A) ::= LP names(B) RP. { A=(sqlp_insert_columns){B,true}; }
%ifndef SQLITE
insert_columns(A) ::= LP RP. { A=(sqlp_insert_columns){EMPTY,true}; }
%endif
%ifndef SQLITE
dml(A) ::= insert_op(R) into object_name(T) insert_columns(C) VALUES rows(V) mysql_insert_alias(I) mysql_duplicate(D). {
  A=FINISH(NODE(INSERT,.as.insert={T,0,C.list,V,{0},R.replace,R.conflict,
    .low_priority=R.low_priority,.duplicate_assignments=D,
    .columns_specified=C.specified,.row_alias=I.row,
    .column_aliases=I.columns}),R.span);
}
%endif
%ifdef SQLITE
dml(A) ::= insert_op(R) into object_name(T) insert_columns(C) query(Q). {
  A=FINISH(NODE(INSERT,.as.insert={T,Q,C.list,{0},{0},R.replace,R.conflict,
    .columns_specified=C.specified}),R.span);
}
%else
// Shift '(' before choosing a column list or a parenthesized query.
dml(A) ::= insert_op(R) into object_name(T) query(Q) mysql_duplicate(D). {
  A=FINISH(NODE(INSERT,.as.insert={T,Q,{0},{0},{0},R.replace,
    .conflict=R.conflict,.low_priority=R.low_priority,.duplicate_assignments=D,
    .columns_specified=false}),R.span);
}
dml(A) ::= insert_op(R) into object_name(T) LP names(C) RP query(Q) mysql_duplicate(D). {
  A=FINISH(NODE(INSERT,.as.insert={T,Q,C,{0},{0},R.replace,
    .conflict=R.conflict,.low_priority=R.low_priority,.duplicate_assignments=D,
    .columns_specified=true}),R.span);
}
dml(A) ::= insert_op(R) into object_name(T) LP RP query(Q) mysql_duplicate(D). {
  A=FINISH(NODE(INSERT,.as.insert={T,Q,{0},{0},{0},R.replace,
    .conflict=R.conflict,.low_priority=R.low_priority,.duplicate_assignments=D,
    .columns_specified=true}),R.span);
}
%endif
%ifndef SQLITE
dml(A) ::= insert_op(R) into object_name(T) SET assignments(S) mysql_insert_alias(I) mysql_duplicate(D). {
  A=FINISH(NODE(INSERT,.as.insert={T,0,{0},{0},S,R.replace,
    .conflict=R.conflict,.low_priority=R.low_priority,.duplicate_assignments=D,
    .row_alias=I.row,.column_aliases=I.columns}),R.span);
}
%type mysql_insert_alias {sqlp_insert_alias}
mysql_insert_alias(A) ::= . { A=(sqlp_insert_alias){0}; }
mysql_insert_alias(A) ::= AS ident(R). { A=(sqlp_insert_alias){AT(NAME,R),EMPTY}; }
mysql_insert_alias(A) ::= AS ident(R) LP mysql_insert_alias_names(C) RP. {
  A=(sqlp_insert_alias){AT(NAME,R),C};
}
%type mysql_insert_alias_names {sqlparser_list}
mysql_insert_alias_names(A) ::= ident(B). { A=LIST(EMPTY,AT(NAME,B)); }
mysql_insert_alias_names(A) ::= mysql_insert_alias_names(B) COMMA ident(C). {
  A=LIST(B,AT(NAME,C));
}
%type mysql_duplicate {sqlparser_list}
mysql_duplicate(A) ::= . { A=EMPTY; }
mysql_duplicate(A) ::= ON DUPLICATE KEY UPDATE assignments(B). { A=B; }
%endif
%type rows {sqlparser_list}
rows(A) ::= row(B). { A=LIST(EMPTY,B); }
rows(A) ::= rows(B) COMMA row(C). { A=LIST(B,C); }
%ifdef SQLITE
row(A) ::= LP(T) exprs(B) RP(E). { A=NODE(ROW,.span=COVER(T,E),.as.row={B}); }
%else
%type mysql_write_values {sqlparser_list}
%type mysql_write_values_opt {sqlparser_list}
mysql_write_values_opt(A) ::= . { A=EMPTY; }
mysql_write_values_opt(A) ::= mysql_write_values(B). { A=B; }
mysql_write_values(A) ::= write_value(B). { A=LIST(EMPTY,B); }
mysql_write_values(A) ::= mysql_write_values(B) COMMA write_value(C). { A=LIST(B,C); }
row(A) ::= LP(T) mysql_write_values_opt(B) RP(E). { A=NODE(ROW,.span=COVER(T,E),.as.row={B}); }
%endif
dml(A) ::= update_delete(B). { A=B; }
%type assignments {sqlparser_list}
assignments(A) ::= assignment(B). { A=LIST(EMPTY,B); }
assignments(A) ::= assignments(B) COMMA assignment(C). { A=LIST(B,C); }
assignment(A) ::= name(B) EQ write_value(C). { A=NODE(ASSIGNMENT,.span=COVER(SP(B),SP(C)),.as.assignment={B,C,SQLPARSER_SCOPE_DEFAULT}); }
write_value(A) ::= expr(B). { A=B; }
write_alias(A) ::= . { A=0; }
write_alias(A) ::= AS ident(B). { A=AT(NAME,B); }
%ifdef SQLITE
write_alias(A) ::= AS INDEXED(B). { A=AT(NAME,B); }
write_alias(A) ::= AS STRING(B). { A=AT(NAME,B); }
%else
write_alias(A) ::= ident(B). { A=AT(NAME,B); }
%endif
%ifndef SQLITE
// A bare DEFAULT is a complete assignment value, never an expression operand.
write_value(A) ::= default_keyword(B). { A=B; }
default_keyword(A) ::= DEFAULT(B). { A=AT(DEFAULT_VALUE,B); }
%endif
%ifndef SQLITE
update_delete(A) ::= UPDATE(T) low_priority(P) mysql_ignore(I) object_name(B) write_alias(Q) SET assignments(C) where(W) order_by(O) limit(L). {
  A=FINISH(NODE(UPDATE,.as.update={.table=B,.alias=Q,.where=W,.limit=L,
    .assignments=C,.order_by=O,
    .conflict=I?SQLPARSER_CONFLICT_IGNORE:SQLPARSER_CONFLICT_DEFAULT,
    .low_priority=P}),T);
}
%type mysql_delete_head {sqlp_write_head}
mysql_delete_head(A) ::= DELETE(T) low_priority(P). { A=(sqlp_write_head){T,P}; }
update_delete(A) ::= mysql_delete_head(T) FROM object_name(B) write_alias(Q) where(W) order_by(O) limit(L). {
  A=FINISH(NODE(DELETE,.as.delete_stmt={.table=B,.alias=Q,.where=W,.limit=L,
    .order_by=O,.low_priority=T.low_priority}),T.span);
}
%type delete_targets {sqlparser_list}
delete_targets(A) ::= delete_target(B). { A=LIST(EMPTY,B); }
delete_targets(A) ::= delete_targets(B) COMMA delete_target(C). { A=LIST(B,C); }
delete_target(A) ::= object_name(B). { A=B; }
delete_target(A) ::= name(B) DOT STAR(E). { A=AT(STAR,COVER(SP(B),E)); }
update_delete(A) ::= mysql_delete_head(T) FROM delete_targets(B) USING tables(F) where(W). {
  A=FINISH(NODE(DELETE,.as.delete_stmt={.where=W,.low_priority=T.low_priority,.targets=B,.from=F}),T.span);
}
update_delete(A) ::= mysql_delete_head(T) delete_targets(B) FROM tables(F) where(W). {
  A=FINISH(NODE(DELETE,.as.delete_stmt={.where=W,.low_priority=T.low_priority,.targets=B,.from=F}),T.span);
}
%else
update_delete(A) ::= UPDATE(T) update_conflict(F) object_name(B) write_alias(Q) index_hint(I) SET assignments(C) where(W) write_order(O) write_limit(L). {
  if (O.count && !L) sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"SQLite UPDATE ORDER BY requires LIMIT");
  A=FINISH(NODE(UPDATE,.as.update={.table=B,.alias=Q,.where=W,.limit=L,
    .assignments=C,.order_by=O,.conflict=F,.indexed_by=I.name,
    .not_indexed=I.not_indexed}),T);
}
update_delete(A) ::= DELETE(T) FROM object_name(B) write_alias(Q) index_hint(I) where(W) write_order(O) write_limit(L). {
  if (O.count && !L) sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"SQLite DELETE ORDER BY requires LIMIT");
  A=FINISH(NODE(DELETE,.as.delete_stmt={.table=B,.alias=Q,.where=W,.limit=L,
    .order_by=O,.indexed_by=I.name,.not_indexed=I.not_indexed}),T);
}
%type write_order {sqlparser_list}
%ifdef SQLITE_ENABLE_UPDATE_DELETE_LIMIT
write_order(A) ::= order_by(B). { A=B; }
write_limit(A) ::= limit(B). { A=B; }
%else
write_order(A) ::= . { A=EMPTY; }
write_limit(A) ::= . { A=0; }
%endif
%endif

%ifndef SQLITE
work ::= .
work ::= WORK.
mysql_tables_keyword ::= TABLE.
mysql_tables_keyword ::= TABLES.
%type lock_targets {sqlparser_list}
%type lock_mode {sqlparser_lock_mode}
lock_targets(A) ::= lock_target(B). { A=LIST(EMPTY,B); }
lock_targets(A) ::= lock_targets(B) COMMA lock_target(C). { A=LIST(B,C); }
lock_alias(A) ::= . { A=0; }
lock_alias(A) ::= ident(N). { A=AT(NAME,N); }
lock_alias(A) ::= AS ident(N). { A=AT(NAME,N); }
lock_mode(A) ::= READ. { A=SQLPARSER_LOCK_READ; }
lock_mode(A) ::= READ LOCAL. { A=SQLPARSER_LOCK_READ_LOCAL; }
lock_mode(A) ::= WRITE. { A=SQLPARSER_LOCK_WRITE; }
lock_target(A) ::= object_name(N) lock_alias(B) lock_mode(M). {
  A=FINISH(NODE(LOCK_TARGET,.as.lock_target={N,B,M}),SP(N));
}
command(A) ::= LOCK(T) mysql_tables_keyword lock_targets(B). { A=FINISH(NODE(LOCK_TABLES,.as.lock_tables={B}),T); }
command(A) ::= UNLOCK(T) mysql_tables_keyword. { A=FINISH(NODE(UNLOCK_TABLES,.span=T),T); }
command(A) ::= PREPARE(T) ident(N) FROM prepare_source(B). {
  A=FINISH(NODE(PREPARE,.as.prepared={AT(NAME,N),B,{0}}),T);
}
prepare_source(A) ::= STRING(B). { A=AT(STRING,B); }
prepare_source(A) ::= mysql_user_variable(B). { A=B; }
mysql_user_variable(A) ::= VARIABLE(B). {
  if (B.length>1 && sqlparser_text(ctx->document,B)[1]=='@')
    sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"expected a MySQL user variable, not a system variable");
  A=AT(VARIABLE,B);
}
command(A) ::= EXECUTE(T) ident(N) execute_parameters(B). {
  A=FINISH(NODE(EXECUTE,.as.prepared={AT(NAME,N),0,B}),T);
}
%type execute_parameters {sqlparser_list}
%type user_variables {sqlparser_list}
execute_parameters(A) ::= . { A=EMPTY; }
execute_parameters(A) ::= USING user_variables(B). { A=B; }
user_variables(A) ::= mysql_user_variable(B). { A=LIST(EMPTY,B); }
user_variables(A) ::= user_variables(B) COMMA mysql_user_variable(C). { A=LIST(B,C); }
%type deallocate_head {sqlparser_span}
deallocate_head(A) ::= DEALLOCATE(T). { A=T; }
deallocate_head(A) ::= DROP(T). { A=T; }
command(A) ::= deallocate_head(T) PREPARE ident(N). {
  A=FINISH(NODE(DEALLOCATE,.as.prepared={.name=AT(NAME,N)}),T);
}
command(A) ::= mysql_with_write(B). { A=B; }
mysql_with_write(A) ::= WITH(T) recursive_opt(R) ctes(C) update_delete(D). {
  A=FINISH(NODE(WITH,.as.with={C,D,R}),T);
}
command(A) ::= SAVEPOINT(T) ident(N). { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_SAVEPOINT,0,AT(NAME,N)}),T); }
command(A) ::= RELEASE(T) SAVEPOINT ident(N). { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_RELEASE,0,AT(NAME,N)}),T); }
command(A) ::= ROLLBACK(T) work TO mysql_savepoint_opt ident(N). { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_ROLLBACK_TO,0,AT(NAME,N)}),T); }
mysql_savepoint_opt ::= .
mysql_savepoint_opt ::= SAVEPOINT.
command(A) ::= BEGIN(T) work. { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_BEGIN}),T); }
command(A) ::= START(T) TRANSACTION start_options(O). {
  A=FINISH(NODE(TRANSACTION,.as.transaction={.kind=SQLPARSER_START_TRANSACTION,.access=O.access,.consistent_snapshot=O.consistent_snapshot}),T);
}
%type start_options {sqlp_transaction_options}
%type start_options_list {sqlp_transaction_options}
%type start_option {sqlp_transaction_options}
start_options(A) ::= . { A=(sqlp_transaction_options){0}; }
start_options(A) ::= start_options_list(B). { A=B; }
start_options_list(A) ::= start_option(B). { A=B; }
start_options_list(A) ::= start_options_list(B) COMMA start_option(C). {
  if (B.access && C.access && B.access != C.access)
    sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"conflicting transaction access modes");
  A=(sqlp_transaction_options){.access=B.access?B.access:C.access,.consistent_snapshot=B.consistent_snapshot||C.consistent_snapshot};
}
start_option(A) ::= WITH CONSISTENT SNAPSHOT. { A=(sqlp_transaction_options){.consistent_snapshot=true}; }
start_option(A) ::= transaction_access(B). { A=(sqlp_transaction_options){.access=B}; }
%type transaction_access {sqlparser_transaction_access}
transaction_access(A) ::= READ ONLY. { A=SQLPARSER_READ_ONLY; }
transaction_access(A) ::= READ WRITE. { A=SQLPARSER_READ_WRITE; }
%type transaction_end {sqlp_transaction_head}
transaction_end(A) ::= COMMIT(T) work. { A=(sqlp_transaction_head){T,SQLPARSER_COMMIT}; }
transaction_end(A) ::= ROLLBACK(T) work. { A=(sqlp_transaction_head){T,SQLPARSER_ROLLBACK}; }
%type transaction_chain {sqlparser_choice}
%type transaction_release {sqlparser_choice}
transaction_chain(A) ::= . { A=SQLPARSER_CHOICE_UNSPECIFIED; }
transaction_chain(A) ::= AND CHAIN. { A=SQLPARSER_CHOICE_YES; }
transaction_chain(A) ::= AND NO CHAIN. { A=SQLPARSER_CHOICE_NO; }
transaction_release(A) ::= . { A=SQLPARSER_CHOICE_UNSPECIFIED; }
transaction_release(A) ::= RELEASE. { A=SQLPARSER_CHOICE_YES; }
transaction_release(A) ::= NO RELEASE. { A=SQLPARSER_CHOICE_NO; }
command(A) ::= transaction_end(T) transaction_chain(C) transaction_release(R). {
  if (C==SQLPARSER_CHOICE_YES && R==SQLPARSER_CHOICE_YES)
    sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"AND CHAIN cannot be combined with RELEASE");
  A=FINISH(NODE(TRANSACTION,.as.transaction={.kind=T.kind,.chain=C,.release=R}),T.span);
}
%type scope {sqlparser_scope}
scope(A) ::= . { A=SQLPARSER_SCOPE_DEFAULT; }
scope(A) ::= SESSION. { A=SQLPARSER_SCOPE_SESSION; }
scope(A) ::= GLOBAL. { A=SQLPARSER_SCOPE_GLOBAL; }
scope(A) ::= LOCAL. { A=SQLPARSER_SCOPE_LOCAL; }
%type transaction_isolation {sqlparser_isolation}
transaction_isolation(A) ::= ISOLATION LEVEL REPEATABLE READ. { A=SQLPARSER_REPEATABLE_READ; }
transaction_isolation(A) ::= ISOLATION LEVEL READ COMMITTED. { A=SQLPARSER_READ_COMMITTED; }
transaction_isolation(A) ::= ISOLATION LEVEL READ UNCOMMITTED. { A=SQLPARSER_READ_UNCOMMITTED; }
transaction_isolation(A) ::= ISOLATION LEVEL SERIALIZABLE. { A=SQLPARSER_SERIALIZABLE; }
%type transaction_characteristics {sqlp_transaction_options}
transaction_characteristics(A) ::= transaction_isolation(B). { A=(sqlp_transaction_options){.isolation=B}; }
transaction_characteristics(A) ::= transaction_access(B). { A=(sqlp_transaction_options){.access=B}; }
transaction_characteristics(A) ::= transaction_isolation(B) COMMA transaction_access(C). { A=(sqlp_transaction_options){.isolation=B,.access=C}; }
transaction_characteristics(A) ::= transaction_access(B) COMMA transaction_isolation(C). { A=(sqlp_transaction_options){.access=B,.isolation=C}; }
command(A) ::= SET(T) scope(S) TRANSACTION transaction_characteristics(O). {
  A=FINISH(NODE(TRANSACTION,.as.transaction={.kind=SQLPARSER_SET_TRANSACTION,.access=O.access,.isolation=O.isolation,.scope=S}),T);
}
%type set_assignments {sqlparser_list}
set_assignments(A) ::= set_assignment(B). { A=LIST(EMPTY,B); }
set_assignments(A) ::= set_assignments(B) COMMA set_assignment(C). { A=LIST(B,C); }
%type set_value {sqlparser_id}
set_value(A) ::= write_value(B). { A=B; }
set_value(A) ::= ON(B). { A=AT(NAME,B); }
set_assignment(A) ::= scope(S) name(B) EQ set_value(C). { A=NODE(ASSIGNMENT,.span=COVER(SP(B),SP(C)),.as.assignment={B,C,S}); }
set_assignment(A) ::= VARIABLE(B) EQ expr(C). { A=NODE(ASSIGNMENT,.span=COVER(B,SP(C)),.as.assignment={AT(VARIABLE,B),C,SQLPARSER_SCOPE_DEFAULT}); }
set_assignment(A) ::= VARIABLE(B) EQ ON(C). {
  if (ctx->error.status == SQLPARSER_OK && (B.length < 2 || sqlparser_text(ctx->document,B)[1] != '@')) {
    sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"ON cannot be assigned to a MySQL user variable");
    ctx->error.offset=C.offset;
  }
  A=NODE(ASSIGNMENT,.span=COVER(B,C),.as.assignment={AT(VARIABLE,B),AT(NAME,C),SQLPARSER_SCOPE_DEFAULT});
}
set_assignment(A) ::= VARIABLE(B) EQ DEFAULT(C). {
  if (ctx->error.status == SQLPARSER_OK && (B.length < 2 || sqlparser_text(ctx->document,B)[1] != '@')) {
    sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"DEFAULT cannot be assigned to a MySQL user variable");
    ctx->error.offset=C.offset;
  }
  A=NODE(ASSIGNMENT,.span=COVER(B,C),.as.assignment={AT(VARIABLE,B),AT(DEFAULT_VALUE,C),SQLPARSER_SCOPE_DEFAULT});
}
set_assignment(A) ::= VARIABLE(B) DOT ident(N) EQ set_value(C). {
  sqlparser_scope scope=sqlp_variable_scope(ctx,B);
  A=NODE(ASSIGNMENT,.span=COVER(B,SP(C)),.as.assignment={AT(NAME,N),C,scope});
}
charset(A) ::= ident(B). { A=AT(NAME,B); }
charset(A) ::= STRING(B). { A=AT(STRING,B); }
set_charset(A) ::= charset(B). { A=B; }
set_charset(A) ::= default_keyword(B). { A=B; }
command(A) ::= SET(T) set_assignments(B). { A=FINISH(NODE(SET,.as.set={SQLPARSER_SET_ASSIGNMENTS,B,0}),T); }
command(A) ::= SET(T) NAMES set_charset(B). { A=FINISH(NODE(SET,.as.set={SQLPARSER_SET_NAMES,{0},B}),T); }
command(A) ::= SET(T) CHARACTER SET set_charset(B). { A=FINISH(NODE(SET,.as.set={SQLPARSER_SET_CHARACTER_SET,{0},B}),T); }

%type full {bool}
full(A) ::= . { A=false; }
full(A) ::= FULL. { A=true; }
database(A) ::= . { A=0; }
database(A) ::= FROM name(B). { A=B; }
database(A) ::= IN name(B). { A=B; }
%type filter {sqlp_show_filter}
filter(A) ::= . { A=(sqlp_show_filter){0}; }
filter(A) ::= LIKE STRING(B). { A=(sqlp_show_filter){.pattern=AT(STRING,B)}; }
filter(A) ::= WHERE expr(B). { A=(sqlp_show_filter){.where=B}; }
command(A) ::= SHOW(T) DATABASES filter(F). { A=FINISH(NODE(SHOW,.as.show={SQLPARSER_SHOW_DATABASES,SQLPARSER_SCOPE_DEFAULT,0,F.pattern,F.where,false}),T); }
command(A) ::= SHOW(T) full(U) TABLES database(D) filter(F). { A=FINISH(NODE(SHOW,.as.show={SQLPARSER_SHOW_TABLES,SQLPARSER_SCOPE_DEFAULT,D,F.pattern,F.where,U}),T); }
command(A) ::= SHOW(T) TABLE STATUS database(D) filter(F). { A=FINISH(NODE(SHOW,.as.show={SQLPARSER_SHOW_TABLE_STATUS,SQLPARSER_SCOPE_DEFAULT,D,F.pattern,F.where,false}),T); }
command(A) ::= SHOW(T) scope(S) VARIABLES filter(F). { A=FINISH(NODE(SHOW,.as.show={SQLPARSER_SHOW_VARIABLES,S,0,F.pattern,F.where,false}),T); }
command(A) ::= SHOW(T) COLLATION filter(F). { A=FINISH(NODE(SHOW,.as.show={SQLPARSER_SHOW_COLLATION,SQLPARSER_SCOPE_DEFAULT,0,F.pattern,F.where,false}),T); }

%type show_database_kind {sqlparser_show_kind}
show_database_kind(A) ::= TRIGGERS. { A=SQLPARSER_SHOW_TRIGGERS; }
show_database_kind(A) ::= EVENTS. { A=SQLPARSER_SHOW_EVENTS; }
show_database_kind(A) ::= OPEN TABLES. { A=SQLPARSER_SHOW_OPEN_TABLES; }
command(A) ::= SHOW(T) show_database_kind(K) database(D) filter(F). {
  A=FINISH(NODE(SHOW,.as.show={K,SQLPARSER_SCOPE_DEFAULT,D,F.pattern,F.where,false}),T);
}
%type show_filter_kind {sqlparser_show_kind}
show_filter_kind(A) ::= CHARACTER SET. { A=SQLPARSER_SHOW_CHARACTER_SET; }
show_filter_kind(A) ::= CHARSET. { A=SQLPARSER_SHOW_CHARACTER_SET; }
show_filter_kind(A) ::= PROCEDURE STATUS. { A=SQLPARSER_SHOW_PROCEDURE_STATUS; }
show_filter_kind(A) ::= FUNCTION STATUS. { A=SQLPARSER_SHOW_FUNCTION_STATUS; }
command(A) ::= SHOW(T) show_filter_kind(K) filter(F). {
  A=FINISH(NODE(SHOW,.as.show={K,SQLPARSER_SCOPE_DEFAULT,0,F.pattern,F.where,false}),T);
}
command(A) ::= SHOW(T) scope(S) STATUS filter(F). {
  A=FINISH(NODE(SHOW,.as.show={SQLPARSER_SHOW_STATUS,S,0,F.pattern,F.where,false}),T);
}
%type show_extended {bool}
show_extended(A) ::= . { A=false; }
show_extended(A) ::= EXTENDED. { A=true; }
show_columns ::= COLUMNS.
show_columns ::= FIELDS.
show_indexes ::= INDEX.
show_indexes ::= INDEXES.
show_indexes ::= KEYS.
show_from ::= FROM.
show_from ::= IN.
%type show_columns_options {sqlp_show_options}
show_columns_options(A) ::= full(U) show_columns. { A=(sqlp_show_options){U,false}; }
show_columns_options(A) ::= EXTENDED full(U) show_columns. { A=(sqlp_show_options){U,true}; }
command(A) ::= SHOW(T) show_columns_options(O) show_from object_name(N) database(D) filter(F). {
  A=FINISH(NODE(SHOW,.as.show={SQLPARSER_SHOW_COLUMNS,SQLPARSER_SCOPE_DEFAULT,D,F.pattern,F.where,O.full,N,O.extended}),T);
}
command(A) ::= SHOW(T) show_extended(E) show_indexes show_from object_name(N) database(D) where(W). {
  A=FINISH(NODE(SHOW,.as.show={SQLPARSER_SHOW_INDEX,SQLPARSER_SCOPE_DEFAULT,D,0,W,false,N,E}),T);
}
command(A) ::= SHOW(T) CREATE TABLE object_name(N). {
  A=FINISH(NODE(SHOW,.as.show={.kind=SQLPARSER_SHOW_CREATE_TABLE,.table=N}),T);
}
command(A) ::= SHOW(T) WARNINGS limit(L). {
  A=FINISH(NODE(SHOW,.as.show={.kind=SQLPARSER_SHOW_WARNINGS,.limit=L}),T);
}
command(A) ::= SHOW(T) COUNT LP STAR RP WARNINGS(E). {
  A=FINISH(NODE(SHOW,.as.show={.kind=SQLPARSER_SHOW_WARNINGS,.count=true}),COVER(T,E));
}
%endif

%type temporary {bool}
%type if_not_exists {bool}
%type if_exists {bool}
temporary(A) ::= . { A=false; }
temporary(A) ::= TEMPORARY. { A=true; }
if_not_exists(A) ::= . { A=false; }
if_not_exists(A) ::= IF NOT EXISTS. { A=true; }
if_exists(A) ::= . { A=false; }
if_exists(A) ::= IF EXISTS. { A=true; }
%ifdef SQLITE
command(A) ::= CREATE(T) temporary(U) TABLE if_not_exists(E) object_name(N) LP elements(B) RP table_options(O). {
  A=FINISH(NODE(CREATE_TABLE,.as.create_table={N,B,U,E,O}),T);
}
%else
command(A) ::= CREATE(T) temporary(U) TABLE if_not_exists(E) object_name(N) LP elements(B) RP table_options(O) mysql_table_query(Q). {
  A=FINISH(NODE(CREATE_TABLE,.as.create_table={N,B,U,E,O,Q}),T);
}
command(A) ::= CREATE(T) temporary(U) TABLE if_not_exists(E) object_name(N) mysql_required_table_options(O) mysql_as_query(Q). {
  A=FINISH(NODE(CREATE_TABLE,.as.create_table={N,{0},U,E,O,Q}),T);
}
command(A) ::= CREATE(T) temporary(U) TABLE if_not_exists(E) object_name(N) mysql_as_query(Q). {
  A=FINISH(NODE(CREATE_TABLE,.as.create_table={N,{0},U,E,{0},Q}),T);
}
%type mysql_required_table_options {sqlparser_list}
mysql_required_table_options(A) ::= table_option(B). { A=LIST(EMPTY,B); }
mysql_required_table_options(A) ::= mysql_required_table_options(B) table_option(C). { A=LIST(B,C); }
mysql_table_query(A) ::= . { A=0; }
mysql_table_query(A) ::= mysql_as_query(B). { A=B; }
mysql_as_query(A) ::= query(B). { A=B; }
mysql_as_query(A) ::= AS query(B). { A=B; }

command(A) ::= ANALYZE(T) TABLE object_name(N). {
  A=FINISH(NODE(ANALYZE,.as.maintenance={N,0}),T);
}
%type mysql_view_columns {sqlparser_list}
%type mysql_view_names {sqlparser_list}
mysql_view_columns(A) ::= . { A=EMPTY; }
mysql_view_columns(A) ::= LP mysql_view_names(B) RP. { A=B; }
mysql_view_names(A) ::= ident(B). { A=LIST(EMPTY,AT(NAME,B)); }
mysql_view_names(A) ::= mysql_view_names(B) COMMA ident(C). { A=LIST(B,AT(NAME,C)); }
command(A) ::= CREATE(T) VIEW object_name(N) mysql_view_columns(C) AS query(Q) view_check(V). {
  A=FINISH(NODE(CREATE_VIEW,.as.create_view={N,Q,C,false,false,V}),T);
}
%type view_check {sqlparser_view_check}
view_check(A) ::= . { A=SQLPARSER_VIEW_CHECK_NONE; }
view_check(A) ::= WITH CHECK OPTION. { A=SQLPARSER_VIEW_CHECK_DEFAULT; }
view_check(A) ::= WITH LOCAL CHECK OPTION. { A=SQLPARSER_VIEW_CHECK_LOCAL; }
view_check(A) ::= WITH CASCADED CHECK OPTION. { A=SQLPARSER_VIEW_CHECK_CASCADED; }
command(A) ::= DROP(T) VIEW if_exists(E) object_name(N). {
  A=FINISH(NODE(DROP_VIEW,.as.drop_object={N,E}),T);
}
%endif
%ifndef SQLITE
command(A) ::= CREATE(T) temporary(U) TABLE if_not_exists(E) object_name(N) mysql_like_table(B). {
  A=FINISH(NODE(CREATE_TABLE,.as.create_table={.table=N,.temporary=U,.if_not_exists=E,.like_table=B}),T);
}
mysql_like_table(A) ::= LIKE object_name(B). { A=B; }
mysql_like_table(A) ::= LP LIKE object_name(B) RP. { A=B; }
command(A) ::= TRUNCATE(T) table_opt object_name(N). { A=FINISH(NODE(TRUNCATE_TABLE,.as.maintenance={N,0}),T); }
table_opt ::= .
table_opt ::= TABLE.
command(A) ::= ALTER(T) TABLE object_name(N) RENAME mysql_rename_to object_name(B). {
  A=FINISH(NODE(ALTER_TABLE,.as.alter={.action=SQLPARSER_RENAME_TABLE,.table=N,.new_name=B}),T);
}
mysql_rename_to ::= .
mysql_rename_to ::= TO.
mysql_rename_to ::= AS.
command(A) ::= ALTER(T) TABLE object_name(N) RENAME COLUMN ident(B) TO ident(C). {
  A=FINISH(NODE(ALTER_TABLE,.as.alter={.action=SQLPARSER_RENAME_COLUMN,.table=N,.column=AT(NAME,B),.new_name=AT(NAME,C)}),T);
}
command(A) ::= ALTER(T) TABLE object_name(N) ENGINE equals charset(B). {
  A=FINISH(NODE(ALTER_TABLE,.as.alter={.action=SQLPARSER_SET_ENGINE,.table=N,.value=B}),T);
}
command(A) ::= ALTER(T) TABLE object_name(N) ALTER mysql_column_opt ident(C) SET DEFAULT mysql_alter_default(V). {
  A=FINISH(NODE(ALTER_TABLE,.as.alter={.action=SQLPARSER_SET_COLUMN_DEFAULT,.table=N,.column=AT(NAME,C),.value=V}),T);
}
command(A) ::= ALTER(T) TABLE object_name(N) ALTER mysql_column_opt ident(C) DROP DEFAULT. {
  A=FINISH(NODE(ALTER_TABLE,.as.alter={.action=SQLPARSER_DROP_COLUMN_DEFAULT,.table=N,.column=AT(NAME,C)}),T);
}
mysql_column_opt ::= .
mysql_column_opt ::= COLUMN.
command(A) ::= ALTER(T) TABLE object_name(N) ADD mysql_column_opt column_definition(C). {
  A=FINISH(NODE(ALTER_TABLE,.as.alter={.action=SQLPARSER_ADD_COLUMN,.table=N,.column=C}),T);
}
command(A) ::= ALTER(T) TABLE object_name(N) ADD mysql_column_opt column_definition(C) FIRST. {
  A=FINISH(NODE(ALTER_TABLE,.as.alter={.action=SQLPARSER_ADD_COLUMN,.table=N,.column=C,.position=SQLPARSER_COLUMN_FIRST}),T);
}
command(A) ::= ALTER(T) TABLE object_name(N) ADD mysql_column_opt column_definition(C) AFTER ident(B). {
  A=FINISH(NODE(ALTER_TABLE,.as.alter={.action=SQLPARSER_ADD_COLUMN,.table=N,.column=C,.position=SQLPARSER_COLUMN_AFTER,.after=AT(NAME,B)}),T);
}
command(A) ::= ALTER(T) TABLE object_name(N) ADD mysql_foreign_key(C). {
  A=FINISH(NODE(ALTER_TABLE,.as.alter={.action=SQLPARSER_ADD_CONSTRAINT,.table=N,.column=C}),T);
}
command(A) ::= ALTER(T) TABLE object_name(N) DROP mysql_column_opt ident(C). {
  A=FINISH(NODE(ALTER_TABLE,.as.alter={.action=SQLPARSER_DROP_COLUMN,.table=N,.column=AT(NAME,C)}),T);
}
mysql_alter_default(A) ::= literal(B). { A=B; }
mysql_alter_default(A) ::= mysql_timestamp(B). { A=B; }
mysql_alter_default(A) ::= MINUS(T) NUMBER(B). { A=NODE(UNARY,.span=COVER(T,B),.as.unary={SQLPARSER_OP_NEGATE,AT(NUMBER,B)}); }
mysql_alter_default(A) ::= PLUS(T) NUMBER(B). { A=NODE(UNARY,.span=COVER(T,B),.as.unary={SQLPARSER_OP_POSITIVE,AT(NUMBER,B)}); }
mysql_alter_default(A) ::= LP expr(B) RP. { A=B; }
mysql_drop_behavior ::= .
mysql_drop_behavior ::= RESTRICT.
mysql_drop_behavior ::= CASCADE.
command(A) ::= DROP(T) temporary(U) TABLE if_exists(E) names(B) mysql_drop_behavior. {
  A=FINISH(NODE(DROP_TABLE,.as.drop_table={B,U,E}),T);
}
%else
command(A) ::= DROP(T) TABLE if_exists(E) object_name(B). {
  A=FINISH(NODE(DROP_TABLE,.as.drop_table={LIST(EMPTY,B),false,E}),T);
}
%endif
%type elements {sqlparser_list}
%ifndef SQLITE
elements(A) ::= element(B). { A=LIST(EMPTY,B); }
elements(A) ::= elements(B) COMMA element(C). { A=LIST(B,C); }
element(A) ::= column_definition(B). { A=B; }
element(A) ::= table_constraint(B). { A=B; }
column_definition(A) ::= object_name(B) type(C) column_constraints(D). {
  A=NODE(COLUMN,.span=COVER(SP(B),SP(D.last?D.last:C)),.as.column={B,C,D});
}
%else
// SQLite requires columns before table constraints. Commas between subsequent
// table constraints are optional and reset the active constraint name.
%type sqlite_columns {sqlparser_list}
sqlite_columns(A) ::= column_definition(B). { A=LIST(EMPTY,B); }
sqlite_columns(A) ::= sqlite_columns(B) COMMA column_definition(C). { A=LIST(B,C); }
elements(A) ::= sqlite_columns(B). { A=B; }
elements(A) ::= sqlite_constraints(B). { A=sqlp_end_constraints(ctx,B).nodes; }
%type sqlite_constraints {sqlp_constraints}
%type constraint_separator {bool}
constraint_separator(A) ::= . { A=false; }
constraint_separator(A) ::= COMMA. { A=true; }
sqlite_constraints(A) ::= sqlite_columns(B) COMMA table_constraint(C). {
  A=sqlp_append_constraint(ctx,sqlp_table_constraints(ctx,B),C);
}
sqlite_constraints(A) ::= sqlite_columns(B) COMMA CONSTRAINT(T) ident(N). {
  A=sqlp_declare_constraint(ctx,sqlp_table_constraints(ctx,B),T,N);
}
sqlite_constraints(A) ::= sqlite_constraints(B) constraint_separator(C) table_constraint(D). {
  A=sqlp_append_constraint(ctx,C?sqlp_end_constraints(ctx,B):B,D);
}
sqlite_constraints(A) ::= sqlite_constraints(B) constraint_separator(C) CONSTRAINT(T) ident(N). {
  A=sqlp_declare_constraint(ctx,C?sqlp_end_constraints(ctx,B):B,T,N);
}
column_definition(A) ::= object_name(B) type(C) column_constraints(D). {
  sqlparser_list constraints=sqlp_end_constraints(ctx,D).nodes;
  A=NODE(COLUMN,.span=COVER(SP(B),SP(constraints.last?constraints.last:C)),.as.column={B,C,constraints});
}
column_definition(A) ::= object_name(B) column_constraints(D). {
  sqlparser_list constraints=sqlp_end_constraints(ctx,D).nodes;
  A=NODE(COLUMN,.span=constraints.last?COVER(SP(B),SP(constraints.last)):SP(B),.as.column={B,0,constraints});
}
%endif
%ifndef SQLITE
type(A) ::= ident(B) type_args(C) type_flags(U). { A=FINISH(NODE(TYPE,.as.type={AT(NAME,B),C,U.is_unsigned,U.zerofill}),B); }
%else
%type type_words {sqlparser_span}
type_words(A) ::= ident(B). { A=B; }
type_words(A) ::= STRING(B). { A=B; }
type_words(A) ::= type_words(B) STRING(C). { A=COVER(B,C); }
type_words(A) ::= type_words(B) ident(C). { A=COVER(B,C); }
type(A) ::= type_words(B) type_args(C). { A=FINISH(NODE(TYPE,.as.type={AT(NAME,B),C,false}),B); }
%endif
%type type_args {sqlparser_list}
type_args(A) ::= . { A=EMPTY; }
type_args(A) ::= LP exprs(B) RP. { A=B; }
%ifndef SQLITE
%type type_flags {sqlp_type_flags}
type_flags(A) ::= . { A=(sqlp_type_flags){0}; }
type_flags(A) ::= type_flags(B) UNSIGNED. { A=B; A.is_unsigned=true; }
type_flags(A) ::= type_flags(B) ZEROFILL. { A=B; A.zerofill=true; }
%endif
%ifndef SQLITE
%type column_constraints {sqlparser_list}
column_constraints(A) ::= . { A=EMPTY; }
column_constraints(A) ::= column_constraints(B) column_constraint(C). { A=LIST(B,C); }
%else
%type column_constraints {sqlp_constraints}
column_constraints(A) ::= . { A=(sqlp_constraints){0}; }
column_constraints(A) ::= column_constraints(B) column_constraint(C). { A=sqlp_append_constraint(ctx,B,C); }
column_constraints(A) ::= column_constraints(B) CONSTRAINT(T) ident(N). { A=sqlp_declare_constraint(ctx,B,T,N); }
%endif
column_constraint(A) ::= NOT(T) NULL conflict_clause(C). { A=FINISH(NODE(CONSTRAINT,.as.constraint={SQLPARSER_NOT_NULL,0,0,0,{0},{0},C}),T); }
column_constraint(A) ::= NULL(T). { A=NODE(CONSTRAINT,.span=T,.as.constraint={SQLPARSER_NULLABLE,0,0,0,{0}}); }
%ifndef SQLITE
column_constraint(A) ::= PRIMARY(T) KEY conflict_clause(C). { A=FINISH(NODE(CONSTRAINT,.as.constraint={SQLPARSER_PRIMARY_KEY,0,0,0,{0},{0},C}),T); }
column_constraint(A) ::= KEY(T) conflict_clause(C). { A=FINISH(NODE(CONSTRAINT,.as.constraint={SQLPARSER_PRIMARY_KEY,0,0,0,{0},{0},C}),T); }
%else
column_constraint(A) ::= PRIMARY(T) KEY direction(D) conflict_clause(C) autoincrement(U). { A=FINISH(NODE(CONSTRAINT,.as.constraint={SQLPARSER_PRIMARY_KEY,0,0,0,{0},{0},C,D,U}),T); }
%type autoincrement {bool}
autoincrement(A) ::= . { A=false; }
autoincrement(A) ::= AUTOINCREMENT. { A=true; }
%endif
column_constraint(A) ::= UNIQUE(T) key_opt conflict_clause(C). { A=FINISH(NODE(CONSTRAINT,.as.constraint={SQLPARSER_UNIQUE,0,0,0,{0},{0},C}),T); }
%ifndef SQLITE
column_constraint(A) ::= AUTO_INCREMENT(T). { A=NODE(CONSTRAINT,.span=T,.as.constraint={SQLPARSER_AUTO_INCREMENT,0,0,0,{0}}); }
column_constraint(A) ::= ON(T) UPDATE mysql_timestamp(B). { A=NODE(CONSTRAINT,.span=COVER(T,SP(B)),.as.constraint={.kind=SQLPARSER_ON_UPDATE,.expression=B}); }
default_value(A) ::= mysql_timestamp(B). { A=B; }
%endif
column_constraint(A) ::= DEFAULT(T) default_value(B). { A=NODE(CONSTRAINT,.span=COVER(T,SP(B)),.as.constraint={SQLPARSER_DEFAULT,0,B,0,{0}}); }
%ifndef SQLITE
%type mysql_reference_actions {sqlparser_reference_options}
%type mysql_reference_action {sqlparser_reference_action}
%type mysql_reference_match {sqlparser_id}
mysql_reference_match(A) ::= . { A=0; }
mysql_reference_match(A) ::= MATCH FULL(B). { A=AT(NAME,B); }
mysql_reference_match(A) ::= MATCH PARTIAL(B). { A=AT(NAME,B); }
mysql_reference_match(A) ::= MATCH SIMPLE(B). { A=AT(NAME,B); }
mysql_reference_actions(A) ::= . { A=(sqlparser_reference_options){0}; }
mysql_reference_actions(A) ::= mysql_reference_actions(B) ON DELETE mysql_reference_action(D). {
  A=B;
  if (A.on_delete != SQLPARSER_REFERENCE_DEFAULT)
    sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"duplicate ON DELETE reference action");
  else A.on_delete=D;
}
mysql_reference_actions(A) ::= mysql_reference_actions(B) ON UPDATE mysql_reference_action(U). {
  A=B;
  if (A.on_update != SQLPARSER_REFERENCE_DEFAULT)
    sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"duplicate ON UPDATE reference action");
  else A.on_update=U;
}
mysql_reference_action(A) ::= RESTRICT. { A=SQLPARSER_REFERENCE_RESTRICT; }
mysql_reference_action(A) ::= CASCADE. { A=SQLPARSER_REFERENCE_CASCADE; }
mysql_reference_action(A) ::= SET NULL. { A=SQLPARSER_REFERENCE_SET_NULL; }
mysql_reference_action(A) ::= NO ACTION. { A=SQLPARSER_REFERENCE_NO_ACTION; }
mysql_reference_action(A) ::= SET DEFAULT. { A=SQLPARSER_REFERENCE_SET_DEFAULT; }
column_constraint(A) ::= REFERENCES(T) object_name(B) LP names(C) RP. {
  A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_REFERENCES,.table=B,.columns=C}),T);
}
%endif
column_constraint(A) ::= CHECK(T) LP expr(B) RP. { A=FINISH(NODE(CONSTRAINT,.as.constraint={SQLPARSER_CHECK,0,B,0,{0}}),T); }
default_value(A) ::= literal(B). { A=B; }
default_value(A) ::= name(B). { A=B; }
default_value(A) ::= name(B) LP RP(E). { A=NODE(CALL,.span=COVER(SP(B),E),.as.call={B,{0},false}); }
default_value(A) ::= MINUS(T) NUMBER(B). { A=NODE(UNARY,.span=COVER(T,B),.as.unary={SQLPARSER_OP_NEGATE,AT(NUMBER,B)}); }
default_value(A) ::= PLUS(T) NUMBER(B). { A=NODE(UNARY,.span=COVER(T,B),.as.unary={SQLPARSER_OP_POSITIVE,AT(NUMBER,B)}); }
default_value(A) ::= LP expr(B) RP. { A=B; }
%ifdef SQLITE
default_value(A) ::= MINUS(T) STRING(B). { A=NODE(UNARY,.span=COVER(T,B),.as.unary={SQLPARSER_OP_NEGATE,AT(STRING,B)}); }
default_value(A) ::= PLUS(T) STRING(B). { A=NODE(UNARY,.span=COVER(T,B),.as.unary={SQLPARSER_OP_POSITIVE,AT(STRING,B)}); }
%endif
%ifndef SQLITE
key_opt ::= . [UNIQUE]
key_opt ::= KEY.
table_unique_opt ::= .
table_unique_opt ::= KEY.
table_unique_opt ::= INDEX.
%else
key_opt ::= .
%endif
%ifndef SQLITE
constraint_name(A) ::= . { A=0; }
constraint_name(A) ::= CONSTRAINT. { A=0; }
constraint_name(A) ::= CONSTRAINT ident(B). { A=AT(NAME,B); }
index_name(A) ::= . { A=0; }
index_name(A) ::= ident(B). { A=AT(NAME,B); }
%endif
%ifndef SQLITE
table_constraint(A) ::= constraint_name(N) PRIMARY(T) KEY LP names(B) RP conflict_clause(C). { A=FINISH(NODE(CONSTRAINT,.as.constraint={SQLPARSER_PRIMARY_KEY,N,0,0,B,{0},C}),T); }
%type mysql_table_keys {sqlp_key_columns}
mysql_table_keys(A) ::= mysql_table_key(B). { A=sqlp_append_key(ctx,(sqlp_key_columns){0},B); }
mysql_table_keys(A) ::= mysql_table_keys(B) COMMA mysql_table_key(C). { A=sqlp_append_key(ctx,B,C); }
mysql_table_key(A) ::= object_name(N) direction(D). { A=FINISH(NODE(ORDER,.as.order={N,D}),SP(N)); }
table_constraint(A) ::= constraint_name(N) UNIQUE(T) table_unique_opt index_name(I) LP mysql_table_keys(B) RP conflict_clause(C). { A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_UNIQUE,.name=I?I:N,.columns=B.names,.conflict=C,.key_terms=B.terms}),T); }
table_constraint(A) ::= INDEX(T) index_name(N) LP mysql_table_keys(B) RP. { A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_INDEX,.name=N,.columns=B.names,.key_terms=B.terms}),T); }
table_constraint(A) ::= KEY(T) index_name(N) LP mysql_table_keys(B) RP. { A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_INDEX,.name=N,.columns=B.names,.key_terms=B.terms}),T); }
table_constraint(A) ::= constraint_name(N) CHECK(T) LP expr(B) RP. { A=FINISH(NODE(CONSTRAINT,.as.constraint={SQLPARSER_CHECK,N,B,0,{0}}),T); }
%type mysql_foreign_key {sqlparser_id}
mysql_foreign_key(A) ::= constraint_name(N) FOREIGN(T) KEY index_name(I) LP names(C) RP REFERENCES object_name(R) LP names(D) RP mysql_reference_match(M) mysql_reference_actions(O). {
  (void)I; O.match=M; A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_FOREIGN_KEY,.name=N,.table=R,
      .columns=C,.referenced_columns=D,.reference=O}),T);
}
table_constraint(A) ::= mysql_foreign_key(B). { A=B; }
%endif
%type table_options {sqlparser_list}
table_options(A) ::= . { A=EMPTY; }
%ifndef SQLITE
%type mysql_index_unique {bool}
%type mysql_index_parts {sqlparser_list}
mysql_index_unique(A) ::= . { A=false; }
mysql_index_unique(A) ::= UNIQUE. { A=true; }
mysql_index_parts(A) ::= mysql_index_part(B). { A=LIST(EMPTY,B); }
mysql_index_parts(A) ::= mysql_index_parts(B) COMMA mysql_index_part(C). { A=LIST(B,C); }
mysql_index_part(A) ::= ident(N) direction(D). {
  A=NODE(INDEX_PART,.span=N,.as.index_part={.column=AT(NAME,N),.descending=D});
}
mysql_index_part(A) ::= ident(N) LP NUMBER(L) RP(E) direction(D). {
  const char *length=sqlparser_text(ctx->document,L);
  for (size_t i=0; i<L.length; ++i) if (length[i]<'0' || length[i]>'9') {
    sqlp_error(ctx,SQLPARSER_SYNTAX_ERROR,"index prefix length requires an unsigned decimal integer"); break;
  }
  A=NODE(INDEX_PART,.span=COVER(N,E),.as.index_part={.column=AT(NAME,N),.length=AT(NUMBER,L),.descending=D});
}
mysql_index_part(A) ::= LP(T) expr(B) RP(E) direction(D). {
  A=NODE(INDEX_PART,.span=COVER(T,E),.as.index_part={.expression=B,.descending=D});
}
command(A) ::= CREATE(T) mysql_index_unique(U) INDEX ident(N) ON object_name(B) LP mysql_index_parts(C) RP. {
  A=FINISH(NODE(CREATE_INDEX,.as.create_index={.name=AT(NAME,N),.table=B,.columns=C,.unique=U}),T);
}
command(A) ::= DROP(T) INDEX ident(N) ON object_name(B). {
  A=FINISH(NODE(DROP_INDEX,.as.drop_object={.name=AT(NAME,N),.table=B}),T);
}
table_options(A) ::= mysql_required_table_options(B). { A=B; }
table_option(A) ::= ENGINE(T) equals charset(B). {
  A=NODE(ASSIGNMENT,.span=COVER(T,SP(B)),.as.assignment={AT(NAME,T),B,SQLPARSER_SCOPE_DEFAULT});
}
table_option(A) ::= default_opt charset_keyword(T) equals charset(B). {
  A=NODE(ASSIGNMENT,.span=COVER(T,SP(B)),.as.assignment={AT(NAME,T),B,SQLPARSER_SCOPE_DEFAULT});
}
table_option(A) ::= default_opt COLLATE(T) equals charset(B). {
  A=NODE(ASSIGNMENT,.span=COVER(T,SP(B)),.as.assignment={AT(NAME,T),B,SQLPARSER_SCOPE_DEFAULT});
}
%type charset_keyword {sqlparser_span}
charset_keyword(A) ::= CHARSET(B). { A=B; }
charset_keyword(A) ::= CHARACTER(B) SET(C). { A=COVER(B,C); }
equals ::= .
equals ::= EQ.
default_opt ::= .
default_opt ::= DEFAULT.
%endif

%type conflict_clause {sqlparser_conflict}
conflict_clause(A) ::= . { A=SQLPARSER_CONFLICT_DEFAULT; }

%ifdef SQLITE
%type conflict_action {sqlparser_conflict}
conflict_action(A) ::= ROLLBACK. { A=SQLPARSER_CONFLICT_ROLLBACK; }
conflict_action(A) ::= ABORT. { A=SQLPARSER_CONFLICT_ABORT; }
conflict_action(A) ::= FAIL. { A=SQLPARSER_CONFLICT_FAIL; }
conflict_action(A) ::= IGNORE. { A=SQLPARSER_CONFLICT_IGNORE; }
conflict_action(A) ::= REPLACE. { A=SQLPARSER_CONFLICT_REPLACE; }
conflict_clause(A) ::= ON CONFLICT conflict_action(B). { A=B; }
insert_op(A) ::= INSERT(T) OR conflict_action(C). { A=(sqlp_insert_head){T,false,C}; }
dml(A) ::= insert_op(R) into object_name(T) insert_columns(C) DEFAULT VALUES. {
  A=FINISH(NODE(INSERT,.as.insert={T,0,C.list,{0},{0},R.replace,R.conflict,true,
    .columns_specified=C.specified}),R.span);
}
%type key_columns {sqlp_key_columns}
key_columns(A) ::= key_column(B). { A=sqlp_append_key(ctx,(sqlp_key_columns){0},B); }
key_columns(A) ::= key_columns(B) COMMA key_column(C). { A=sqlp_append_key(ctx,B,C); }
key_name(A) ::= ident(B). { A=AT(NAME,B); }
key_name(A) ::= STRING(B). { A=AT(NAME,B); }
key_name(A) ::= INDEXED(B). { A=AT(NAME,B); }
key_name(A) ::= join_word(B). { A=AT(NAME,B); }
key_column(A) ::= key_name(B) key_collation(C) direction(D). {
  sqlparser_id expression=C?NODE(COLLATE,.span=COVER(SP(B),SP(C)),.as.collate={B,C}):B;
  A=FINISH(NODE(ORDER,.as.order={expression,D}),SP(B));
}
key_collation(A) ::= . { A=0; }
key_collation(A) ::= COLLATE collation_name(B). { A=AT(NAME,B); }
table_constraint(A) ::= PRIMARY(T) KEY LP key_columns(B) autoincrement(U) RP conflict_clause(C). {
  A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_PRIMARY_KEY,.columns=B.names,.conflict=C,.autoincrement=U,.key_terms=B.terms}),T);
}
table_constraint(A) ::= UNIQUE(T) LP key_columns(B) RP conflict_clause(C). {
  A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_UNIQUE,.columns=B.names,.conflict=C,.key_terms=B.terms}),T);
}
table_constraint(A) ::= CHECK(T) LP expr(B) RP conflict_clause(C). {
  A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_CHECK,.expression=B,.conflict=C}),T);
}
column_constraint(A) ::= COLLATE(T) collation_name(N). { A=NODE(CONSTRAINT,.span=COVER(T,N),.as.constraint={SQLPARSER_COLUMN_COLLATION,AT(NAME,N),0,0,{0}}); }
column_constraint(A) ::= REFERENCES(T) object_name(B) columns(C) reference_actions(R). {
  A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_REFERENCES,.table=B,.columns=C,.reference=R}),T);
}
column_constraint(A) ::= DEFERRABLE(T) initially(B). {
  A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_DEFERRABILITY,.reference={.has_deferrable=true,.deferrable=true,.initially_deferred=B}}),T);
}
column_constraint(A) ::= NOT(T) DEFERRABLE initially(B). {
  A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_DEFERRABILITY,.reference={.has_deferrable=true,.initially_deferred=B}}),T);
}
table_constraint(A) ::= FOREIGN(T) KEY LP names(C) RP REFERENCES object_name(B) columns(D) reference_tail(R). {
  A=FINISH(NODE(CONSTRAINT,.as.constraint={.kind=SQLPARSER_FOREIGN_KEY,.table=B,.columns=C,.referenced_columns=D,.reference=R}),T);
}
%type reference_tail {sqlparser_reference_options}
%type reference_actions {sqlparser_reference_options}
%type reference_defer {sqlparser_reference_options}
%type reference_action {sqlparser_reference_action}
%type initially {bool}
reference_tail(A) ::= reference_actions(B) reference_defer(C). {
  A=B; A.has_deferrable=C.has_deferrable; A.deferrable=C.deferrable; A.initially_deferred=C.initially_deferred;
}
reference_actions(A) ::= . { A=(sqlparser_reference_options){0}; }
reference_actions(A) ::= reference_actions(B) ON DELETE reference_action(C). { A=B; A.on_delete=C; }
reference_actions(A) ::= reference_actions(B) ON UPDATE reference_action(C). { A=B; A.on_update=C; }
reference_actions(A) ::= reference_actions(B) MATCH ident(C). { A=B; A.match=AT(NAME,C); }
reference_actions(A) ::= reference_actions(B) MATCH FULL(C). { A=B; A.match=AT(NAME,C); }
reference_action(A) ::= SET NULL. { A=SQLPARSER_REFERENCE_SET_NULL; }
reference_action(A) ::= SET DEFAULT. { A=SQLPARSER_REFERENCE_SET_DEFAULT; }
reference_action(A) ::= CASCADE. { A=SQLPARSER_REFERENCE_CASCADE; }
reference_action(A) ::= RESTRICT. { A=SQLPARSER_REFERENCE_RESTRICT; }
reference_action(A) ::= NO ACTION. { A=SQLPARSER_REFERENCE_NO_ACTION; }
reference_defer(A) ::= . { A=(sqlparser_reference_options){0}; }
reference_defer(A) ::= DEFERRABLE initially(B). { A=(sqlparser_reference_options){.has_deferrable=true,.deferrable=true,.initially_deferred=B}; }
reference_defer(A) ::= NOT DEFERRABLE initially(B). { A=(sqlparser_reference_options){.has_deferrable=true,.initially_deferred=B}; }
initially(A) ::= . { A=false; }
initially(A) ::= INITIALLY DEFERRED. { A=true; }
initially(A) ::= INITIALLY IMMEDIATE. { A=false; }
command(A) ::= CREATE(T) temporary(U) TABLE if_not_exists(E) object_name(N) AS query(Q). {
  A=FINISH(NODE(CREATE_TABLE,.as.create_table={N,{0},U,E,{0},Q}),T);
}
table_options(A) ::= sqlite_table_options(B). { A=B; }
%type sqlite_table_options {sqlparser_list}
sqlite_table_options(A) ::= sqlite_table_option(B). { A=LIST(EMPTY,B); }
sqlite_table_options(A) ::= sqlite_table_options(B) COMMA sqlite_table_option(C). { A=LIST(B,C); }
sqlite_table_option(A) ::= WITHOUT(T) ROWID(E). { A=NODE(TABLE_OPTION,.span=COVER(T,E),.as.table_option={SQLPARSER_WITHOUT_ROWID}); }
sqlite_table_option(A) ::= STRICT(T). { A=NODE(TABLE_OPTION,.span=T,.as.table_option={SQLPARSER_STRICT}); }

%type transaction_mode {sqlparser_transaction_mode}
transaction_mode(A) ::= . { A=SQLPARSER_TRANSACTION_DEFAULT; }
transaction_mode(A) ::= DEFERRED. { A=SQLPARSER_DEFERRED; }
transaction_mode(A) ::= IMMEDIATE. { A=SQLPARSER_IMMEDIATE; }
transaction_mode(A) ::= EXCLUSIVE. { A=SQLPARSER_EXCLUSIVE; }
transaction_opt ::= .
transaction_opt ::= TRANSACTION.
savepoint_opt ::= .
savepoint_opt ::= SAVEPOINT.
command(A) ::= BEGIN(T) transaction_mode(M) transaction_opt. { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_BEGIN,M,0}),T); }
command(A) ::= COMMIT(T) transaction_opt. { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_COMMIT}),T); }
command(A) ::= END(T) transaction_opt. { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_COMMIT}),T); }
command(A) ::= ROLLBACK(T) transaction_opt. { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_ROLLBACK}),T); }
command(A) ::= SAVEPOINT(T) name(N). { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_SAVEPOINT,0,N}),T); }
command(A) ::= RELEASE(T) savepoint_opt name(N). { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_RELEASE,0,N}),T); }
command(A) ::= ROLLBACK(T) transaction_opt TO savepoint_opt name(N). { A=FINISH(NODE(TRANSACTION,.as.transaction={SQLPARSER_ROLLBACK_TO,0,N}),T); }

pragma_value(A) ::= ident(B). { A=AT(NAME,B); }
pragma_value(A) ::= STRING(B). { A=AT(STRING,B); }
pragma_value(A) ::= NUMBER(B). { A=AT(NUMBER,B); }
pragma_value(A) ::= BOOLEAN(B). { A=AT(BOOLEAN,B); }
pragma_value(A) ::= PLUS(T) NUMBER(B). { A=NODE(UNARY,.span=COVER(T,B),.as.unary={SQLPARSER_OP_POSITIVE,AT(NUMBER,B)}); }
pragma_value(A) ::= MINUS(T) NUMBER(B). { A=NODE(UNARY,.span=COVER(T,B),.as.unary={SQLPARSER_OP_NEGATE,AT(NUMBER,B)}); }
pragma_value(A) ::= ON(B). { A=AT(NAME,B); }
pragma_value(A) ::= DELETE(B). { A=AT(NAME,B); }
pragma_value(A) ::= DEFAULT(B). { A=AT(NAME,B); }
pragma_value(A) ::= EXCLUSIVE(B). { A=AT(NAME,B); }
pragma_value(A) ::= FULL(B). { A=AT(NAME,B); }
command(A) ::= PRAGMA(T) name(N). { A=FINISH(NODE(PRAGMA,.as.pragma={N,0}),T); }
command(A) ::= PRAGMA(T) name(N) EQ pragma_value(V). { A=FINISH(NODE(PRAGMA,.as.pragma={N,V}),T); }
command(A) ::= PRAGMA(T) name(N) LP pragma_value(V) RP. { A=FINISH(NODE(PRAGMA,.as.pragma={N,V}),T); }

%type unique_opt {bool}
unique_opt(A) ::= . { A=false; }
unique_opt(A) ::= UNIQUE. { A=true; }
command(A) ::= CREATE(T) unique_opt(U) INDEX if_not_exists(E) object_name(N) ON object_name(B) LP orders(C) RP where(W). {
  A=FINISH(NODE(CREATE_INDEX,.as.create_index={N,B,W,C,U,E}),T);
}
command(A) ::= DROP(T) INDEX if_exists(E) object_name(N). { A=FINISH(NODE(DROP_INDEX,.as.drop_object={N,E}),T); }
command(A) ::= CREATE(T) temporary(U) VIEW if_not_exists(E) object_name(N) columns(C) AS query(Q). {
  A=FINISH(NODE(CREATE_VIEW,.as.create_view={N,Q,C,U,E}),T);
}
command(A) ::= DROP(T) VIEW if_exists(E) object_name(N). { A=FINISH(NODE(DROP_VIEW,.as.drop_object={N,E}),T); }
database_opt ::= .
database_opt ::= DATABASE.
command(A) ::= ATTACH(T) database_opt expr(F) AS expr(N). { A=FINISH(NODE(ATTACH,.as.attach={F,N}),T); }
command(A) ::= DETACH(T) database_opt expr(N). { A=FINISH(NODE(DETACH,.as.attach={0,N}),T); }

select(A) ::= VALUES(T) rows(R). { A=FINISH(NODE(VALUES,.as.values={R}),T); }
compound(A) ::= compound(B) INTERSECT select(C). {
  A=NODE(UNION,.span=COVER(SP(B),SP(C)),.as.compound={B,C,false,{0},0,SQLPARSER_COMPOUND_INTERSECT});
}
compound(A) ::= compound(B) EXCEPT select(C). {
  A=NODE(UNION,.span=COVER(SP(B),SP(C)),.as.compound={B,C,false,{0},0,SQLPARSER_COMPOUND_EXCEPT});
}

expr(A) ::= expr(B) in_op(N) LP RP(E). [IN] { A=NODE(IN,.span=COVER(SP(B),E),.as.in={B,0,{0},N}); }
%type match_op {sqlparser_operator}
match_op(A) ::= GLOB. { A=SQLPARSER_OP_GLOB; }
match_op(A) ::= NOT GLOB. { A=SQLPARSER_OP_NOT_GLOB; }
match_op(A) ::= REGEXP. { A=SQLPARSER_OP_REGEXP; }
match_op(A) ::= NOT REGEXP. { A=SQLPARSER_OP_NOT_REGEXP; }
match_op(A) ::= MATCH. { A=SQLPARSER_OP_MATCH; }
match_op(A) ::= NOT MATCH. { A=SQLPARSER_OP_NOT_MATCH; }
expr(A) ::= expr(B) match_op(O) expr(C). [LIKE] { A=NODE(BINARY,.span=COVER(SP(B),SP(C)),.as.binary={O,B,C,0}); }

%type update_conflict {sqlparser_conflict}
update_conflict(A) ::= . { A=SQLPARSER_CONFLICT_DEFAULT; }
update_conflict(A) ::= OR conflict_action(B). { A=B; }
command(A) ::= WITH(T) recursive_opt(R) ctes(C) dml(D). {
  A=FINISH(NODE(WITH,.as.with={C,D,R}),T);
}

command(A) ::= ANALYZE(T) maintenance_target(N). { A=FINISH(NODE(ANALYZE,.as.maintenance={N,0}),T); }
command(A) ::= REINDEX(T) maintenance_target(N). { A=FINISH(NODE(REINDEX,.as.maintenance={N,0}),T); }
command(A) ::= VACUUM(T) maintenance_target(N) vacuum_into(F). { A=FINISH(NODE(VACUUM,.as.maintenance={N,F}),T); }
maintenance_target(A) ::= . { A=0; }
maintenance_target(A) ::= object_name(B). { A=B; }
vacuum_into(A) ::= . { A=0; }
vacuum_into(A) ::= INTO expr(B). { A=B; }

command(A) ::= ALTER(T) TABLE object_name(N) RENAME TO object_name(B). { A=FINISH(NODE(ALTER_TABLE,.as.alter={SQLPARSER_RENAME_TABLE,N,0,B}),T); }
command(A) ::= ALTER(T) TABLE object_name(N) RENAME column_opt object_name(B) TO object_name(C). { A=FINISH(NODE(ALTER_TABLE,.as.alter={SQLPARSER_RENAME_COLUMN,N,B,C}),T); }
command(A) ::= ALTER(T) TABLE object_name(N) ADD column_opt column_definition(C). { A=FINISH(NODE(ALTER_TABLE,.as.alter={SQLPARSER_ADD_COLUMN,N,C,0}),T); }
command(A) ::= ALTER(T) TABLE object_name(N) DROP column_opt object_name(C). { A=FINISH(NODE(ALTER_TABLE,.as.alter={SQLPARSER_DROP_COLUMN,N,C,0}),T); }
column_opt ::= .
column_opt ::= COLUMN.

command(A) ::= CREATE(T) VIRTUAL TABLE if_not_exists(E) object_name(N) USING ident(M) module_arguments(B). {
  A=FINISH(NODE(CREATE_VIRTUAL_TABLE,.as.virtual_table={N,AT(NAME,M),B,E}),T);
}
%type module_arguments {sqlparser_list}
%type module_argument_list {sqlparser_list}
%type module_text {sqlparser_span}
%type module_piece {sqlparser_span}
module_arguments(A) ::= . { A=EMPTY; }
module_arguments(A) ::= LP module_argument_list(B) RP. { A=B; }
module_argument_list(A) ::= module_text(B). { A=LIST(EMPTY,AT(MODULE_ARGUMENT,B)); }
module_argument_list(A) ::= module_argument_list(B) COMMA module_text(C). { A=LIST(B,AT(MODULE_ARGUMENT,C)); }
module_text(A) ::= . { A=(sqlparser_span){ctx->token.offset,0}; }
module_text(A) ::= module_text(B) module_piece(C). { A=B.length?COVER(B,C):C; }
module_piece(A) ::= ANY(B). { A=B; }
module_piece(A) ::= LP(B) module_nested RP(C). { A=COVER(B,C); }
module_nested ::= .
module_nested ::= module_nested ANY.
module_nested ::= module_nested LP module_nested RP.

command(A) ::= CREATE(T) temporary(U) TRIGGER if_not_exists(E) object_name(N)
 trigger_timing(F) trigger_event(V) ON object_name(B) trigger_each trigger_when(W)
 BEGIN trigger_steps(S) END. {
  A=FINISH(NODE(CREATE_TRIGGER,.as.trigger={N,B,W,V.columns,S,F,V.kind,U,E}),T);
}
command(A) ::= DROP(T) TRIGGER if_exists(E) object_name(N). { A=FINISH(NODE(DROP_TRIGGER,.as.drop_object={N,E}),T); }
%type trigger_timing {sqlparser_trigger_timing}
trigger_timing(A) ::= . { A=SQLPARSER_TRIGGER_BEFORE; }
trigger_timing(A) ::= BEFORE. { A=SQLPARSER_TRIGGER_BEFORE; }
trigger_timing(A) ::= AFTER. { A=SQLPARSER_TRIGGER_AFTER; }
trigger_timing(A) ::= INSTEAD OF. { A=SQLPARSER_TRIGGER_INSTEAD_OF; }
%type trigger_event {sqlp_trigger_event}
trigger_event(A) ::= INSERT. { A=(sqlp_trigger_event){SQLPARSER_TRIGGER_INSERT,{0}}; }
trigger_event(A) ::= DELETE. { A=(sqlp_trigger_event){SQLPARSER_TRIGGER_DELETE,{0}}; }
trigger_event(A) ::= UPDATE. { A=(sqlp_trigger_event){SQLPARSER_TRIGGER_UPDATE,{0}}; }
trigger_event(A) ::= UPDATE OF names(B). { A=(sqlp_trigger_event){SQLPARSER_TRIGGER_UPDATE,B}; }
trigger_each ::= .
trigger_each ::= FOR EACH ROW.
trigger_when(A) ::= . { A=0; }
trigger_when(A) ::= WHEN expr(B). { A=B; }
%type trigger_steps {sqlparser_list}
trigger_steps(A) ::= trigger_step(B) SEMI. { sqlp_trigger_step(ctx,B); A=LIST(EMPTY,B); }
trigger_steps(A) ::= trigger_steps(B) trigger_step(C) SEMI. { sqlp_trigger_step(ctx,C); A=LIST(B,C); }
trigger_step(A) ::= dml(B). { A=B; }
trigger_step(A) ::= query(B). { A=B; }
expr(A) ::= RAISE(T) LP IGNORE RP(E). { A=NODE(RAISE,.span=COVER(T,E),.as.raise={SQLPARSER_CONFLICT_IGNORE,0}); }
expr(A) ::= RAISE(T) LP raise_action(R) COMMA expr(B) RP(E). { A=NODE(RAISE,.span=COVER(T,E),.as.raise={R,B}); }
%type raise_action {sqlparser_conflict}
raise_action(A) ::= ROLLBACK. { A=SQLPARSER_CONFLICT_ROLLBACK; }
raise_action(A) ::= ABORT. { A=SQLPARSER_CONFLICT_ABORT; }
raise_action(A) ::= FAIL. { A=SQLPARSER_CONFLICT_FAIL; }
%endif
