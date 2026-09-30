#include <turbodb_mysql.h>
#include <tinytest.hpp>

spec("standalone MySQL C++ consumer") {
  (void)ttest_config__;
  it("links the C session API without an ORM dependency") {
    mysql_session_error_t error{};
    check_equal(mysql_session_connect_and_ping(nullptr, &error),
                MYSQL_SESSION_INVALID);
    check_equal(error.status, MYSQL_SESSION_INVALID);
  }
}
