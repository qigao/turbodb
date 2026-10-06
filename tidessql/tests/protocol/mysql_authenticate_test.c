#include "authenticate.h"
#include "dispatch.h"
#include <gmssl/mem.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static size_t verify_calls,connect_calls,state_calls,close_calls;
static turbodb_status_t verify_failure,connect_failure,state_failure,close_failure;
static tdsql_database *connected_database;
static turbodb_status_t probe_verify(const tdsql_mysql_password_record *record,const uint8_t *bytes,size_t size,
    bool tls,bool *verified,turbodb_error_t *error) {
  ++verify_calls;
  if(verify_failure) return verify_failure;
  return tdsql_mysql_password_verify(record,bytes,size,tls,verified,error);
}
static turbodb_status_t probe_connect(tdsql_database *database,tdsql_connection **out,turbodb_error_t *error) {
  ++connect_calls; connected_database=database;
  if(connect_failure) { *out=NULL; return connect_failure; }
  return tdsql_database_connect(database,out,error);
}
static turbodb_status_t probe_state(const tdsql_connection *connection,tdsql_session_state *out,turbodb_error_t *error) {
  ++state_calls;
  if(state_failure) return state_failure;
  return tdsql_connection_state(connection,out,error);
}
static turbodb_status_t probe_close(tdsql_connection *connection,turbodb_error_t *error) {
  if(connection) ++close_calls;
  if(connection && close_failure) return close_failure;
  return tdsql_connection_close(connection,error);
}
#define tdsql_mysql_password_verify probe_verify
#define tdsql_database_connect probe_connect
#define tdsql_connection_state probe_state
#define tdsql_connection_close probe_close
#include "../../server/mysql/authenticate.c"
#undef tdsql_mysql_password_verify
#undef tdsql_database_connect
#undef tdsql_connection_state
#undef tdsql_connection_close

enum { TEST_DATABASES=2,TEST_ACCOUNTS=3,TEST_BUFFER=2048,TEST_SENTINEL=77,
       TEST_HEADER_BYTES=MYSQL_WIRE_PACKET_HEADER_SIZE,TEST_FULL_SEQUENCE=3,
       TEST_PASSWORD_SEQUENCE=4,TEST_RESULT_SEQUENCE=5 };
