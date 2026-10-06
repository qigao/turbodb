# Drivers are built after the generic ORM and own their native dependencies.
get_target_property(ORM_SOURCE_DIR turbo_orm SOURCE_DIR)

add_subdirectory(sqlite)
add_subdirectory(postgresql)
add_subdirectory(tidesdb)
add_subdirectory(mysql)

# MongoDB is retained as source only.
