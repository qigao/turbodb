#include "dispatch.h"
#include <tinytest.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { TEST_BUFFER=4096,TEST_UNKNOWN_ID=999,TEST_LARGE=700,
       TEST_ERR_GENERAL=1105,TEST_ERR_ARGUMENT=1210,TEST_ERR_UNKNOWN=1047,
       TEST_ERR_STATEMENT=1243,TEST_ERR_UNSUPPORTED=1235,TEST_SEQUENCE_LAST=255 };
static tdsql_database *database;
static tdsql_connection *first,*second;
static tdsql_mysql_dispatch dispatch;
static tdsql_mysql_registry_config bounds;
static turbodb_error_t error;
static char *directory;
static uint8_t scratch[TEST_BUFFER],output[TEST_BUFFER],request[TEST_BUFFER];
static size_t request_size,packet_size;
static uint8_t expected_sequence;
static uint32_t capabilities;

static tdsql_session_state session(void) {
  tdsql_session_state out=tdsql_session_state_default();
  check_equal(tdsql_connection_state(first,&out,&error),TURBODB_STATUS_OK); return out;
}
static void admit(void) {
  expected_sequence=1;
  check_equal(tdsql_mysql_dispatch_accept(&dispatch,request,request_size,expected_sequence,&error),TURBODB_STATUS_OK);
}
static void query(const char *text) {
  request[0]=TDSQL_MYSQL_QUERY; request_size=strlen(text)+1;
  check_true(request_size<=sizeof(request)); memcpy(request+1,text,request_size-1); admit();
}
static void ack(void) { check_equal(tdsql_mysql_dispatch_acknowledge(&dispatch,&error),TURBODB_STATUS_OK); }
static const uint8_t *packet(void) {
  tdsql_mysql_output out={output,sizeof(output),0}; int reply=0;
  check_equal(tdsql_mysql_dispatch_emit(&dispatch,&out,&reply,&error),TURBODB_STATUS_OK);
  check_equal(reply,TDSQL_MYSQL_REPLY_PACKET);
  uint32_t length=0; uint8_t sequence=0;
  check_equal(mysql_wire_packet_header_decode(output,out.size,expected_sequence,MYSQL_WIRE_PACKET_MAX_PAYLOAD,&length,&sequence),MYSQL_WIRE_STATUS_OK);
  check_equal(out.size,(size_t)length+MYSQL_WIRE_PACKET_HEADER_SIZE); check_equal(sequence,expected_sequence);
  ++expected_sequence; packet_size=length; return output+MYSQL_WIRE_PACKET_HEADER_SIZE;
}
static void complete(int expected) {
  tdsql_mysql_output out={output,sizeof(output),TEST_UNKNOWN_ID}; int reply=0;
  check_equal(tdsql_mysql_dispatch_emit(&dispatch,&out,&reply,&error),TURBODB_STATUS_OK);
  check_equal(reply,expected); check_equal(out.size,TEST_UNKNOWN_ID);
}
static mysql_wire_ok_packet_t ok(void) {
  mysql_wire_ok_packet_t out={0}; const uint8_t *data=packet();
  check_equal(mysql_wire_decode_ok_packet(data,packet_size,capabilities,&out),MYSQL_WIRE_STATUS_OK); ack(); return out;
}
static mysql_wire_err_packet_t failure(uint16_t number,const char *state) {
  mysql_wire_err_packet_t out={0}; const uint8_t *data=packet();
  check_equal(mysql_wire_decode_err_packet(data,packet_size,capabilities,&out),MYSQL_WIRE_STATUS_OK);
  check_equal(out.error_code,number); check_equal(out.sql_state,state,6); check_true(out.message.length>0); ack(); return out;
}
static mysql_column_definition_t column(void) {
  mysql_column_definition_t out={0}; const uint8_t *data=packet();
  check_equal(mysql_wire_decode_column_definition41(data,packet_size,&out),MYSQL_WIRE_STATUS_OK);
  check_equal(out.catalog.data,"def",3); check_true(out.name.length>0); ack(); return out;
}
static uint16_t end(bool final) {
  if(final && dispatch.deprecate_eof) {
    const mysql_wire_ok_packet_t out=ok(); check_equal(out.header,UINT8_C(0xfe)); return out.status_flags;
  }
  mysql_wire_eof_packet_t out={0}; const uint8_t *data=packet();
  check_equal(mysql_wire_decode_eof_packet(data,packet_size,capabilities,&out),MYSQL_WIRE_STATUS_OK); ack(); return out.status_flags;
}
static void metadata(size_t count,mysql_column_definition_t *columns) {
  const uint8_t *data=packet(); size_t offset=0; uint64_t width=0; bool null=false;
  check_equal(mysql_wire_read_lenenc_uint(data,packet_size,&offset,&width,&null),MYSQL_WIRE_STATUS_OK);
  check_equal(width,(uint64_t)count); check_equal(offset,packet_size); check_false(null); ack();
  for(size_t i=0;i<count;++i) columns[i]=column();
  if(!dispatch.deprecate_eof) (void)end(false);
}
static void named_metadata(size_t count,mysql_column_definition_t *columns,const char *const *names) {
  const uint8_t *data=packet(); size_t offset=0; uint64_t width=0; bool null=false;
  check_equal(mysql_wire_read_lenenc_uint(data,packet_size,&offset,&width,&null),MYSQL_WIRE_STATUS_OK);
  check_equal(width,(uint64_t)count); check_equal(offset,packet_size); check_false(null); ack();
  for(size_t i=0;i<count;++i) {
    columns[i]=column(); check_equal(columns[i].name.data,names[i],strlen(names[i]));
  }
  if(!dispatch.deprecate_eof) (void)end(false);
}
static mysql_stmt_prepare_ok_t prepare(const char *text) {
  check_equal(mysql_wire_build_stmt_prepare((const uint8_t *)text,strlen(text),request,sizeof(request),&request_size),MYSQL_WIRE_STATUS_OK); admit();
  mysql_stmt_prepare_ok_t out={0}; const uint8_t *data=packet();
  check_equal(mysql_wire_decode_stmt_prepare_ok(data,packet_size,&out),MYSQL_WIRE_STATUS_OK); ack();
  for(size_t i=0;i<out.parameter_count;++i) (void)column();
  if(out.parameter_count && !dispatch.deprecate_eof) (void)end(false);
  for(size_t i=0;i<out.column_count;++i) (void)column();
  if(out.column_count && !dispatch.deprecate_eof) (void)end(false);
  complete(TDSQL_MYSQL_REPLY_COMPLETE); return out;
}
static void execute(uint32_t id,const mysql_stmt_value_t *values,size_t count) {
  check_equal(mysql_wire_build_stmt_execute(id,values,count,request,sizeof(request),&request_size),MYSQL_WIRE_STATUS_OK); admit();
}
static void scalar_is(int64_t expected) {
  const tdsql_request read=tdsql_request_default(turbodb_view("SELECT score FROM items WHERE id=1"));
  tdsql_result *result=NULL; check_equal(tdsql_connection_query(second,&read,&result,&error),TURBODB_STATUS_OK);
  tdsql_row row={0}; check_equal(tdsql_result_next(result,&row,&error),TURBODB_STATUS_OK);
  check_equal(row.values[0].data.int64_value,expected); check_equal(tdsql_result_destroy_checked(result,&error),TURBODB_STATUS_OK);
}
static void mode(bool deprecated,size_t capacity) {
  check_equal(tdsql_mysql_dispatch_dispose(&dispatch,&error),TURBODB_STATUS_OK);
  capabilities=MYSQL_WIRE_CLIENT_PROTOCOL_41|(deprecated?MYSQL_WIRE_CLIENT_DEPRECATE_EOF:0);
  check_equal(tdsql_mysql_dispatch_init(&dispatch,first,&bounds,deprecated,scratch,capacity,&error),TURBODB_STATUS_OK);
}