static tdsql_database *databases[TEST_DATABASES];
static char *directories[TEST_DATABASES];
static tdsql_connection *first,*second;
static tdsql_result *result;
static tdsql_mysql_account accounts[TEST_ACCOUNTS];
static tdsql_mysql_database_binding bindings[TEST_DATABASES];
static tdsql_mysql_auth_policy policy;
static tdsql_mysql_auth auth,other;
static tdsql_mysql_negotiation gate;
static tdsql_mysql_login login;
static turbodb_error_t error;
static uint8_t payload[TEST_BUFFER],framed[TEST_BUFFER],scratch[TEST_BUFFER];
static tdsql_mysql_output output;
static const uint8_t password[]="test-password";
static tdsql_mysql_password_record verifier(void) {
  tdsql_mysql_password_record record={.iterations=TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS,
    .salt={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    .hash={0xca,0xe9,0xc8,0x01,0x37,0x45,0x96,0xf1,0x7d,0xe4,0x8b,0xa4,0xed,0x70,0x61,0x69,
      0x2b,0x5d,0x0a,0xb4,0x33,0x93,0x2a,0x7d,0x3c,0xdf,0x69,0x8d,0xfd,0x8b,0xb3,0xe8}};
  return record;
}
static void begin(tdsql_mysql_auth *owner,const char *user,const char *database) {
  uint32_t advertised=tdsql_mysql_server_capabilities(true),selected=advertised;
  if(!database) selected&=~MYSQL_WIRE_CLIENT_CONNECT_WITH_DB;
  size_t size=0;
  gate=(tdsql_mysql_negotiation){0}; login=(tdsql_mysql_login){0};
  check_equal(tdsql_mysql_negotiation_init(&gate,advertised),TDSQL_MYSQL_OK);
  check_equal(mysql_wire_build_ssl_request(selected,TEST_BUFFER,TDSQL_MYSQL_BINARY_CHARSET,
      payload,sizeof(payload),&size),MYSQL_WIRE_STATUS_OK);
  check_equal(tdsql_mysql_negotiation_accept(&gate,payload,size,1,false,&login),TDSQL_MYSQL_OK);
  check_equal(tdsql_mysql_negotiation_tls_ready(&gate),TDSQL_MYSQL_OK);
  uint8_t token[TDSQL_MYSQL_AUTH_BYTES]={0};
  check_equal(mysql_wire_build_handshake_response(selected,TEST_BUFFER,TDSQL_MYSQL_BINARY_CHARSET,user,
      token,sizeof(token),database,TDSQL_MYSQL_AUTH_PLUGIN,payload,sizeof(payload),&size),MYSQL_WIRE_STATUS_OK);
  check_equal(tdsql_mysql_negotiation_accept(&gate,payload,size,2,true,&login),TDSQL_MYSQL_OK);
  check_equal(tdsql_mysql_auth_begin(owner,&policy,&gate,&login,&error),TURBODB_STATUS_OK);
  /* No input view retained across input release or asynchronous handoff. */
  memset(payload,0xa5,sizeof(payload));
}
static mysql_wire_bytes_t reply(tdsql_mysql_auth *owner,uint8_t sequence) {
  output=(tdsql_mysql_output){framed,sizeof(framed),0};
  check_equal(tdsql_mysql_auth_emit(owner,&output,&error),TURBODB_STATUS_OK);
  uint32_t size=0; uint8_t actual_sequence=0;
  check_equal(mysql_wire_packet_header_decode(framed,output.size,sequence,TEST_BUFFER,&size,&actual_sequence),MYSQL_WIRE_STATUS_OK);
  check_equal(actual_sequence,sequence); check_equal(output.size,(size_t)size+TEST_HEADER_BYTES);
  return (mysql_wire_bytes_t){framed+TEST_HEADER_BYTES,size};
}
static void request(tdsql_mysql_auth *owner) {
  mysql_wire_bytes_t bytes=reply(owner,TEST_FULL_SEQUENCE); const uint8_t expected[]={1,4};
  check_equal(bytes.length,sizeof(expected)); check_equal(bytes.data,expected,sizeof(expected));
  check_null(owner->connection); check_equal(tdsql_mysql_auth_acknowledge(owner,&error),TURBODB_STATUS_OK);
  check_equal(owner->phase,TDSQL_MYSQL_AUTH_WAIT_PASSWORD);
}
static void authenticate(tdsql_mysql_auth *owner,const char *user,const char *database) {
  begin(owner,user,database); request(owner);
  check_equal(tdsql_mysql_auth_accept(owner,password,sizeof(password),TEST_PASSWORD_SEQUENCE,true,&error),TURBODB_STATUS_OK);
}
static void error_is(tdsql_mysql_auth *owner,uint16_t code,const char *sqlstate) {
  mysql_wire_bytes_t bytes=reply(owner,TEST_RESULT_SEQUENCE); mysql_wire_err_packet_t decoded={0};
  check_equal(mysql_wire_decode_err_packet(bytes.data,bytes.length,gate.header.capabilities,&decoded),MYSQL_WIRE_STATUS_OK);
  check_equal(decoded.error_code,code); check_equal(decoded.sql_state,sqlstate);
  check_equal(tdsql_mysql_auth_acknowledge(owner,&error),TURBODB_STATUS_OK);
  check_equal(owner->phase,TDSQL_MYSQL_AUTH_FAILED);
}
static void success(tdsql_mysql_auth *owner,tdsql_connection **out) {
  mysql_wire_bytes_t bytes=reply(owner,TEST_RESULT_SEQUENCE); mysql_wire_ok_packet_t decoded={0};
  check_equal(mysql_wire_decode_ok_packet(bytes.data,bytes.length,gate.header.capabilities,&decoded),MYSQL_WIRE_STATUS_OK);
  check_equal(decoded.status_flags,TDSQL_MYSQL_SERVER_AUTOCOMMIT); check_equal(decoded.warnings,0u);
  check_equal(decoded.affected_rows,0u); check_equal(decoded.last_insert_id,0u);
  check_equal(tdsql_mysql_auth_acknowledge(owner,&error),TURBODB_STATUS_OK);
  check_equal(tdsql_mysql_auth_take(owner,out,&error),TURBODB_STATUS_OK); check_not_null(*out);
  check_null(owner->connection);
}
static void policy_fails(tdsql_mysql_auth_policy *configuration,turbodb_status_t expected) {
  tdsql_mysql_auth_policy before=policy;
  check_equal(tdsql_mysql_auth_policy_init(&policy,configuration,&error),expected);
  check_equal(&policy,&before,sizeof(before));
}
spec("MySQL account database authorization and session handoff") {
  before_each() {
    first=second=NULL; result=NULL; auth=(tdsql_mysql_auth){0}; other=(tdsql_mysql_auth){0};
    verify_calls=connect_calls=state_calls=close_calls=0;
    verify_failure=connect_failure=state_failure=close_failure=TURBODB_STATUS_OK; connected_database=NULL;
    turbodb_error_init(&error);
    for(size_t i=0;i<TEST_DATABASES;++i) {
      databases[i]=NULL; directories[i]=tt_make_temp_dir("tidessql-auth"); check_not_null(directories[i]);
      const turbodb_option_t options[]={
        {turbodb_view("path"),turbodb_view(directories[i])},
        {turbodb_view("column_family"),turbodb_view("auth")},
        {turbodb_view("sql_initialize"),turbodb_view("true")},
        {turbodb_view("sql_max_connections"),turbodb_view("2")}};
      tdsql_config config=tdsql_config_default(); config.options=options; config.option_count=sizeof(options)/sizeof(options[0]);
      check_equal(tdsql_database_open(&config,&databases[i],&error),TURBODB_STATUS_OK);
    }
    accounts[0]=(tdsql_mysql_account){.username=turbodb_view("alice"),.password=verifier(),.databases=1,.default_database=0};
    accounts[1]=(tdsql_mysql_account){.username=turbodb_view("bob"),.password=verifier(),.databases=2,.default_database=1};
    accounts[2]=(tdsql_mysql_account){.username=turbodb_view("admin"),.password=verifier(),.databases=3,.default_database=0};
    bindings[0]=(tdsql_mysql_database_binding){turbodb_view("tenant_a"),databases[0]};
    bindings[1]=(tdsql_mysql_database_binding){turbodb_view("tenant_b"),databases[1]};
    tdsql_mysql_auth_policy configuration=tdsql_mysql_auth_policy_default();
    configuration.accounts=accounts; configuration.account_count=TEST_ACCOUNTS;
    configuration.databases=bindings; configuration.database_count=TEST_DATABASES;
    check_equal(tdsql_mysql_auth_policy_init(&policy,&configuration,&error),TURBODB_STATUS_OK);
  }
  after_each() {
    /* Terminal faults below occur before touching the real SDK. The test owns
     * a separate cleanup reference; never reset a production quarantine. */
    if(auth.cleanup_failure && auth.connection) {
      check_equal(tdsql_connection_close(auth.connection,&error),TURBODB_STATUS_OK); auth.connection=NULL;
      auth=(tdsql_mysql_auth){0};
    }
    close_failure=TURBODB_STATUS_OK;
    check_equal(tdsql_result_destroy_checked(result,&error),TURBODB_STATUS_OK); result=NULL;
    check_equal(tdsql_mysql_auth_dispose(&auth,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_auth_dispose(&other,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(first,&error),TURBODB_STATUS_OK); first=NULL;
    check_equal(tdsql_connection_close(second,&error),TURBODB_STATUS_OK); second=NULL;
    for(size_t i=0;i<TEST_DATABASES;++i) {
      check_equal(tdsql_database_close(databases[i],&error),TURBODB_STATUS_OK); databases[i]=NULL;
      check_equal(tt_remove_tree(directories[i]),0); free(directories[i]); directories[i]=NULL;
    }
    gmssl_secure_clear(accounts,sizeof(accounts)); gmssl_secure_clear(payload,sizeof(payload));
  }
  it("creates only the explicitly authorized SDK session and moves it after final OK handoff") {
    begin(&auth,"alice","tenant_a"); check_equal(connect_calls,0u); check_equal(verify_calls,0u);
    check_equal(tdsql_mysql_auth_take(&auth,&first,&error),TURBODB_STATUS_INVALID_STATE); check_null(first);
    request(&auth); check_equal(tdsql_mysql_auth_accept(&auth,password,sizeof(password),TEST_PASSWORD_SEQUENCE,true,&error),TURBODB_STATUS_OK);
    check_equal(connect_calls,1u); check_equal(state_calls,1u); check_equal(verify_calls,1u);
    check_true(connected_database==databases[0]); check_not_null(auth.connection);
    check_equal(tdsql_mysql_auth_take(&auth,&first,&error),TURBODB_STATUS_INVALID_STATE); check_null(first);
    success(&auth,&first); check_equal(auth.phase,TDSQL_MYSQL_AUTH_TRANSFERRED);
    check_equal(tdsql_mysql_auth_take(&auth,&second,&error),TURBODB_STATUS_INVALID_STATE); check_null(second);
  }
  it("uses the configured default only when the client omits a database") {
    authenticate(&auth,"bob",NULL); check_true(connected_database==databases[1]); success(&auth,&first);
  }
  it("honors explicit grants for a second configured database") {
    authenticate(&auth,"admin","tenant_b"); check_true(connected_database==databases[1]); success(&auth,&first);
  }
  it("rejects a valid password for an ungranted database without opening a session") {
    authenticate(&auth,"alice","tenant_b"); check_equal(connect_calls,0u); error_is(&auth,1044,"42000");
    check_equal(tdsql_mysql_auth_take(&auth,&first,&error),TURBODB_STATUS_INVALID_STATE); check_null(first);
  }
  it("rejects path-like database names instead of selecting defaults or opening paths") {
    authenticate(&auth,"alice","../../tenant_b"); check_equal(connect_calls,0u); error_is(&auth,1049,"42000");
  }
  it("matches database and account names exactly and case sensitively") {
    authenticate(&auth,"alice","Tenant_a"); check_equal(connect_calls,0u); error_is(&auth,1049,"42000");
    authenticate(&other,"Alice","tenant_a"); check_equal(connect_calls,0u); error_is(&other,1045,"28000");
  }
  it("rejects an unknown account even when the password matches the work-only record") {
    authenticate(&auth,"unknown","tenant_a"); check_equal(verify_calls,1u); check_equal(connect_calls,0u);
    error_is(&auth,1045,"28000");
  }
  it("uses the same generic error for unknown users and wrong passwords without disclosing database existence") {
    begin(&auth,"alice","unknown_db"); request(&auth); const uint8_t wrong[]="wrong-password";
    check_equal(tdsql_mysql_auth_accept(&auth,wrong,sizeof(wrong),TEST_PASSWORD_SEQUENCE,true,&error),TURBODB_STATUS_OK);
    mysql_wire_bytes_t first_error=reply(&auth,TEST_RESULT_SEQUENCE); uint8_t original[TEST_BUFFER];
    memcpy(original,first_error.data,first_error.length); size_t size=first_error.length;
    check_equal(tdsql_mysql_auth_acknowledge(&auth,&error),TURBODB_STATUS_OK);
    authenticate(&other,"unknown","tenant_a"); mysql_wire_bytes_t second_error=reply(&other,TEST_RESULT_SEQUENCE);
    check_equal(second_error.length,size); check_equal(second_error.data,original,size); check_equal(connect_calls,0u);
  }
  it("blocks authentication without a completed TLS login gate") {
    begin(&other,"alice","tenant_a"); gate.phase=TDSQL_MYSQL_WAIT_TLS;
    check_equal(tdsql_mysql_auth_begin(&auth,&policy,&gate,&login,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(auth.phase,0); check_null(auth.connection); check_equal(connect_calls,0u);
  }
  it("terminates wrong password sequence and missing TLS before KDF or SDK work") {
    begin(&auth,"alice","tenant_a"); request(&auth);
    check_equal(tdsql_mysql_auth_accept(&auth,password,sizeof(password),TEST_RESULT_SEQUENCE,true,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(auth.phase,TDSQL_MYSQL_AUTH_FAILED);
    begin(&other,"alice","tenant_a"); request(&other);
    check_equal(tdsql_mysql_auth_accept(&other,password,sizeof(password),TEST_PASSWORD_SEQUENCE,false,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(other.phase,TDSQL_MYSQL_AUTH_FAILED); check_equal(verify_calls,0u); check_equal(connect_calls,0u);
  }
  it("rejects malformed or empty complete passwords as authentication failures") {
    begin(&auth,"alice","tenant_a"); request(&auth); const uint8_t empty[]={0};
    check_equal(tdsql_mysql_auth_accept(&auth,empty,sizeof(empty),TEST_PASSWORD_SEQUENCE,true,&error),TURBODB_STATUS_OK);
    error_is(&auth,1045,"28000"); check_equal(connect_calls,0u);
  }
  it("keeps KDF failure distinct from wrong password and never opens a session") {
    verify_failure=TURBODB_STATUS_INTERNAL_ERROR; authenticate(&auth,"alice","tenant_a");
    check_equal(auth.failure,TURBODB_STATUS_INTERNAL_ERROR); error_is(&auth,1105,"HY000"); check_equal(connect_calls,0u);
  }
  it("does not replay verification or session creation when framed output capacity is insufficient") {
    authenticate(&auth,"alice","tenant_a"); uint8_t original[TEST_BUFFER]; memset(framed,0xa5,sizeof(framed)); memcpy(original,framed,sizeof(original));
    output=(tdsql_mysql_output){framed,TEST_HEADER_BYTES,TEST_SENTINEL};
    check_equal(tdsql_mysql_auth_emit(&auth,&output,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(output.size,(size_t)TEST_SENTINEL); check_equal(framed,original,sizeof(original)); check_false(auth.emitted);
    success(&auth,&first); check_equal(verify_calls,1u); check_equal(connect_calls,1u); check_equal(state_calls,1u);
  }
  it("requires exactly one handoff ACK and forbids measurement from advancing authentication") {
    begin(&auth,"alice","tenant_a");
    check_equal(tdsql_mysql_auth_acknowledge(&auth,&error),TURBODB_STATUS_INVALID_STATE);
    output=(tdsql_mysql_output){.capacity=TEST_BUFFER};
    check_equal(tdsql_mysql_auth_emit(&auth,&output,&error),TURBODB_STATUS_INVALID_ARGUMENT); check_false(auth.emitted);
    (void)reply(&auth,TEST_FULL_SEQUENCE);
    check_equal(tdsql_mysql_auth_emit(&auth,&output,&error),TURBODB_STATUS_BUSY);
    output.data=framed; check_equal(tdsql_mysql_auth_emit(&auth,&output,&error),TURBODB_STATUS_BUSY);
    check_equal(tdsql_mysql_auth_acknowledge(&auth,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_auth_acknowledge(&auth,&error),TURBODB_STATUS_INVALID_STATE); check_equal(connect_calls,0u);
  }
  it("cleans an authenticated but unhanded session on disconnect") {
    authenticate(&auth,"alice","tenant_a"); check_not_null(auth.connection);
    (void)reply(&auth,TEST_RESULT_SEQUENCE);
    check_equal(tdsql_mysql_auth_dispose(&auth,&error),TURBODB_STATUS_OK);
    check_equal(close_calls,1u); check_null(auth.connection); check_equal(auth.phase,0);
    check_equal(tdsql_database_close(databases[0],&error),TURBODB_STATUS_OK); databases[0]=NULL;
  }
  it("allows BUSY cleanup to retain ownership without restoring authentication") {
    authenticate(&auth,"alice","tenant_a"); tdsql_connection *owned=auth.connection; close_failure=TURBODB_STATUS_BUSY;
    check_equal(tdsql_mysql_auth_dispose(&auth,&error),TURBODB_STATUS_BUSY); check_true(auth.connection==owned);
    check_equal(auth.phase,TDSQL_MYSQL_AUTH_FAILED); check_equal(close_calls,1u);
    close_failure=TURBODB_STATUS_OK; check_equal(tdsql_mysql_auth_dispose(&auth,&error),TURBODB_STATUS_OK);
    check_equal(close_calls,2u); check_null(auth.connection);
  }
  it("quarantines non-BUSY close failure without repeating SDK cleanup") {
    authenticate(&auth,"alice","tenant_a"); tdsql_connection *owned=auth.connection; close_failure=TURBODB_STATUS_CLEANUP_FAILED;
    check_equal(tdsql_mysql_auth_dispose(&auth,&error),TURBODB_STATUS_CLEANUP_FAILED); check_true(auth.connection==owned);
    check_equal(tdsql_mysql_auth_dispose(&auth,&error),TURBODB_STATUS_CLEANUP_FAILED); check_equal(close_calls,1u);
    check_equal(tdsql_mysql_auth_take(&auth,&first,&error),TURBODB_STATUS_INVALID_STATE); check_null(first);
  }
  it("reports real shared session quota exhaustion without publishing a connection") {
    check_equal(tdsql_database_connect(databases[0],&first,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_connect(databases[0],&second,&error),TURBODB_STATUS_OK);
    authenticate(&auth,"alice","tenant_a"); check_null(auth.connection); check_equal(connect_calls,1u);
    error_is(&auth,1040,"08004"); check_equal(state_calls,0u);
  }
  it("reports SDK allocation failure as resource failure rather than authentication success") {
    connect_failure=TURBODB_STATUS_OUT_OF_MEMORY; authenticate(&auth,"alice","tenant_a");
    error_is(&auth,1037,"HY001"); check_null(auth.connection); check_equal(state_calls,0u);
  }
  it("retains then cleans a created session if the SDK state query fails") {
    state_failure=TURBODB_STATUS_INTERNAL_ERROR; authenticate(&auth,"alice","tenant_a");
    check_not_null(auth.connection); error_is(&auth,1105,"HY000");
    check_equal(tdsql_mysql_auth_dispose(&auth,&error),TURBODB_STATUS_OK); check_equal(close_calls,1u);
  }
  it("replaces unpublished success if command owner preparation fails") {
    authenticate(&auth,"alice","tenant_a");
    check_equal(tdsql_mysql_auth_reject(&auth,TURBODB_STATUS_OUT_OF_MEMORY,&error),TURBODB_STATUS_OK);
    check_not_null(auth.connection); error_is(&auth,1037,"HY001");
    check_equal(tdsql_mysql_auth_take(&auth,&first,&error),TURBODB_STATUS_INVALID_STATE); check_null(first);
  }
  it("hands two distinct SQL sessions to command owners without sharing mutable SQL state") {
    authenticate(&auth,"alice","tenant_a"); success(&auth,&first);
    authenticate(&other,"alice","tenant_a"); success(&other,&second); check_true(first!=second);
    const tdsql_request request=tdsql_request_default(turbodb_view("SET autocommit=OFF")); uint64_t affected=0;
    check_equal(tdsql_connection_execute(first,&request,&affected,&error),TURBODB_STATUS_OK);
    tdsql_session_state a=tdsql_session_state_default(),b=tdsql_session_state_default();
    check_equal(tdsql_connection_state(first,&a,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_connection_state(second,&b,&error),TURBODB_STATUS_OK);
    check_false(a.autocommit); check_true(b.autocommit);
  }
  it("initializes the actual dispatcher before final OK and transfers its borrowed SDK connection") {
    authenticate(&auth,"alice","tenant_a"); tdsql_mysql_dispatch dispatch={0};
    tdsql_mysql_registry_config config=tdsql_mysql_registry_config_default();
    check_equal(tdsql_mysql_dispatch_init(&dispatch,auth.connection,&config,true,scratch,sizeof(scratch),&error),TURBODB_STATUS_OK);
    success(&auth,&first);
    const uint8_t ping[]={TDSQL_MYSQL_PING};
    check_equal(tdsql_mysql_dispatch_accept(&dispatch,ping,sizeof(ping),1,&error),TURBODB_STATUS_OK);
    int next=0; output=(tdsql_mysql_output){framed,sizeof(framed),0};
    check_equal(tdsql_mysql_dispatch_emit(&dispatch,&output,&next,&error),TURBODB_STATUS_OK);
    check_equal(next,TDSQL_MYSQL_REPLY_PACKET);
    check_equal(tdsql_mysql_dispatch_acknowledge(&dispatch,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_mysql_dispatch_dispose(&dispatch,&error),TURBODB_STATUS_OK);
  }
  it("validates duplicate names null handles and bounded policy spans atomically") {
    tdsql_mysql_auth_policy config=policy; config.max_accounts=0; policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    config=policy; config.max_accounts=TDSQL_MYSQL_MAX_ACCOUNTS+1; policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    config=policy; config.account_count=config.max_accounts+1; policy_fails(&config,TURBODB_STATUS_LIMIT_EXCEEDED);
    config=policy; config.database_count=TDSQL_MYSQL_MAX_DATABASES+1; policy_fails(&config,TURBODB_STATUS_LIMIT_EXCEEDED);
    config=policy; config.accounts=NULL; policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    bindings[1].name=bindings[0].name; config=policy; policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    bindings[1].name=turbodb_view("tenant_b"); bindings[1].database=NULL; policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    bindings[1].database=databases[1]; accounts[1].username=accounts[0].username; policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
  }
  it("rejects invalid default grants unknown bitmap bits and inconsistent work factors") {
    tdsql_mysql_auth_policy config=policy;
    accounts[0].databases=0; policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    accounts[0].databases=UINT64_C(1)<<TEST_DATABASES; policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    accounts[0].databases=1; accounts[0].default_database=1; policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    accounts[0].default_database=TEST_DATABASES; policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    accounts[0].default_database=0; accounts[1].password.iterations=TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS+1;
    policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
  }
  it("rejects empty embedded NUL and overlength configuration names") {
    tdsql_mysql_auth_policy config=policy; accounts[0].username=turbodb_view("");
    policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    const char embedded[]={'a',0,'b'}; accounts[0].username=(vstr){embedded,sizeof(embedded)};
    policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    char large[TDSQL_MYSQL_USERNAME_BYTES+1]; memset(large,'x',sizeof(large)); accounts[0].username=(vstr){large,sizeof(large)};
    policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
    accounts[0].username=turbodb_view("alice"); bindings[0].name=(vstr){embedded,sizeof(embedded)};
    policy_fails(&config,TURBODB_STATUS_INVALID_ARGUMENT);
  }
  it("handles all 64 grant bits without undefined shifts or accepting invalid defaults") {
    tdsql_mysql_database_binding all[TDSQL_MYSQL_MAX_DATABASES]; char names[TDSQL_MYSQL_MAX_DATABASES][16];
    for(size_t i=0;i<TDSQL_MYSQL_MAX_DATABASES;++i) {
      check_greater(snprintf(names[i],sizeof(names[i]),"db_%zu",i),0);
      all[i]=(tdsql_mysql_database_binding){turbodb_view(names[i]),databases[0]};
    }
    tdsql_mysql_account account=accounts[0]; account.databases=UINT64_MAX; account.default_database=TDSQL_MYSQL_MAX_DATABASES-1;
    tdsql_mysql_auth_policy config=tdsql_mysql_auth_policy_default(),validated={0};
    config.accounts=&account; config.account_count=1; config.databases=all; config.database_count=TDSQL_MYSQL_MAX_DATABASES;
    check_equal(tdsql_mysql_auth_policy_init(&validated,&config,&error),TURBODB_STATUS_OK);
    account.default_database=TDSQL_MYSQL_MAX_DATABASES;
    check_equal(tdsql_mysql_auth_policy_init(&validated,&config,&error),TURBODB_STATUS_INVALID_ARGUMENT);
  }
}
