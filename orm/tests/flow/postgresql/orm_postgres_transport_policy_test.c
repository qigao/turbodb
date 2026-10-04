#include "postgres_transport_policy.h"

#include "tinytest.h"

#include <string.h>

static orm_config_t config_with(orm_option_t *options, uint32_t count) {
  orm_config_t config;
  orm_config(&config);
  config.driver = orm_view("postgresql");
  config.options = options;
  config.option_count = count;
  return config;
}

spec("ORM PostgreSQL secure transport policy") {
  it("requires an explicit transport mode") {
    orm_option_t options[] = {
        {orm_view("host"), orm_view("127.0.0.1")}};
    orm_config_t config = config_with(options, 1u);
    orm_postgres_transport_policy policy = ORM_POSTGRES_TRANSPORT_POLICY_INIT;
    orm_error_t error;
    orm_error_init(&error);

    check_equal(orm_postgres_transport_policy_parse(&config, &policy, &error),
                ORM_STATUS_INVALID_ARGUMENT);
  }

  it("accepts explicit plaintext mode without secure options") {
    orm_option_t options[] = {
        {orm_view("turbodb_pg_transport"), orm_view("disabled")},
        {orm_view("host"), orm_view("127.0.0.1")}};
    orm_config_t config = config_with(options, 2u);
    orm_postgres_transport_policy policy = ORM_POSTGRES_TRANSPORT_POLICY_INIT;
    orm_error_t error;
    orm_error_init(&error);

    check_equal(orm_postgres_transport_policy_parse(&config, &policy, &error),
                ORM_STATUS_OK);
    check_equal(policy.mode, ORM_POSTGRES_TRANSPORT_DISABLED);
    check_equal(policy.ingress_buffer_bytes,
                ORM_POSTGRES_TRANSPORT_DEFAULT_BUFFER_BYTES);
  }

  it("rejects secure fields when plaintext mode is selected") {
    orm_option_t options[] = {
        {orm_view("turbodb_pg_transport"), orm_view("disabled")},
        {orm_view("turbodb_pg_remote_host"), orm_view("db.example.test")}};
    orm_config_t config = config_with(options, 2u);
    orm_postgres_transport_policy policy = ORM_POSTGRES_TRANSPORT_POLICY_INIT;
    orm_error_t error;
    orm_error_init(&error);

    check_equal(orm_postgres_transport_policy_parse(&config, &policy, &error),
                ORM_STATUS_INVALID_ARGUMENT);
  }

  it("parses a bounded PG17 direct TLS policy") {
    orm_option_t options[] = {
        {orm_view("turbodb_pg_transport"), orm_view("direct_tls")},
        {orm_view("turbodb_pg_remote_host"), orm_view("db.example.test")},
        {orm_view("turbodb_pg_remote_port"), orm_view("5432")},
        {orm_view("turbodb_pg_server_name"), orm_view("db.example.test")},
        {orm_view("turbodb_pg_ca_file"), orm_view("/tmp/postgres-ca.pem")},
        {orm_view("turbodb_pg_connect_timeout_ms"), orm_view("5000")},
        {orm_view("turbodb_pg_handshake_timeout_ms"), orm_view("5000")},
        {orm_view("turbodb_pg_idle_timeout_ms"), orm_view("30000")},
        {orm_view("turbodb_pg_ingress_bytes"), orm_view("32768")},
        {orm_view("turbodb_pg_egress_bytes"), orm_view("65536")}};
    orm_config_t config = config_with(options, 10u);
    orm_postgres_transport_policy policy = ORM_POSTGRES_TRANSPORT_POLICY_INIT;
    orm_error_t error;
    orm_error_init(&error);

    check_equal(orm_postgres_transport_policy_parse(&config, &policy, &error),
                ORM_STATUS_OK);
    check_equal(policy.mode, ORM_POSTGRES_TRANSPORT_DIRECT_TLS);
    check_equal(policy.channel_binding, ORM_POSTGRES_CHANNEL_BINDING_PREFER);
    check_equal(policy.remote_port, (uint16_t)5432u);
    check_equal(policy.connect_timeout_ms, (uint32_t)5000u);
    check_equal(policy.ingress_buffer_bytes, (uint32_t)32768u);
    check_equal(policy.egress_buffer_bytes, (uint32_t)65536u);
    check_equal(strcmp(policy.server_name, "db.example.test"), 0);
  }

  it("accepts explicit require and disable channel-binding policies") {
    orm_option_t required[] = {
        {orm_view("turbodb_pg_transport"), orm_view("direct_tls")},
        {orm_view("turbodb_pg_remote_host"), orm_view("db.example.test")},
        {orm_view("turbodb_pg_remote_port"), orm_view("5432")},
        {orm_view("turbodb_pg_server_name"), orm_view("db.example.test")},
        {orm_view("turbodb_pg_ca_file"), orm_view("/tmp/ca.pem")},
        {orm_view("turbodb_pg_channel_binding"), orm_view("require")}};
    orm_option_t disabled[] = {
        {orm_view("turbodb_pg_transport"), orm_view("direct_tls")},
        {orm_view("turbodb_pg_remote_host"), orm_view("db.example.test")},
        {orm_view("turbodb_pg_remote_port"), orm_view("5432")},
        {orm_view("turbodb_pg_server_name"), orm_view("db.example.test")},
        {orm_view("turbodb_pg_ca_file"), orm_view("/tmp/ca.pem")},
        {orm_view("turbodb_pg_channel_binding"), orm_view("disable")}};
    orm_config_t config;
    orm_postgres_transport_policy policy = ORM_POSTGRES_TRANSPORT_POLICY_INIT;
    orm_error_t error;

    orm_error_init(&error);
    config = config_with(required, 6u);
    check_equal(orm_postgres_transport_policy_parse(&config, &policy, &error),
                ORM_STATUS_OK);
    check_equal(policy.channel_binding, ORM_POSTGRES_CHANNEL_BINDING_REQUIRE);

    orm_error_init(&error);
    config = config_with(disabled, 6u);
    check_equal(orm_postgres_transport_policy_parse(&config, &policy, &error),
                ORM_STATUS_OK);
    check_equal(policy.channel_binding, ORM_POSTGRES_CHANNEL_BINDING_DISABLE);
  }

  it("requires exactly one CA source for direct TLS") {
    orm_option_t options[] = {
        {orm_view("turbodb_pg_transport"), orm_view("direct_tls")},
        {orm_view("turbodb_pg_remote_host"), orm_view("db.example.test")},
        {orm_view("turbodb_pg_remote_port"), orm_view("5432")},
        {orm_view("turbodb_pg_server_name"), orm_view("db.example.test")},
        {orm_view("turbodb_pg_ca_file"), orm_view("/tmp/ca.pem")},
        {orm_view("turbodb_pg_ca_path"), orm_view("/tmp/certs")}};
    orm_config_t config = config_with(options, 6u);
    orm_postgres_transport_policy policy = ORM_POSTGRES_TRANSPORT_POLICY_INIT;
    orm_error_t error;
    orm_error_init(&error);

    check_equal(orm_postgres_transport_policy_parse(&config, &policy, &error),
                ORM_STATUS_INVALID_ARGUMENT);
  }

  it("rejects unknown reserved options and out-of-range buffers") {
    orm_option_t unknown[] = {
        {orm_view("turbodb_pg_transport"), orm_view("disabled")},
        {orm_view("turbodb_pg_magic"), orm_view("1")}};
    orm_option_t small_buffer[] = {
        {orm_view("turbodb_pg_transport"), orm_view("direct_tls")},
        {orm_view("turbodb_pg_remote_host"), orm_view("db.example.test")},
        {orm_view("turbodb_pg_remote_port"), orm_view("5432")},
        {orm_view("turbodb_pg_server_name"), orm_view("db.example.test")},
        {orm_view("turbodb_pg_ca_file"), orm_view("/tmp/ca.pem")},
        {orm_view("turbodb_pg_ingress_bytes"), orm_view("1024")}};
    orm_config_t config;
    orm_postgres_transport_policy policy = ORM_POSTGRES_TRANSPORT_POLICY_INIT;
    orm_error_t error;

    orm_error_init(&error);
    config = config_with(unknown, 2u);
    check_equal(orm_postgres_transport_policy_parse(&config, &policy, &error),
                ORM_STATUS_INVALID_ARGUMENT);

    orm_error_init(&error);
    config = config_with(small_buffer, 6u);
    check_equal(orm_postgres_transport_policy_parse(&config, &policy, &error),
                ORM_STATUS_OUT_OF_RANGE);
  }
}