spec("MySQL command response chains with the real TidesSQL SDK") {
  before_each() {
    database=NULL; first=second=NULL; dispatch=(tdsql_mysql_dispatch){0}; turbodb_error_init(&error);
    bounds=tdsql_mysql_registry_config_default();
    directory=tt_make_temp_dir("tidessql-mysql-dispatch"); check_not_null(directory);
    const turbodb_option_t options[]={
      {turbodb_view("path"),turbodb_view(directory)},
      {turbodb_view("column_family"),turbodb_view("dispatch")},
      {turbodb_view("sql_initialize"),turbodb_view("true")},
      {turbodb_view("sql_max_recursive_iterations"),turbodb_view("64")}
    };
    tdsql_config config=tdsql_config_default(); config.options=options; config.option_count=sizeof(options)/sizeof(options[0]);
    check_equal(tdsql_database_open(&config,&database,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_connect(database,&first,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_connect(database,&second,&error),TURBODB_STATUS_OK);
    tdsql_request setup=tdsql_request_default(turbodb_view("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)")); uint64_t affected=0;
    check_equal(tdsql_connection_execute(first,&setup,&affected,&error),TURBODB_STATUS_OK);
    setup=tdsql_request_default(turbodb_view("INSERT INTO items VALUES(1,10),(2,20)"));
    check_equal(tdsql_connection_execute(first,&setup,&affected,&error),TURBODB_STATUS_OK); mode(true,sizeof(scratch));
  }
  after_each() {
    check_equal(tdsql_mysql_dispatch_dispose(&dispatch,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(first,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(second,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_close(database,&error),TURBODB_STATUS_OK);
    check_equal(tt_remove_tree(directory),0); free(directory); directory=NULL;
  }
  it("streams text rows and checked-destroys before either negotiated terminator") {
    for(int deprecated=0;deprecated<=1;++deprecated) {
      mode(deprecated!=0,sizeof(scratch)); query("SELECT score AS value FROM items ORDER BY id");
      check_true(session().busy); mysql_column_definition_t columns[1]; metadata(1,columns);
      check_equal(columns[0].type,MYSQL_TYPE_LONGLONG); check_equal(columns[0].name.length,5u);
      const char *expected[]={"10","20"};
      for(size_t i=0;i<2;++i) {
        mysql_wire_bytes_t row[1]; const uint8_t *data=packet();
        check_equal(mysql_wire_decode_text_row(data,packet_size,1,row,1),MYSQL_WIRE_STATUS_OK);
        check_equal(row[0].data,expected[i],2); ack(); check_true(session().busy);
      }
      check_equal(end(true),TDSQL_MYSQL_SERVER_AUTOCOMMIT); check_false(session().busy); check_null(dispatch.response.result);
      complete(TDSQL_MYSQL_REPLY_COMPLETE);
    }
  }
  it("finishes empty results with metadata and no phantom row") {
    query("SELECT score FROM items WHERE id=99"); mysql_column_definition_t columns[1]; metadata(1,columns);
    check_equal(end(true),TDSQL_MYSQL_SERVER_AUTOCOMMIT); check_false(session().busy); complete(TDSQL_MYSQL_REPLY_COMPLETE);
  }
  it("reports warnings raised by lazy row evaluation in the final packet") {
    query("SELECT 1 DIV 0 AS value"); check_equal(session().warning_count,0u);
    mysql_column_definition_t columns[1]; metadata(1,columns); const uint8_t *data=packet();
    mysql_wire_bytes_t row[1]; check_equal(mysql_wire_decode_text_row(data,packet_size,1,row,1),MYSQL_WIRE_STATUS_OK);
    check_true(row[0].is_null); ack(); check_equal(session().warning_count,1u);
    const mysql_wire_ok_packet_t final=ok(); check_equal(final.header,UINT8_C(0xfe)); check_equal(final.warnings,1u);
    check_false(session().busy); complete(TDSQL_MYSQL_REPLY_COMPLETE);
  }
  it("emits real command counts, warning counts and independent transaction flags") {
    query("UPDATE items SET score=25 WHERE id=1"); check_equal(ok().affected_rows,1u); scalar_is(25);
    query("INSERT IGNORE INTO items VALUES(1,99)"); const mysql_wire_ok_packet_t ignored=ok();
    check_equal(ignored.affected_rows,0u); check_equal(ignored.warnings,1u);
    request[0]=TDSQL_MYSQL_PING; request_size=1; admit(); check_equal(ok().warnings,1u);
    query("SET autocommit=OFF"); check_equal(ok().status_flags,0u);
    query("BEGIN"); check_equal(ok().status_flags,TDSQL_MYSQL_SERVER_IN_TRANS);
    query("ROLLBACK"); check_equal(ok().status_flags,0u);
    query("SET autocommit=ON"); check_equal(ok().status_flags,TDSQL_MYSQL_SERVER_AUTOCOMMIT);
    query("SET TRANSACTION READ ONLY"); (void)ok(); query("BEGIN");
    check_equal(ok().status_flags,TDSQL_MYSQL_SERVER_AUTOCOMMIT|TDSQL_MYSQL_SERVER_IN_TRANS|TDSQL_MYSQL_SERVER_IN_TRANS_READONLY);
    query("ROLLBACK"); check_equal(ok().status_flags,TDSQL_MYSQL_SERVER_AUTOCOMMIT);
  }
  it("uses execution metadata for dynamic markers and all native binary value kinds") {
    const mysql_stmt_prepare_ok_t prepared=prepare("SELECT ? AS i,? AS u,? AS d,? AS b,? AS t,? AS x,? AS n");
    check_equal(prepared.parameter_count,7u); check_equal(prepared.column_count,7u);
    const uint8_t blob[]={0,255,1}; const mysql_stmt_value_t values[]={
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=-5},
      {.kind=MYSQL_STMT_VALUE_UINT64,.data.uint64_value=UINT64_MAX},
      {.kind=MYSQL_STMT_VALUE_DOUBLE,.data.double_value=1.5},
      {.kind=MYSQL_STMT_VALUE_BOOL,.data.bool_value=1},
      {.kind=MYSQL_STMT_VALUE_TEXT,.data.bytes={(const uint8_t *)"quoted'",7}},
      {.kind=MYSQL_STMT_VALUE_BLOB,.data.bytes={blob,sizeof(blob)}},
      {.kind=MYSQL_STMT_VALUE_NULL}
    };
    execute(prepared.statement_id,values,7); memset(request,'x',sizeof(request));
    mysql_column_definition_t columns[7]; metadata(7,columns); mysql_binary_value_t row[7]; const uint8_t *data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,7,row,7),MYSQL_WIRE_STATUS_OK);
    check_equal(row[0].data.sint64_value,-5); check_equal(row[1].data.uint64_value,UINT64_MAX);
    check_equal(row[2].data.double_value,1.5); check_equal(row[3].data.sint64_value,1);
    check_equal(row[4].data.bytes.data,"quoted'",7); check_equal(row[5].data.bytes.data,blob,sizeof(blob));
    check_equal(row[6].kind,MYSQL_BINARY_VALUE_NULL); check_equal(columns[5].character_set,63u); ack();
    (void)end(true); complete(TDSQL_MYSQL_REPLY_COMPLETE);
  }
  it("prepares and executes inferred JOIN aggregate markers over the binary protocol") {
    const mysql_stmt_prepare_ok_t prepared=prepare(
        "SELECT a.id,SUM(?) AS total FROM items a JOIN items b ON a.id=b.id+? "
        "GROUP BY a.id HAVING a.id>? ORDER BY a.id");
    check_equal(prepared.parameter_count,3u); check_equal(prepared.column_count,2u);
    const mysql_stmt_value_t values[]={
      {.kind=MYSQL_STMT_VALUE_DOUBLE,.data.double_value=2.5},
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=0},
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=0}
    };
    execute(prepared.statement_id,values,3); mysql_column_definition_t columns[2]; metadata(2,columns);
    mysql_binary_value_t row[2]; const uint8_t *data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,2,row,2),MYSQL_WIRE_STATUS_OK);
    check_equal(row[0].data.sint64_value,1); check_equal(row[1].data.double_value,2.5); ack();
    data=packet(); check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,2,row,2),MYSQL_WIRE_STATUS_OK);
    check_equal(row[0].data.sint64_value,2); check_equal(row[1].data.double_value,2.5); ack();
    (void)end(true); complete(TDSQL_MYSQL_REPLY_COMPLETE);
  }
  it("prepares and executes inferred CTE markers over the binary protocol") {
    const mysql_stmt_prepare_ok_t prepared=prepare(
        "WITH q AS(SELECT score+? AS n FROM items) SELECT n FROM q WHERE n>? ORDER BY n");
    check_equal(prepared.parameter_count,2u); check_equal(prepared.column_count,1u);
    const mysql_stmt_value_t values[]={
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=1},
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=0}
    };
    execute(prepared.statement_id,values,2); mysql_column_definition_t columns[1]; metadata(1,columns);
    mysql_binary_value_t row[1]; const uint8_t *data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,1,row,1),MYSQL_WIRE_STATUS_OK);
    check_equal(row[0].data.sint64_value,11); ack();
    data=packet(); check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,1,row,1),MYSQL_WIRE_STATUS_OK);
    check_equal(row[0].data.sint64_value,21); ack();
    (void)end(true); complete(TDSQL_MYSQL_REPLY_COMPLETE);
  }
  it("prepares cross-query and compound markers over the binary protocol") {
    const mysql_stmt_prepare_ok_t scalar=prepare(
        "SELECT ?+(SELECT id FROM items LIMIT 1) AS n LIMIT ?");
    check_equal(scalar.parameter_count,2u); check_equal(scalar.column_count,1u);
    const mysql_stmt_value_t scalar_values[]={
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=4},
      {.kind=MYSQL_STMT_VALUE_UINT64,.data.uint64_value=1}
    };
    execute(scalar.statement_id,scalar_values,2); mysql_column_definition_t columns[1];
    metadata(1,columns); mysql_binary_value_t row[1]; const uint8_t *data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,1,row,1),MYSQL_WIRE_STATUS_OK);
    check_equal(row[0].data.sint64_value,5); ack(); (void)end(true);
    complete(TDSQL_MYSQL_REPLY_COMPLETE);

    const mysql_stmt_prepare_ok_t compound=prepare(
        "SELECT ?+1 AS n UNION ALL SELECT score+? AS n FROM items LIMIT ?");
    check_equal(compound.parameter_count,3u); check_equal(compound.column_count,1u);
    const mysql_stmt_value_t compound_values[]={
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=4},
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=2},
      {.kind=MYSQL_STMT_VALUE_UINT64,.data.uint64_value=2}
    };
    execute(compound.statement_id,compound_values,3); metadata(1,columns); data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,1,row,1),MYSQL_WIRE_STATUS_OK);
    check_equal(row[0].data.sint64_value,5); ack(); data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,1,row,1),MYSQL_WIRE_STATUS_OK);
    check_equal(row[0].data.sint64_value,12); ack(); (void)end(true);
    complete(TDSQL_MYSQL_REPLY_COMPLETE);

    const mysql_stmt_prepare_ok_t propagated=prepare(
        "SELECT ? AS n UNION ALL SELECT score AS n FROM items LIMIT ?");
    check_equal(propagated.parameter_count,2u); check_equal(propagated.column_count,1u);
    const mysql_stmt_value_t propagated_values[]={
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=7},
      {.kind=MYSQL_STMT_VALUE_UINT64,.data.uint64_value=2}
    };
    execute(propagated.statement_id,propagated_values,2); metadata(1,columns); data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,1,row,1),MYSQL_WIRE_STATUS_OK);
    check_equal(row[0].data.sint64_value,7); ack(); data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,1,row,1),MYSQL_WIRE_STATUS_OK);
    check_equal(row[0].data.sint64_value,10); ack(); (void)end(true);
    complete(TDSQL_MYSQL_REPLY_COMPLETE);

    const mysql_stmt_prepare_ok_t recursive=prepare(
        "WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT n+? FROM c WHERE n<? LIMIT ?) "
        "SELECT n+? AS n FROM c ORDER BY n");
    check_equal(recursive.parameter_count,4u); check_equal(recursive.column_count,1u);
    const mysql_stmt_value_t recursive_values[]={
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=1},
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=100},
      {.kind=MYSQL_STMT_VALUE_UINT64,.data.uint64_value=3},
      {.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=10}
    };
    execute(recursive.statement_id,recursive_values,4); metadata(1,columns);
    for(int64_t expected=11;expected<=13;++expected) {
      data=packet();
      check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,1,row,1),MYSQL_WIRE_STATUS_OK);
      check_equal(row[0].data.sint64_value,expected); ack();
    }
    (void)end(true); complete(TDSQL_MYSQL_REPLY_COMPLETE);
  }
  it("prepares and executes SHOW and EXPLAIN over the binary protocol") {
    const mysql_stmt_prepare_ok_t shown=prepare("SHOW TABLES");
    check_equal(shown.parameter_count,0u); check_equal(shown.column_count,1u);
    execute(shown.statement_id,NULL,0); mysql_column_definition_t show_columns[1];
    const char *const show_names[]={"Tables_in_dispatch"}; named_metadata(1,show_columns,show_names);
    mysql_binary_value_t show_row[1]; const uint8_t *data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,show_columns,1,show_row,1),MYSQL_WIRE_STATUS_OK);
    check_equal(show_row[0].data.bytes.data,"items",5); ack(); (void)end(true); complete(TDSQL_MYSQL_REPLY_COMPLETE);

    const mysql_stmt_prepare_ok_t filtered=prepare("SHOW TABLES WHERE `Tables_in_dispatch` LIKE ? "
        "AND `Tables_in_dispatch` IN (?,?) AND NOT ? AND CASE WHEN ? THEN TRUE ELSE FALSE END "
        "AND COALESCE(?,FALSE)");
    check_equal(filtered.parameter_count,6u); check_equal(filtered.column_count,1u);
    const mysql_stmt_value_t table[]={
      {.kind=MYSQL_STMT_VALUE_TEXT,.data.bytes={(const uint8_t *)"it%",3}},
      {.kind=MYSQL_STMT_VALUE_TEXT,.data.bytes={(const uint8_t *)"items",5}},
      {.kind=MYSQL_STMT_VALUE_TEXT,.data.bytes={(const uint8_t *)"other",5}},
      {.kind=MYSQL_STMT_VALUE_BOOL,.data.bool_value=0},
      {.kind=MYSQL_STMT_VALUE_BOOL,.data.bool_value=1},
      {.kind=MYSQL_STMT_VALUE_BOOL,.data.bool_value=1}
    };
    execute(filtered.statement_id,table,6); mysql_column_definition_t filtered_columns[1];
    named_metadata(1,filtered_columns,show_names); mysql_binary_value_t filtered_row[1]; data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,filtered_columns,1,filtered_row,1),MYSQL_WIRE_STATUS_OK);
    check_equal(filtered_row[0].data.bytes.data,"items",5); ack(); (void)end(true); complete(TDSQL_MYSQL_REPLY_COMPLETE);

    const mysql_stmt_prepare_ok_t explained=prepare("EXPLAIN SELECT score FROM items WHERE id=?");
    check_equal(explained.parameter_count,1u); check_equal(explained.column_count,12u);
    const mysql_stmt_value_t id={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=1};
    execute(explained.statement_id,&id,1); mysql_column_definition_t explain_columns[12];
    const char *const explain_names[]={
      "id","select_type","table","partitions","type","possible_keys","key","key_len","ref","rows","filtered","Extra"
    };
    named_metadata(12,explain_columns,explain_names);
    mysql_binary_value_t explain_row[12]; data=packet();
    check_equal(mysql_wire_decode_binary_row(data,packet_size,explain_columns,12,explain_row,12),MYSQL_WIRE_STATUS_OK);
    ack(); (void)end(true); complete(TDSQL_MYSQL_REPLY_COMPLETE);
  }
  it("follows both PREPARE metadata chains and RESET/CLOSE no-response semantics") {
    for(int deprecated=0;deprecated<=1;++deprecated) {
      mode(deprecated!=0,sizeof(scratch)); const mysql_stmt_prepare_ok_t prepared=prepare("SELECT score FROM items WHERE id=?");
      check_equal(prepared.parameter_count,1u); check_equal(prepared.column_count,1u);
      const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=1};
      execute(prepared.statement_id,&value,1); mysql_column_definition_t columns[1]; metadata(1,columns);
      mysql_binary_value_t row[1]; const uint8_t *data=packet();
      check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,1,row,1),MYSQL_WIRE_STATUS_OK);
      check_equal(row[0].data.sint64_value,10); ack(); (void)end(true);
      check_equal(mysql_wire_build_stmt_reset(prepared.statement_id,request,sizeof(request),&request_size),MYSQL_WIRE_STATUS_OK); admit(); (void)ok();
      enum { TYPE_FLAG_OFFSET=11,TYPE_OFFSET=12,VALUE_OFFSET=14 };
      check_equal(mysql_wire_build_stmt_execute(prepared.statement_id,&value,1,request,sizeof(request),&request_size),MYSQL_WIRE_STATUS_OK);
      request[TYPE_FLAG_OFFSET]=0; memmove(request+TYPE_OFFSET,request+VALUE_OFFSET,request_size-VALUE_OFFSET); request_size-=TDSQL_MYSQL_TYPE_BYTES;
      admit(); metadata(1,columns); data=packet();
      check_equal(mysql_wire_decode_binary_row(data,packet_size,columns,1,row,1),MYSQL_WIRE_STATUS_OK);
      check_equal(row[0].data.sint64_value,10); ack(); (void)end(true);
      check_equal(mysql_wire_build_stmt_close(prepared.statement_id,request,sizeof(request),&request_size),MYSQL_WIRE_STATUS_OK); admit();
      complete(TDSQL_MYSQL_REPLY_COMPLETE); check_equal(tdsql_mysql_registry_count(&dispatch.registry),0u);
      check_equal(mysql_wire_build_stmt_close(TEST_UNKNOWN_ID,request,sizeof(request),&request_size),MYSQL_WIRE_STATUS_OK); admit(); complete(TDSQL_MYSQL_REPLY_COMPLETE);
    }
  }
  it("handles zero-parameter prepared DDL and DML with command OK only") {
    const mysql_stmt_prepare_ok_t ddl=prepare("CREATE TABLE extra(id BIGINT PRIMARY KEY)");
    check_equal(ddl.parameter_count,0u); check_equal(ddl.column_count,0u); execute(ddl.statement_id,NULL,0); (void)ok();
    const mysql_stmt_prepare_ok_t update=prepare("UPDATE items SET score=? WHERE id=1");
    check_equal(update.column_count,0u); const mysql_stmt_value_t value={.kind=MYSQL_STMT_VALUE_SINT64,.data.sint64_value=25};
    execute(update.statement_id,&value,1); check_equal(ok().affected_rows,1u); complete(TDSQL_MYSQL_REPLY_COMPLETE); scalar_is(25);
  }
  it("returns one ERR for invalid SQL, multiple statements and unknown IDs without SQL effects") {
    query("UPDATE items SET score=99 WHERE id=1; DELETE FROM items"); (void)failure(TEST_ERR_GENERAL,"HY000"); scalar_is(10);
    query("SELECT FROM"); (void)failure(TEST_ERR_GENERAL,"HY000"); complete(TDSQL_MYSQL_REPLY_COMPLETE);
    query("CREATE VIEW v AS SELECT score FROM items"); (void)failure(TEST_ERR_UNSUPPORTED,"42000");
    check_equal(mysql_wire_build_stmt_reset(TEST_UNKNOWN_ID,request,sizeof(request),&request_size),MYSQL_WIRE_STATUS_OK); admit();
    (void)failure(TEST_ERR_STATEMENT,"HY000");
    execute(TEST_UNKNOWN_ID,NULL,0); (void)failure(TEST_ERR_STATEMENT,"HY000");
    check_equal(mysql_wire_build_stmt_execute(TEST_UNKNOWN_ID,NULL,0,request,sizeof(request),&request_size),MYSQL_WIRE_STATUS_OK);
    enum { EXECUTE_FLAGS_OFFSET=5 }; request[EXECUTE_FLAGS_OFFSET]=1; admit(); (void)failure(TEST_ERR_UNSUPPORTED,"42000");
    request[0]=UINT8_C(0x7f); request_size=1; admit(); (void)failure(TEST_ERR_UNKNOWN,"08S01");
    request[0]=TDSQL_MYSQL_PING; request[1]=0; request_size=2; admit(); (void)failure(TEST_ERR_ARGUMENT,"HY000");
    request_size=1; admit(); (void)ok();
  }
  it("keeps the consumed row pending on frame overflow and rejects command or emit reentry") {
    query("SELECT score FROM items ORDER BY id"); mysql_column_definition_t columns[1]; metadata(1,columns);
    memset(output,UINT8_C(0xa5),sizeof(output)); const uint8_t saved=output[0];
    tdsql_mysql_output too_small={output,MYSQL_WIRE_PACKET_HEADER_SIZE,TEST_UNKNOWN_ID}; int reply=TEST_UNKNOWN_ID;
    check_equal(tdsql_mysql_dispatch_emit(&dispatch,&too_small,&reply,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(too_small.size,TEST_UNKNOWN_ID); check_equal(reply,TEST_UNKNOWN_ID); check_equal(output[0],saved);
    check_equal(tdsql_mysql_dispatch_acknowledge(&dispatch,&error),TURBODB_STATUS_INVALID_STATE);
    request[0]=TDSQL_MYSQL_PING; request_size=1;
    check_equal(tdsql_mysql_dispatch_accept(&dispatch,request,request_size,1,&error),TURBODB_STATUS_BUSY);
    mysql_wire_bytes_t row[1]; const uint8_t *data=packet();
    check_equal(mysql_wire_decode_text_row(data,packet_size,1,row,1),MYSQL_WIRE_STATUS_OK); check_equal(row[0].data,"10",2);
    tdsql_mysql_output repeated={output,sizeof(output),0};
    check_equal(tdsql_mysql_dispatch_emit(&dispatch,&repeated,&reply,&error),TURBODB_STATUS_BUSY); ack();
    data=packet(); check_equal(mysql_wire_decode_text_row(data,packet_size,1,row,1),MYSQL_WIRE_STATUS_OK); check_equal(row[0].data,"20",2); ack();
    (void)end(true); complete(TDSQL_MYSQL_REPLY_COMPLETE);
  }
  it("does not replay committed commands when frame capacity is retried") {
    query("UPDATE items SET score=score+1 WHERE id=1");
    tdsql_mysql_output small={output,1,0}; int reply=0;
    check_equal(tdsql_mysql_dispatch_emit(&dispatch,&small,&reply,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    scalar_is(11); check_equal(ok().affected_rows,1u); scalar_is(11);
  }
  it("checks row scratch limits, releases results and allows a subsequent PING") {
    mode(true,TDSQL_MYSQL_MIN_REPLY_BYTES);
    char large[TEST_LARGE+1]; memset(large,'a',TEST_LARGE); large[TEST_LARGE]=0;
    char text[TEST_BUFFER]; (void)snprintf(text,sizeof(text),"SELECT '%s' AS value",large); query(text);
    mysql_column_definition_t columns[1]; metadata(1,columns); (void)failure(TEST_ERR_GENERAL,"HY000");
    check_false(session().busy); check_null(dispatch.response.result); complete(TDSQL_MYSQL_REPLY_COMPLETE);
    request[0]=TDSQL_MYSQL_PING; request_size=1; admit(); (void)ok();
  }
  it("returns a late ERR on the SDK row limit without losing the next command") {
    bounds.limits.max_result_rows=1; mode(true,sizeof(scratch));
    query("SELECT score FROM items ORDER BY id"); mysql_column_definition_t columns[1]; metadata(1,columns);
    (void)packet(); ack(); (void)failure(TEST_ERR_GENERAL,"HY000");
    check_false(session().busy); complete(TDSQL_MYSQL_REPLY_COMPLETE);
    request[0]=TDSQL_MYSQL_PING; request_size=1; admit(); (void)ok();
  }
  it("preserves the SDK alias limit before publishing PREPARE metadata or a result count") {
    mode(true,TDSQL_MYSQL_MIN_REPLY_BYTES);
    char name[TEST_LARGE+1]; memset(name,'a',TEST_LARGE); name[TEST_LARGE]=0;
    char text[TEST_BUFFER]; (void)snprintf(text,sizeof(text),"SELECT 1 AS `%s`",name);
    check_equal(mysql_wire_build_stmt_prepare((const uint8_t *)text,strlen(text),request,sizeof(request),&request_size),MYSQL_WIRE_STATUS_OK); admit();
    (void)failure(TEST_ERR_GENERAL,"HY000"); check_equal(tdsql_mysql_registry_count(&dispatch.registry),0u);
    check_equal(dispatch.registry.last_id,0u);
    query(text); (void)failure(TEST_ERR_GENERAL,"HY000"); check_false(session().busy);
  }
  it("wraps packet sequence numbers and resets them for each complete command") {
    request[0]=TDSQL_MYSQL_QUERY; const char text[]="SELECT 1 AS value";
    memcpy(request+1,text,sizeof(text)-1); request_size=sizeof(text); expected_sequence=TEST_SEQUENCE_LAST;
    check_equal(tdsql_mysql_dispatch_accept(&dispatch,request,request_size,expected_sequence,&error),TURBODB_STATUS_OK);
    mysql_column_definition_t columns[1]; metadata(1,columns); const uint8_t *data=packet(); const uint8_t expected[]={1,'1'};
    check_equal(packet_size,sizeof(expected)); check_equal(data,expected,sizeof(expected)); ack(); (void)end(true);
    request[0]=TDSQL_MYSQL_PING; request_size=1; admit(); (void)ok();
  }
  it("rolls back a disconnected session after checked release of an unfinished result") {
    query("BEGIN"); (void)ok(); query("UPDATE items SET score=25 WHERE id=1"); (void)ok();
    query("SELECT score FROM items"); (void)packet(); ack();
    check_equal(tdsql_mysql_dispatch_dispose(&dispatch,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(first,&error),TURBODB_STATUS_OK); first=NULL; scalar_is(10);
  }
  it("keeps QUIT and unsupported long-data commands terminal without an unexpected packet") {
    request[0]=TDSQL_MYSQL_QUIT; request_size=1; admit(); complete(TDSQL_MYSQL_REPLY_DISCONNECT);
    check_equal(tdsql_mysql_dispatch_accept(&dispatch,request,request_size,1,&error),TURBODB_STATUS_INVALID_STATE);
    mode(true,sizeof(scratch)); request[0]=UINT8_C(0x18); request_size=1;
    check_equal(tdsql_mysql_dispatch_accept(&dispatch,request,request_size,1,&error),TURBODB_STATUS_UNSUPPORTED);
    complete(TDSQL_MYSQL_REPLY_DISCONNECT);
    mode(true,sizeof(scratch)); request[0]=MYSQL_COM_STMT_CLOSE;
    check_equal(tdsql_mysql_dispatch_accept(&dispatch,request,request_size,1,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    complete(TDSQL_MYSQL_REPLY_DISCONNECT);
  }
}
