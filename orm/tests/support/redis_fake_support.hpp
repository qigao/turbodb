#ifndef ORM_TEST_REDIS_FAKE_SUPPORT_HPP
#define ORM_TEST_REDIS_FAKE_SUPPORT_HPP

#include <redis_client.h>

#include <cstddef>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

void fake_redis_reset(void);
void fake_redis_script_next_outcome(redis_command_outcome_t outcome);
void fake_redis_script_next_server_error(redis_server_error_t server_error,
                                         const char* text);
void fake_redis_script_next_null_reply(void);
void fake_redis_script_exec_reply_count(size_t count);
void fake_redis_script_exec_error_at(size_t index, const char* message);

#ifdef __cplusplus
}

// Test-only accessor for commands observed by the Redis client fake.
std::vector<std::vector<std::string>> fake_redis_commands();

#endif

#endif
