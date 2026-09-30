#include <orm.hpp>
#include <tinytest.hpp>

spec("ORM public retained ownership C++ facade") {
  it("surfaces checked BUSY and closes after the child handle is released") {
    orm::connection connection(
        orm::config("sqlite").option("filename", ":memory:"));

    {
      auto query = connection.raw("select 1");
      bool busy = false;
      try {
        connection.close();
      } catch (const orm::status_error &error) {
        busy = error.status() == ORM_STATUS_BUSY;
      }
      check_true(busy);
      query.close();
    }

    connection.close();
  }

  it("keeps move-only RAII compatible with retained native ownership") {
    orm::connection first(
        orm::config("sqlite").option("filename", ":memory:"));
    orm::connection moved(std::move(first));
    auto query = moved.raw("select 1");
    query.close();
  }
}
