#include "server.h"
#include <session_async.h>
#include <salts/clock.h>
#include <gmssl/mem.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_TIMEOUT_MS=10000,TEST_IO_TIMEOUT_MS=2000,TEST_BUFFER_BYTES=65536,
       TEST_QUEUE=32,TEST_REQUESTS=16,TEST_CONNECTIONS=2 };
static const uint8_t query[]="SELECT 42 AS n";
static const char password[]="test-password";
static tdsql_database *database;
static char *directory;
static tdsql_mysql_account account;
static tdsql_mysql_database_binding binding;
static tdsql_mysql_auth_policy policy;
static tdsql_mysql_server server;
static tdsql_mysql_server_config server_config;
static mysql_async_source source,second_source;
static turbodb_error_t error;
static mysql_session_error_t client_error;

static tdsql_mysql_password_record verifier(void) {
  return (tdsql_mysql_password_record){.iterations=TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS,
    .salt={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    .hash={0xca,0xe9,0xc8,0x01,0x37,0x45,0x96,0xf1,0x7d,0xe4,0x8b,0xa4,0xed,0x70,0x61,0x69,
      0x2b,0x5d,0x0a,0xb4,0x33,0x93,0x2a,0x7d,0x3c,0xdf,0x69,0x8d,0xfd,0x8b,0xb3,0xe8}};
}
static cnet_client_config transport_config(void) {
  return (cnet_client_config){.backend=
#if defined(_WIN32)
    NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
    NATIVE_IO_BACKEND_EPOLL,
#else
    NATIVE_IO_BACKEND_KQUEUE,
#endif
    .connection_capacity=TEST_CONNECTIONS,.command_capacity=TEST_QUEUE,
    .request_capacity=TEST_REQUESTS,.completion_batch_capacity=TEST_REQUESTS,
    .event_capacity=TEST_QUEUE,.max_send_bytes=TEST_BUFFER_BYTES,
    .receive_buffer_bytes=TEST_BUFFER_BYTES,.connect_timeout_ms=TEST_IO_TIMEOUT_MS,
    .read_timeout_ms=TEST_IO_TIMEOUT_MS,.write_timeout_ms=TEST_IO_TIMEOUT_MS,
    .tls_io_buffer_bytes=CNET_TLS_MIN_IO_BUFFER_BYTES,
    .tls_handshake_timeout_ms=TEST_IO_TIMEOUT_MS};
}
static void close_client_and_drain_server(void) {
  if(source.context) check_equal(mysql_session_async_close(&source,&client_error),MYSQL_SESSION_OK);
  if(second_source.context) check_equal(mysql_session_async_close(&second_source,&client_error),MYSQL_SESSION_OK);
  const uint64_t deadline=salts_monotonic_ms()+TEST_TIMEOUT_MS;
  while(server.initialized && server.active && salts_monotonic_ms()<deadline) {
    size_t events=0;
    check_equal(tdsql_mysql_server_poll(&server,1,&events,&error),TURBODB_STATUS_OK);
  }
  if(server.initialized) check_equal(server.active,0u);
}
static void start_client_until_accepted(void) {
  uint16_t port=0; check_equal(tdsql_mysql_server_port(&server,&port,&error),TURBODB_STATUS_OK);
  mysql_session_config_t client={.host="127.0.0.1",.port=port,.username="alice",
    .password=password,.database="tenant",.ca_file=TEST_TLS_CA,.server_name="localhost",
    .timeout_ms=TEST_IO_TIMEOUT_MS};
  mysql_session_cursor_limits_t limits={.max_result_rows=8,.max_columns=8,
    .max_metadata_bytes=TEST_BUFFER_BYTES,.max_row_bytes=TEST_BUFFER_BYTES,
    .max_command_bytes=TEST_BUFFER_BYTES};
  check_equal(mysql_session_start_async_source(&client,query,sizeof(query)-1,NULL,0,&limits,
    &source,&client_error),MYSQL_SESSION_OK);
  const uint64_t deadline=salts_monotonic_ms()+TEST_TIMEOUT_MS;
  while(server.active==0 && salts_monotonic_ms()<deadline) {
    const mysql_async_step step=mysql_session_async_next(&source);
    check_false(step.kind==MYSQL_ASYNC_ERROR);
    size_t events=0; check_equal(tdsql_mysql_server_poll(&server,1,&events,&error),TURBODB_STATUS_OK);
  }
  check_equal(server.active,1u); check_equal(server.accepted,UINT64_C(1));
}
spec("TidesSQL MySQL server existing client end to end") {
  before_each() {
    database=NULL; directory=tt_make_temp_dir("tidessql-mysql-server"); check_not_null(directory);
    server=(tdsql_mysql_server){0}; source=(mysql_async_source){0}; second_source=(mysql_async_source){0};
    turbodb_error_init(&error); memset(&client_error,0,sizeof(client_error));
    const turbodb_option_t options[]={
      {turbodb_view("path"),turbodb_view(directory)},
      {turbodb_view("column_family"),turbodb_view("server")},
      {turbodb_view("sql_initialize"),turbodb_view("true")}};
    tdsql_config database_config=tdsql_config_default(); database_config.options=options;
    database_config.option_count=sizeof(options)/sizeof(options[0]);
    check_equal(tdsql_database_open(&database_config,&database,&error),TURBODB_STATUS_OK);
    account=(tdsql_mysql_account){.username=turbodb_view("alice"),.password=verifier(),
      .databases=UINT64_C(1),.default_database=0};
    binding=(tdsql_mysql_database_binding){turbodb_view("tenant"),database};
    tdsql_mysql_auth_policy requested=tdsql_mysql_auth_policy_default();
    requested.accounts=&account; requested.account_count=1; requested.databases=&binding;
    requested.database_count=1;
    check_equal(tdsql_mysql_auth_policy_init(&policy,&requested,&error),TURBODB_STATUS_OK);
    server_config=(tdsql_mysql_server_config){.policy=&policy,.registry=tdsql_mysql_registry_config_default(),
      .server_version=turbodb_view("TidesSQL-test"),
      .listener={.backend=transport_config().backend,.host="127.0.0.1",.port=0,.backlog=TEST_CONNECTIONS},
      .transport=transport_config(),
      .tls={.size=sizeof(cnet_tls_server_config),.cert_file=TEST_TLS_CERT,.key_file=TEST_TLS_KEY},
      .max_connections=TEST_CONNECTIONS,.input_bytes=TEST_BUFFER_BYTES,
      .scratch_bytes=TEST_BUFFER_BYTES,.output_bytes=TEST_BUFFER_BYTES};
    check_equal(tdsql_mysql_server_init(&server,&server_config,&error),TURBODB_STATUS_OK);
  }
  after_each() {
    close_client_and_drain_server();
    check_equal(tdsql_mysql_server_stop(&server,TEST_IO_TIMEOUT_MS,&error),TURBODB_STATUS_OK);
    check_equal(tdsql_database_close(database,&error),TURBODB_STATUS_OK); database=NULL;
    check_equal(tt_remove_tree(directory),0); free(directory); directory=NULL;
    gmssl_secure_clear(&account,sizeof(account));
  }
  it("runs TLS full authentication prepare execute metadata row and close") {
    uint16_t port=0; check_equal(tdsql_mysql_server_port(&server,&port,&error),TURBODB_STATUS_OK);
    check_greater(port,0u);
    mysql_session_config_t client={.host="127.0.0.1",.port=port,.username="alice",
      .password=password,.database="tenant",.ca_file=TEST_TLS_CA,.server_name="localhost",
      .timeout_ms=TEST_IO_TIMEOUT_MS};
    mysql_session_cursor_limits_t limits={.max_result_rows=8,.max_columns=8,
      .max_metadata_bytes=TEST_BUFFER_BYTES,.max_row_bytes=TEST_BUFFER_BYTES,
      .max_command_bytes=TEST_BUFFER_BYTES};
    check_equal(mysql_session_start_async_source(&client,query,sizeof(query)-1,NULL,0,&limits,
      &source,&client_error),MYSQL_SESSION_OK);
    bool metadata=false,row=false,done=false; size_t columns=0,row_size=0;
    const uint64_t deadline=salts_monotonic_ms()+TEST_TIMEOUT_MS;
    while(!done && salts_monotonic_ms()<deadline) {
      size_t events=0;
      check_equal(tdsql_mysql_server_poll(&server,1,&events,&error),TURBODB_STATUS_OK);
      const mysql_async_step step=mysql_session_async_next(&source);
      if(step.kind==MYSQL_ASYNC_METADATA) { metadata=true; columns=step.column_count; }
      else if(step.kind==MYSQL_ASYNC_ROW) { row=true; row_size=step.row_size; }
      else if(step.kind==MYSQL_ASYNC_DONE) done=true;
      else check_false(step.kind==MYSQL_ASYNC_ERROR);
    }
    check_true(done); check_true(metadata); check_equal(columns,1u); check_true(row);
    check_greater(row_size,0u); check_equal(server.accepted,UINT64_C(1));
    check_equal(server.transport_failures,UINT64_C(0));
    check_equal(server.protocol_failures,UINT64_C(0));
  }
  it("returns the existing client's authentication error for a wrong password without a SQL session") {
    uint16_t port=0; check_equal(tdsql_mysql_server_port(&server,&port,&error),TURBODB_STATUS_OK);
    mysql_session_config_t client={.host="127.0.0.1",.port=port,.username="alice",
      .password="wrong-password",.database="tenant",.ca_file=TEST_TLS_CA,.server_name="localhost",
      .timeout_ms=TEST_IO_TIMEOUT_MS};
    mysql_session_cursor_limits_t limits={.max_result_rows=8,.max_columns=8,
      .max_metadata_bytes=TEST_BUFFER_BYTES,.max_row_bytes=TEST_BUFFER_BYTES,
      .max_command_bytes=TEST_BUFFER_BYTES};
    check_equal(mysql_session_start_async_source(&client,query,sizeof(query)-1,NULL,0,&limits,
      &source,&client_error),MYSQL_SESSION_OK);
    mysql_async_step terminal={0};
    const uint64_t deadline=salts_monotonic_ms()+TEST_TIMEOUT_MS;
    while(terminal.kind!=MYSQL_ASYNC_ERROR && salts_monotonic_ms()<deadline) {
      size_t events=0; check_equal(tdsql_mysql_server_poll(&server,1,&events,&error),TURBODB_STATUS_OK);
      terminal=mysql_session_async_next(&source);
    }
    check_equal(terminal.kind,MYSQL_ASYNC_ERROR); check_equal(terminal.status,MYSQL_SESSION_AUTH);
    check_equal(server.accepted,UINT64_C(1)); check_equal(server.transport_failures,UINT64_C(0));
    check_equal(server.protocol_failures,UINT64_C(0));
  }
  it("rejects an authenticated account without the requested database grant") {
    uint16_t port=0; check_equal(tdsql_mysql_server_port(&server,&port,&error),TURBODB_STATUS_OK);
    mysql_session_config_t client={.host="127.0.0.1",.port=port,.username="alice",
      .password=password,.database="denied",.ca_file=TEST_TLS_CA,.server_name="localhost",
      .timeout_ms=TEST_IO_TIMEOUT_MS};
    mysql_session_cursor_limits_t limits={.max_result_rows=8,.max_columns=8,
      .max_metadata_bytes=TEST_BUFFER_BYTES,.max_row_bytes=TEST_BUFFER_BYTES,
      .max_command_bytes=TEST_BUFFER_BYTES};
    check_equal(mysql_session_start_async_source(&client,query,sizeof(query)-1,NULL,0,&limits,
      &source,&client_error),MYSQL_SESSION_OK);
    mysql_async_step terminal={0};
    const uint64_t deadline=salts_monotonic_ms()+TEST_TIMEOUT_MS;
    while(terminal.kind!=MYSQL_ASYNC_ERROR && salts_monotonic_ms()<deadline) {
      size_t events=0; check_equal(tdsql_mysql_server_poll(&server,1,&events,&error),TURBODB_STATUS_OK);
      terminal=mysql_session_async_next(&source);
    }
    check_equal(terminal.kind,MYSQL_ASYNC_ERROR); check_equal(terminal.status,MYSQL_SESSION_AUTH);
    check_equal(server.accepted,UINT64_C(1)); check_equal(server.transport_failures,UINT64_C(0));
    check_equal(server.protocol_failures,UINT64_C(0));
  }
  it("keeps two authenticated prepared result sessions isolated") {
    uint16_t port=0; check_equal(tdsql_mysql_server_port(&server,&port,&error),TURBODB_STATUS_OK);
    mysql_session_config_t client={.host="127.0.0.1",.port=port,.username="alice",
      .password=password,.database="tenant",.ca_file=TEST_TLS_CA,.server_name="localhost",
      .timeout_ms=TEST_IO_TIMEOUT_MS};
    mysql_session_cursor_limits_t limits={.max_result_rows=8,.max_columns=8,
      .max_metadata_bytes=TEST_BUFFER_BYTES,.max_row_bytes=TEST_BUFFER_BYTES,
      .max_command_bytes=TEST_BUFFER_BYTES};
    check_equal(mysql_session_start_async_source(&client,query,sizeof(query)-1,NULL,0,&limits,
      &source,&client_error),MYSQL_SESSION_OK);
    bool first_ready=false;
    mysql_session_status_t first_status=MYSQL_SESSION_OK;
    const uint64_t deadline=salts_monotonic_ms()+TEST_TIMEOUT_MS;
    while(!first_ready && first_status==MYSQL_SESSION_OK && salts_monotonic_ms()<deadline) {
      size_t events=0; check_equal(tdsql_mysql_server_poll(&server,1,&events,&error),TURBODB_STATUS_OK);
      const mysql_async_step step=mysql_session_async_next(&source);
      if(step.kind==MYSQL_ASYNC_METADATA) first_ready=true;
      else if(step.kind==MYSQL_ASYNC_ERROR) first_status=step.status;
    }
    check_equal(first_status,MYSQL_SESSION_OK); check_true(first_ready);
    check_equal(mysql_session_start_async_source(&client,query,sizeof(query)-1,NULL,0,&limits,
      &second_source,&client_error),MYSQL_SESSION_OK);
    bool first_done=false,second_done=false,first_row=false,second_row=false;
    mysql_session_status_t second_status=MYSQL_SESSION_OK;
    while((!first_done || !second_done) && salts_monotonic_ms()<deadline) {
      size_t events=0; check_equal(tdsql_mysql_server_poll(&server,1,&events,&error),TURBODB_STATUS_OK);
      if(!first_done) {
        const mysql_async_step step=mysql_session_async_next(&source);
        if(step.kind==MYSQL_ASYNC_ROW) first_row=true;
        else if(step.kind==MYSQL_ASYNC_DONE) first_done=true;
        else if(step.kind==MYSQL_ASYNC_ERROR) { first_status=step.status; first_done=true; }
      }
      if(!second_done) {
        const mysql_async_step step=mysql_session_async_next(&second_source);
        if(step.kind==MYSQL_ASYNC_ROW) second_row=true;
        else if(step.kind==MYSQL_ASYNC_DONE) second_done=true;
        else if(step.kind==MYSQL_ASYNC_ERROR) { second_status=step.status; second_done=true; }
      }
    }
    info("sessions: first=%d second=%d active=%zu transport=%llu protocol=%llu network=%d",
      (int)first_status,(int)second_status,server.active,
      (unsigned long long)server.transport_failures,(unsigned long long)server.protocol_failures,
      server.network_failure);
    check_equal(first_status,MYSQL_SESSION_OK); check_equal(second_status,MYSQL_SESSION_OK);
    check_true(first_done); check_true(second_done); check_true(first_row); check_true(second_row);
    check_equal(server.accepted,UINT64_C(2));
    check_equal(server.active,2u);
    check_equal(server.transport_failures,UINT64_C(0)); check_equal(server.protocol_failures,UINT64_C(0));
  }
  it("bounds shutdown when an accepted client stops before TLS negotiation") {
    start_client_until_accepted();
    check_equal(tdsql_mysql_server_stop(&server,TEST_IO_TIMEOUT_MS,&error),TURBODB_STATUS_OK);
    check_false(server.initialized);
    check_equal(mysql_session_async_close(&source,&client_error),MYSQL_SESSION_OK);
    check_null(source.context);
  }
  it("expires an accepted client that stalls before TLS negotiation") {
    start_client_until_accepted();
    const uint64_t deadline=salts_monotonic_ms()+TEST_TIMEOUT_MS;
    while(server.active && salts_monotonic_ms()<deadline) {
      size_t events=0;
      check_equal(tdsql_mysql_server_poll(&server,10,&events,&error),TURBODB_STATUS_OK);
    }
    check_equal(server.active,0u); check_equal(server.closed,UINT64_C(1));
    check_equal(server.transport_failures,UINT64_C(1));
    check_equal(server.network_failure,SALTS_ETIMEDOUT);
    check_equal(mysql_session_async_close(&source,&client_error),MYSQL_SESSION_OK);
    check_null(source.context);
  }
  it("keeps stop timeout retryable without reopening admission") {
    start_client_until_accepted();
    check_equal(tdsql_mysql_server_stop(&server,0,&error),TURBODB_STATUS_BUSY);
    check_true(server.initialized); check_true(server.stopping);
    size_t events=TEST_CONNECTIONS;
    check_equal(tdsql_mysql_server_poll(&server,0,&events,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(events,0u);
    check_equal(tdsql_mysql_server_stop(&server,TEST_IO_TIMEOUT_MS,&error),TURBODB_STATUS_OK);
    check_false(server.initialized);
    check_equal(mysql_session_async_close(&source,&client_error),MYSQL_SESSION_OK);
    check_null(source.context);
  }
  it("rejects unbounded or malformed service configuration without publishing an owner") {
    tdsql_mysql_server candidate={.next_connection_id=99};
    tdsql_mysql_server_config invalid=server_config; invalid.transport.read_timeout_ms=0;
    check_equal(tdsql_mysql_server_init(&candidate,&invalid,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_false(candidate.initialized); check_equal(candidate.next_connection_id,99u);
    candidate=(tdsql_mysql_server){.next_connection_id=101};
    static const char malformed_version[]={'b','a','d',0,'v'};
    invalid=server_config; invalid.server_version=(vstr){malformed_version,sizeof(malformed_version)};
    check_equal(tdsql_mysql_server_init(&candidate,&invalid,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_false(candidate.initialized); check_equal(candidate.next_connection_id,101u);
    candidate=(tdsql_mysql_server){.next_connection_id=103};
    invalid=server_config; invalid.output_bytes=TDSQL_MYSQL_SERVER_MIN_OUTPUT_BYTES-1;
    check_equal(tdsql_mysql_server_init(&candidate,&invalid,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_false(candidate.initialized); check_equal(candidate.next_connection_id,103u);
  }
}
