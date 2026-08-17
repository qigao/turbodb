#ifndef ORM_C_API_TEST_SUPPORT_H
#define ORM_C_API_TEST_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void fake_pg_reset(void);
void fake_pg_set_result(size_t rows,
                        size_t columns,
                        const char* const* values,
                        const uint8_t* null_flags);
void fake_pg_set_result_types(size_t columns, const uint32_t* types);
void fake_pg_fail_next_query(void);
const char* fake_pg_last_sql(void);
size_t fake_pg_parameter_count(void);
const char* fake_pg_parameter_at(size_t index);
int fake_pg_finished_connections(void);
int fake_pg_cleared_results(void);

#ifdef __cplusplus
}
#endif

#endif
