#include "c_facade_fixture.orm.h"

#include <tinytest.h>

#include <stdint.h>

static orm_connection_t *create_connection(orm_error_t *error) {
  orm_config_t config;
  orm_option_t options[2];
  orm_connection_t *connection = NULL;

  orm_config(&config);
  options[0].keyword = orm_view("filename");
  options[0].value = orm_view(":memory:");
  options[1].keyword = orm_view("open_mode");
  options[1].value = orm_view("read_write_create");
  config.driver = orm_view("sqlite");
  config.options = options;
  config.option_count = 2u;
  check(orm_connect(&config, &connection, error) == ORM_STATUS_OK,
        error->message);
  check_not_null(connection);
  return connection;
}

static void execute_sql(orm_connection_t *connection, const char *sql,
                        orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;

  check(orm_raw(connection, orm_view(sql), &query, error) == ORM_STATUS_OK,
        error->message);
  check(orm_query_execute(query, &result, error) == ORM_STATUS_OK,
        error->message);
  orm_result_destroy(result);
  orm_query_destroy(query);
}

static int user_has_nickname(const User_t *user) {
  const size_t byte_index = (size_t)User_OPTIONAL_nickname / 8u;
  const uint8_t mask =
      (uint8_t)(1u << ((unsigned int)User_OPTIONAL_nickname % 8u));
  return (user->_presence[byte_index] & mask) != 0u;
}

static void user_set_nickname_present(User_t *user) {
  const size_t byte_index = (size_t)User_OPTIONAL_nickname / 8u;
  const uint8_t mask =
      (uint8_t)(1u << ((unsigned int)User_OPTIONAL_nickname % 8u));
  user->_presence[byte_index] |= mask;
}

suite("ORM generated C facade") {
  it("performs typed CRUD with ownership and optimistic locking") {
    orm_error_t error;
    orm_connection_t *connection;
    User_t inserted;
    User_t current;
    User_t stale;
    User_t verified;
    User_t missing;
    uint64_t affected = 0u;
    uint8_t found = 0u;

    orm_error_init(&error);
    connection = create_connection(&error);
    execute_sql(connection,
                "create table users("
                "user_id integer primary key,"
                "entity_version integer not null,"
                "state integer not null,"
                "display_name text not null,"
                "nickname text null)",
                &error);

    User_init(&inserted);
    User_init(&current);
    User_init(&stale);
    User_init(&verified);
    User_init(&missing);

    inserted.id = 7u;
    inserted.version = 0u;
    inserted.state = UserState_active;
    inserted.display_name = tstr_cpy(inserted.display_name, "Alice");
    check_not_null(inserted.display_name);
    check(CStore_User_orm_insert(connection, &inserted, &affected, &error) ==
              ORM_STATUS_OK,
          error.message);
    check_ull_eq(affected, 1u);

    check(CStore_User_orm_find(connection, inserted.id, &current, &found,
                               &error) == ORM_STATUS_OK,
          error.message);
    check_int_eq(found, 1);
    check_str_eq(current.display_name, "Alice");
    check_int_eq(current.state, UserState_active);
    check_false(user_has_nickname(&current));

    check(CStore_User_orm_find(connection, inserted.id, &stale, &found,
                               &error) == ORM_STATUS_OK,
          error.message);
    check_int_eq(found, 1);
    current.display_name = tstr_cpy(current.display_name, "Alice Cooper");
    current.nickname = tstr_cpy(current.nickname, "ally");
    check_not_null(current.display_name);
    check_not_null(current.nickname);
    user_set_nickname_present(&current);

    check(CStore_User_orm_update(connection, &current, &affected, &error) ==
              ORM_STATUS_OK,
          error.message);
    check_ull_eq(affected, 1u);
    check_ull_eq(current.version, 1u);
    check(CStore_User_orm_update(connection, &stale, &affected, &error) ==
              ORM_STATUS_INVALID_STATE,
          "stale entity update must fail optimistic locking");
    check_ull_eq(affected, 0u);
    check_ull_eq(stale.version, 0u);

    check(CStore_User_orm_find(connection, inserted.id, &verified, &found,
                               &error) == ORM_STATUS_OK,
          error.message);
    check_int_eq(found, 1);
    check_str_eq(verified.display_name, "Alice Cooper");
    check_true(user_has_nickname(&verified));
    check_str_eq(verified.nickname, "ally");
    check_ull_eq(verified.version, 1u);

    check(CStore_User_orm_remove(connection, &current, &affected, &error) ==
              ORM_STATUS_OK,
          error.message);
    check_ull_eq(affected, 1u);
    check(CStore_User_orm_find(connection, inserted.id, &missing, &found,
                               &error) == ORM_STATUS_OK,
          error.message);
    check_int_eq(found, 0);

    User_clear(&missing);
    User_clear(&verified);
    User_clear(&stale);
    User_clear(&current);
    User_clear(&inserted);
    orm_disconnect(connection);
  }
}
