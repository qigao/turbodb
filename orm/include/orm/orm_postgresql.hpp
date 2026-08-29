#ifndef ORM_POSTGRESQL_HPP
#define ORM_POSTGRESQL_HPP

#include "orm.hpp"
#include "orm_postgresql.h"

namespace orm {

  inline connection postgresql_connection(const config &configuration) {
    return connection(configuration, orm_postgresql_connect);
  }

} // namespace orm

#endif
