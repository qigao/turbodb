/*
 * redis_ext.c — Extended Redis command implementations
 *
 * Covers: String extras, List extras, Hash extras, Set extras,
 *         Sorted Set, Key management, Server commands,
 *         Transaction, Scripting.
 */

#include "redis_client.h"
#include "redis_internal.h"
#include <fmt.h>
#include <turbo_error.h>
#include <limits.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* forward-declared in redis_client.c and linked via the shared library */
extern int redis_commandv(redis_client_t *client, int argc, const char **argv,
                          const size_t *argvlen,
                          redis_command_cb_t callback, void *user_data);

#define calc_argc_mul_add redis_argc_mul_add
#define alloc_argv redis_argv_alloc

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Build argv from a fixed prefix + a caller-supplied array. */
static int dispatch_prefix_array(redis_client_t *client,
                                 const char **prefix, int prefix_len,
                                 const char **items, int item_count,
                                 redis_command_cb_t callback, void *user_data) {
  int total = 0;
  if (calc_argc_mul_add(prefix_len, item_count, 1, &total) < 0) return -1;
  const char **argv = alloc_argv(total);
  if (!argv) return -1;
  for (int i = 0; i < prefix_len; i++) argv[i] = prefix[i];
  for (int i = 0; i < item_count; i++) argv[prefix_len + i] = items[i];
  int result = redis_commandv(client, total, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

/* Shared logic for BLPOP / BRPOP. */
static int blocking_pop(redis_client_t *client, const char *cmd,
                        int key_count, const char **keys, double timeout_sec,
                        redis_command_cb_t callback, void *user_data) {
  if (!client || !keys || key_count <= 0) return -1;
  if (timeout_sec < 0.0 || isnan(timeout_sec) || isinf(timeout_sec)) return -1;
  int argc = 0;
  char timeout_str[32];
  fmt(timeout_str, sizeof(timeout_str), "{:.6f}", timeout_sec);
  if (calc_argc_mul_add(2, key_count, 1, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  argv[0] = cmd;
  for (int i = 0; i < key_count; i++) argv[1 + i] = keys[i];
  argv[1 + key_count] = timeout_str;
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

/* Shared logic for SUNION/SINTER/SDIFF. */
static int set_multikey(redis_client_t *client, const char *cmd,
                        int key_count, const char **keys,
                        redis_command_cb_t callback, void *user_data) {
  if (!client || !keys || key_count <= 0) return -1;
  const char *prefix[] = {cmd};
  return dispatch_prefix_array(client, prefix, 1, keys, key_count,
                               callback, user_data);
}

/* Shared logic for SUNIONSTORE / SINTERSTORE / SDIFFSTORE. */
static int set_store(redis_client_t *client, const char *cmd,
                     const char *dest, int key_count, const char **keys,
                     redis_command_cb_t callback, void *user_data) {
  if (!client || !dest || !keys || key_count <= 0) return -1;
  int argc = 0;
  if (calc_argc_mul_add(2, key_count, 1, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  argv[0] = cmd;
  argv[1] = dest;
  for (int i = 0; i < key_count; i++) argv[2 + i] = keys[i];
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

/* EVAL / EVALSHA shared body. */
static int eval_common(redis_client_t *client,
                       const char *cmd, const char *script_or_sha,
                       int key_count, const char **keys,
                       int arg_count, const char **args,
                       redis_command_cb_t callback, void *user_data) {
  if (!client || !script_or_sha) return -1;
  int argc = 0;
  if (calc_argc_mul_add(3, key_count, 1, &argc) < 0) return -1;
  if (calc_argc_mul_add(argc, arg_count, 1, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  char numkeys_str[32];
  fmt(numkeys_str, sizeof(numkeys_str), "{}", key_count);
  int idx = 0;
  argv[idx++] = cmd;
  argv[idx++] = script_or_sha;
  argv[idx++] = numkeys_str;
  for (int i = 0; i < key_count; i++) argv[idx++] = keys[i];
  for (int i = 0; i < arg_count; i++) argv[idx++] = args[i];
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

static int eval_common_result(redis_client_t *client,
                              const char *cmd, const char *script_or_sha,
                              int key_count, const char **keys,
                              int arg_count, const char **args,
                              redis_command_result_t *out) {
  int argc = 0;
  const char **argv;
  char numkeys_str[32];
  int idx = 0;
  int result;

  if (!out) return TURBO_EINVAL;
  *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
  if (!client || !cmd || !script_or_sha || key_count < 0 || arg_count < 0 ||
      (key_count > 0 && !keys) || (arg_count > 0 && !args)) {
    out->status = TURBO_EINVAL;
    return out->status;
  }
  for (int i = 0; i < key_count; ++i) {
    if (!keys[i]) {
      out->status = TURBO_EINVAL;
      return out->status;
    }
  }
  for (int i = 0; i < arg_count; ++i) {
    if (!args[i]) {
      out->status = TURBO_EINVAL;
      return out->status;
    }
  }
  if (calc_argc_mul_add(3, key_count, 1, &argc) < 0 ||
      calc_argc_mul_add(argc, arg_count, 1, &argc) < 0) {
    out->status = TURBO_EINVAL;
    return out->status;
  }
  argv = alloc_argv(argc);
  if (!argv) {
    out->status = TURBO_ENOMEM;
    return out->status;
  }
  fmt(numkeys_str, sizeof(numkeys_str), "{}", key_count);
  argv[idx++] = cmd;
  argv[idx++] = script_or_sha;
  argv[idx++] = numkeys_str;
  for (int i = 0; i < key_count; ++i) argv[idx++] = keys[i];
  for (int i = 0; i < arg_count; ++i) argv[idx++] = args[i];
  result = redis_commandv_result(client, argc, argv, NULL, out);
  redis_argv_free(argv);
  return result;
}

/* =========================================================================
 * Extended String API
 * ========================================================================= */

int redis_mset(redis_client_t *client,
               int pair_count, const char **keys, const char **values,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !keys || !values || pair_count <= 0) return -1;
  int argc = 0;
  if (calc_argc_mul_add(1, pair_count, 2, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  argv[0] = "MSET";
  for (int i = 0; i < pair_count; i++) {
    argv[1 + i * 2]     = keys[i];
    argv[1 + i * 2 + 1] = values[i];
  }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_mget(redis_client_t *client,
               int key_count, const char **keys,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !keys || key_count <= 0) return -1;
  const char *prefix[] = {"MGET"};
  return dispatch_prefix_array(client, prefix, 1, keys, key_count,
                               callback, user_data);
}

int redis_setnx(redis_client_t *client, const char *key, const char *value,
                redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"SETNX", key, value};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_setex(redis_client_t *client, const char *key, int seconds,
                const char *value,
                redis_command_cb_t callback, void *user_data) {
  char sec_str[32];
  fmt(sec_str, sizeof(sec_str), "{}", seconds);
  const char *argv[] = {"SETEX", key, sec_str, value};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_psetex(redis_client_t *client, const char *key, int64_t ms,
                 const char *value,
                 redis_command_cb_t callback, void *user_data) {
  char ms_str[32];
  fmt(ms_str, sizeof(ms_str), "{}", ms);
  const char *argv[] = {"PSETEX", key, ms_str, value};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_getset(redis_client_t *client, const char *key, const char *value,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"GETSET", key, value};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_getdel(redis_client_t *client, const char *key,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"GETDEL", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_incrby(redis_client_t *client, const char *key, int64_t increment,
                 redis_command_cb_t callback, void *user_data) {
  char inc_str[32];
  fmt(inc_str, sizeof(inc_str), "{}", increment);
  const char *argv[] = {"INCRBY", key, inc_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_decrby(redis_client_t *client, const char *key, int64_t decrement,
                 redis_command_cb_t callback, void *user_data) {
  char dec_str[32];
  fmt(dec_str, sizeof(dec_str), "{}", decrement);
  const char *argv[] = {"DECRBY", key, dec_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_decr(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"DECR", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_incrbyfloat(redis_client_t *client, const char *key, double increment,
                      redis_command_cb_t callback, void *user_data) {
  char inc_str[64];
  fmt(inc_str, sizeof(inc_str), "{:.17g}", increment);
  const char *argv[] = {"INCRBYFLOAT", key, inc_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_append(redis_client_t *client, const char *key, const char *value,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"APPEND", key, value};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_strlen(redis_client_t *client, const char *key,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"STRLEN", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_getrange(redis_client_t *client, const char *key,
                   int64_t start, int64_t end,
                   redis_command_cb_t callback, void *user_data) {
  char s_str[32], e_str[32];
  fmt(s_str, sizeof(s_str), "{}", start);
  fmt(e_str, sizeof(e_str), "{}", end);
  const char *argv[] = {"GETRANGE", key, s_str, e_str};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_setrange(redis_client_t *client, const char *key,
                   int64_t offset, const char *value,
                   redis_command_cb_t callback, void *user_data) {
  char off_str[32];
  fmt(off_str, sizeof(off_str), "{}", offset);
  const char *argv[] = {"SETRANGE", key, off_str, value};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

/* =========================================================================
 * Extended List API
 * ========================================================================= */

int redis_llen(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"LLEN", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_lrange(redis_client_t *client, const char *key,
                 int64_t start, int64_t stop,
                 redis_command_cb_t callback, void *user_data) {
  char s_str[32], e_str[32];
  fmt(s_str, sizeof(s_str), "{}", start);
  fmt(e_str, sizeof(e_str), "{}", stop);
  const char *argv[] = {"LRANGE", key, s_str, e_str};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_lindex(redis_client_t *client, const char *key, int64_t index,
                 redis_command_cb_t callback, void *user_data) {
  char idx_str[32];
  fmt(idx_str, sizeof(idx_str), "{}", index);
  const char *argv[] = {"LINDEX", key, idx_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_lset(redis_client_t *client, const char *key, int64_t index,
               const char *value,
               redis_command_cb_t callback, void *user_data) {
  char idx_str[32];
  fmt(idx_str, sizeof(idx_str), "{}", index);
  const char *argv[] = {"LSET", key, idx_str, value};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_lrem(redis_client_t *client, const char *key, int64_t count,
               const char *value,
               redis_command_cb_t callback, void *user_data) {
  char cnt_str[32];
  fmt(cnt_str, sizeof(cnt_str), "{}", count);
  const char *argv[] = {"LREM", key, cnt_str, value};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_linsert(redis_client_t *client, const char *key, int before,
                  const char *pivot, const char *value,
                  redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"LINSERT", key, before ? "BEFORE" : "AFTER", pivot, value};
  return redis_commandv(client, 5, argv, NULL, callback, user_data);
}

int redis_ltrim(redis_client_t *client, const char *key,
                int64_t start, int64_t stop,
                redis_command_cb_t callback, void *user_data) {
  char s_str[32], e_str[32];
  fmt(s_str, sizeof(s_str), "{}", start);
  fmt(e_str, sizeof(e_str), "{}", stop);
  const char *argv[] = {"LTRIM", key, s_str, e_str};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_blpop(redis_client_t *client,
                int key_count, const char **keys, double timeout_sec,
                redis_command_cb_t callback, void *user_data) {
  return blocking_pop(client, "BLPOP", key_count, keys, timeout_sec,
                      callback, user_data);
}

int redis_brpop(redis_client_t *client,
                int key_count, const char **keys, double timeout_sec,
                redis_command_cb_t callback, void *user_data) {
  return blocking_pop(client, "BRPOP", key_count, keys, timeout_sec,
                      callback, user_data);
}

int redis_lmove(redis_client_t *client,
                const char *src, const char *dst,
                const char *wherefrom, const char *whereto,
                redis_command_cb_t callback, void *user_data) {
  if (!client || !src || !dst || !wherefrom || !whereto) return -1;
  const char *argv[] = {"LMOVE", src, dst, wherefrom, whereto};
  return redis_commandv(client, 5, argv, NULL, callback, user_data);
}

/* =========================================================================
 * Extended Hash API
 * ========================================================================= */

int redis_hmset(redis_client_t *client, const char *key,
                int pair_count, const char **fields, const char **values,
                redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !fields || !values || pair_count <= 0) return -1;
  int argc = 0;
  if (calc_argc_mul_add(2, pair_count, 2, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  argv[0] = "HMSET";
  argv[1] = key;
  for (int i = 0; i < pair_count; i++) {
    argv[2 + i * 2]     = fields[i];
    argv[2 + i * 2 + 1] = values[i];
  }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_hmget(redis_client_t *client, const char *key,
                int field_count, const char **fields,
                redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !fields || field_count <= 0) return -1;
  int argc = 0;
  if (calc_argc_mul_add(2, field_count, 1, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  argv[0] = "HMGET";
  argv[1] = key;
  for (int i = 0; i < field_count; i++) argv[2 + i] = fields[i];
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_hgetall(redis_client_t *client, const char *key,
                  redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"HGETALL", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_hkeys(redis_client_t *client, const char *key,
                redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"HKEYS", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_hvals(redis_client_t *client, const char *key,
                redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"HVALS", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_hlen(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"HLEN", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_hexists(redis_client_t *client, const char *key, const char *field,
                  redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"HEXISTS", key, field};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_hdel(redis_client_t *client, const char *key,
               int field_count, const char **fields,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !fields || field_count <= 0) return -1;
  int argc = 0;
  if (calc_argc_mul_add(2, field_count, 1, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  argv[0] = "HDEL";
  argv[1] = key;
  for (int i = 0; i < field_count; i++) argv[2 + i] = fields[i];
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_hincrby(redis_client_t *client, const char *key,
                  const char *field, int64_t increment,
                  redis_command_cb_t callback, void *user_data) {
  char inc_str[32];
  fmt(inc_str, sizeof(inc_str), "{}", increment);
  const char *argv[] = {"HINCRBY", key, field, inc_str};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_hincrbyfloat(redis_client_t *client, const char *key,
                       const char *field, double increment,
                       redis_command_cb_t callback, void *user_data) {
  char inc_str[64];
  fmt(inc_str, sizeof(inc_str), "{:.17g}", increment);
  const char *argv[] = {"HINCRBYFLOAT", key, field, inc_str};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_hsetnx(redis_client_t *client, const char *key,
                 const char *field, const char *value,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"HSETNX", key, field, value};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

/* =========================================================================
 * Extended Set API
 * ========================================================================= */

int redis_srem(redis_client_t *client, const char *key,
               int member_count, const char **members,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !members || member_count <= 0) return -1;
  const char *prefix[] = {"SREM", key};
  return dispatch_prefix_array(client, prefix, 2, members, member_count,
                               callback, user_data);
}

int redis_scard(redis_client_t *client, const char *key,
                redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"SCARD", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_sismember(redis_client_t *client, const char *key, const char *member,
                    redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"SISMEMBER", key, member};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_smismember(redis_client_t *client, const char *key,
                     int member_count, const char **members,
                     redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !members || member_count <= 0) return -1;
  const char *prefix[] = {"SMISMEMBER", key};
  return dispatch_prefix_array(client, prefix, 2, members, member_count,
                               callback, user_data);
}

int redis_spop(redis_client_t *client, const char *key, int count,
               redis_command_cb_t callback, void *user_data) {
  if (count <= 1) {
    const char *argv[] = {"SPOP", key};
    return redis_commandv(client, 2, argv, NULL, callback, user_data);
  }
  char cnt_str[32];
  fmt(cnt_str, sizeof(cnt_str), "{}", count);
  const char *argv[] = {"SPOP", key, cnt_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_srandmember(redis_client_t *client, const char *key, int count,
                      redis_command_cb_t callback, void *user_data) {
  if (count == 1) {
    const char *argv[] = {"SRANDMEMBER", key};
    return redis_commandv(client, 2, argv, NULL, callback, user_data);
  }
  char cnt_str[32];
  fmt(cnt_str, sizeof(cnt_str), "{}", count);
  const char *argv[] = {"SRANDMEMBER", key, cnt_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_sunion(redis_client_t *client, int key_count, const char **keys,
                 redis_command_cb_t callback, void *user_data) {
  return set_multikey(client, "SUNION", key_count, keys, callback, user_data);
}

int redis_sinter(redis_client_t *client, int key_count, const char **keys,
                 redis_command_cb_t callback, void *user_data) {
  return set_multikey(client, "SINTER", key_count, keys, callback, user_data);
}

int redis_sdiff(redis_client_t *client, int key_count, const char **keys,
                redis_command_cb_t callback, void *user_data) {
  return set_multikey(client, "SDIFF", key_count, keys, callback, user_data);
}

int redis_sunionstore(redis_client_t *client, const char *dest,
                      int key_count, const char **keys,
                      redis_command_cb_t callback, void *user_data) {
  return set_store(client, "SUNIONSTORE", dest, key_count, keys,
                   callback, user_data);
}

int redis_sinterstore(redis_client_t *client, const char *dest,
                      int key_count, const char **keys,
                      redis_command_cb_t callback, void *user_data) {
  return set_store(client, "SINTERSTORE", dest, key_count, keys,
                   callback, user_data);
}

int redis_sdiffstore(redis_client_t *client, const char *dest,
                     int key_count, const char **keys,
                     redis_command_cb_t callback, void *user_data) {
  return set_store(client, "SDIFFSTORE", dest, key_count, keys,
                   callback, user_data);
}

/* =========================================================================
 * Sorted Set API
 * ========================================================================= */

int redis_zadd(redis_client_t *client, const char *key,
               int pair_count, const char **scores, const char **members,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !scores || !members || pair_count <= 0) return -1;
  int argc = 0;
  if (calc_argc_mul_add(2, pair_count, 2, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  argv[0] = "ZADD";
  argv[1] = key;
  for (int i = 0; i < pair_count; i++) {
    argv[2 + i * 2]     = scores[i];
    argv[2 + i * 2 + 1] = members[i];
  }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_zrem(redis_client_t *client, const char *key,
               int member_count, const char **members,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !members || member_count <= 0) return -1;
  const char *prefix[] = {"ZREM", key};
  return dispatch_prefix_array(client, prefix, 2, members, member_count,
                               callback, user_data);
}

int redis_zscore(redis_client_t *client, const char *key, const char *member,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"ZSCORE", key, member};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_zmscore(redis_client_t *client, const char *key,
                  int member_count, const char **members,
                  redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !members || member_count <= 0) return -1;
  const char *prefix[] = {"ZMSCORE", key};
  return dispatch_prefix_array(client, prefix, 2, members, member_count,
                               callback, user_data);
}

int redis_zincrby(redis_client_t *client, const char *key,
                  double increment, const char *member,
                  redis_command_cb_t callback, void *user_data) {
  char inc_str[64];
  fmt(inc_str, sizeof(inc_str), "{:.17g}", increment);
  const char *argv[] = {"ZINCRBY", key, inc_str, member};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_zrank(redis_client_t *client, const char *key, const char *member,
                redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"ZRANK", key, member};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_zrevrank(redis_client_t *client, const char *key, const char *member,
                   redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"ZREVRANK", key, member};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_zcard(redis_client_t *client, const char *key,
                redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"ZCARD", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_zcount(redis_client_t *client, const char *key,
                 const char *min, const char *max,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"ZCOUNT", key, min, max};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_zrange(redis_client_t *client, const char *key,
                 int64_t start, int64_t stop, int withscores,
                 redis_command_cb_t callback, void *user_data) {
  char s_str[32], e_str[32];
  fmt(s_str, sizeof(s_str), "{}", start);
  fmt(e_str, sizeof(e_str), "{}", stop);
  if (withscores) {
    const char *argv[] = {"ZRANGE", key, s_str, e_str, "WITHSCORES"};
    return redis_commandv(client, 5, argv, NULL, callback, user_data);
  }
  const char *argv[] = {"ZRANGE", key, s_str, e_str};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_zrevrange(redis_client_t *client, const char *key,
                    int64_t start, int64_t stop, int withscores,
                    redis_command_cb_t callback, void *user_data) {
  char s_str[32], e_str[32];
  fmt(s_str, sizeof(s_str), "{}", start);
  fmt(e_str, sizeof(e_str), "{}", stop);
  if (withscores) {
    const char *argv[] = {"ZREVRANGE", key, s_str, e_str, "WITHSCORES"};
    return redis_commandv(client, 5, argv, NULL, callback, user_data);
  }
  const char *argv[] = {"ZREVRANGE", key, s_str, e_str};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

static int zrangebyscore_common(redis_client_t *client, const char *cmd,
                                const char *key,
                                const char *bound1, const char *bound2,
                                int withscores,
                                int use_limit, int64_t offset, int64_t lcount,
                                redis_command_cb_t callback, void *user_data) {
  char off_str[32], cnt_str[32];
  int argc = 4;
  if (withscores) {
    if (calc_argc_mul_add(argc, 1, 1, &argc) < 0) return -1;
  }
  if (use_limit) {
    if (calc_argc_mul_add(argc, 1, 3, &argc) < 0) return -1;
  }
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  int idx = 0;
  argv[idx++] = cmd;
  argv[idx++] = key;
  argv[idx++] = bound1;
  argv[idx++] = bound2;
  if (withscores) argv[idx++] = "WITHSCORES";
  if (use_limit) {
    fmt(off_str, sizeof(off_str), "{}", offset);
    fmt(cnt_str, sizeof(cnt_str), "{}", lcount);
    argv[idx++] = "LIMIT";
    argv[idx++] = off_str;
    argv[idx++] = cnt_str;
  }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_zrangebyscore(redis_client_t *client, const char *key,
                        const char *min, const char *max,
                        int withscores,
                        int use_limit, int64_t offset, int64_t limit_count,
                        redis_command_cb_t callback, void *user_data) {
  return zrangebyscore_common(client, "ZRANGEBYSCORE", key, min, max,
                              withscores, use_limit, offset, limit_count,
                              callback, user_data);
}

int redis_zrevrangebyscore(redis_client_t *client, const char *key,
                           const char *max, const char *min,
                           int withscores,
                           int use_limit, int64_t offset, int64_t limit_count,
                           redis_command_cb_t callback, void *user_data) {
  return zrangebyscore_common(client, "ZREVRANGEBYSCORE", key, max, min,
                              withscores, use_limit, offset, limit_count,
                              callback, user_data);
}

int redis_zpopmin(redis_client_t *client, const char *key, int count,
                  redis_command_cb_t callback, void *user_data) {
  if (count <= 1) {
    const char *argv[] = {"ZPOPMIN", key};
    return redis_commandv(client, 2, argv, NULL, callback, user_data);
  }
  char cnt_str[32];
  fmt(cnt_str, sizeof(cnt_str), "{}", count);
  const char *argv[] = {"ZPOPMIN", key, cnt_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_zpopmax(redis_client_t *client, const char *key, int count,
                  redis_command_cb_t callback, void *user_data) {
  if (count <= 1) {
    const char *argv[] = {"ZPOPMAX", key};
    return redis_commandv(client, 2, argv, NULL, callback, user_data);
  }
  char cnt_str[32];
  fmt(cnt_str, sizeof(cnt_str), "{}", count);
  const char *argv[] = {"ZPOPMAX", key, cnt_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_zrangebylex(redis_client_t *client, const char *key,
                      const char *min, const char *max,
                      int use_limit, int64_t offset, int64_t limit_count,
                      redis_command_cb_t callback, void *user_data) {
  return zrangebyscore_common(client, "ZRANGEBYLEX", key, min, max,
                              0 /* no WITHSCORES */, use_limit, offset,
                              limit_count, callback, user_data);
}

int redis_zlexcount(redis_client_t *client, const char *key,
                    const char *min, const char *max,
                    redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"ZLEXCOUNT", key, min, max};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

/* =========================================================================
 * Key Management API
 * ========================================================================= */

int redis_type(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"TYPE", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_ttl(redis_client_t *client, const char *key,
              redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"TTL", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_pttl(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"PTTL", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_persist(redis_client_t *client, const char *key,
                  redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"PERSIST", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_expireat(redis_client_t *client, const char *key, int64_t timestamp,
                   redis_command_cb_t callback, void *user_data) {
  char ts_str[32];
  fmt(ts_str, sizeof(ts_str), "{}", timestamp);
  const char *argv[] = {"EXPIREAT", key, ts_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_pexpire(redis_client_t *client, const char *key, int64_t ms,
                  redis_command_cb_t callback, void *user_data) {
  char ms_str[32];
  fmt(ms_str, sizeof(ms_str), "{}", ms);
  const char *argv[] = {"PEXPIRE", key, ms_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_pexpireat(redis_client_t *client, const char *key, int64_t ts_ms,
                    redis_command_cb_t callback, void *user_data) {
  char ts_str[32];
  fmt(ts_str, sizeof(ts_str), "{}", ts_ms);
  const char *argv[] = {"PEXPIREAT", key, ts_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_rename(redis_client_t *client, const char *key, const char *newkey,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"RENAME", key, newkey};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_renamenx(redis_client_t *client, const char *key, const char *newkey,
                   redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"RENAMENX", key, newkey};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_unlink(redis_client_t *client, int key_count, const char **keys,
                 redis_command_cb_t callback, void *user_data) {
  if (!client || !keys || key_count <= 0) return -1;
  const char *prefix[] = {"UNLINK"};
  return dispatch_prefix_array(client, prefix, 1, keys, key_count,
                               callback, user_data);
}

int redis_scan(redis_client_t *client,
               const char *cursor, const char *pattern,
               size_t count, const char *type,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !cursor) return -1;
  char count_str[32];
  int argc = 2;
  if (pattern) {
    if (calc_argc_mul_add(argc, 1, 2, &argc) < 0) return -1;
  }
  if (count > 0) {
    if (calc_argc_mul_add(argc, 1, 2, &argc) < 0) return -1;
  }
  if (type) {
    if (calc_argc_mul_add(argc, 1, 2, &argc) < 0) return -1;
  }

  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  int idx = 0;
  argv[idx++] = "SCAN";
  argv[idx++] = cursor;
  if (pattern) { argv[idx++] = "MATCH"; argv[idx++] = pattern; }
  if (count > 0) {
    fmt(count_str, sizeof(count_str), "{}", count);
    argv[idx++] = "COUNT"; argv[idx++] = count_str;
  }
  if (type) { argv[idx++] = "TYPE"; argv[idx++] = type; }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_keys(redis_client_t *client, const char *pattern,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"KEYS", pattern ? pattern : "*"};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_copy(redis_client_t *client, const char *src, const char *dst,
               int dest_db, int replace,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !src || !dst) return -1;
  char db_str[32];
  int argc = 3;
  if (dest_db >= 0) {
    if (calc_argc_mul_add(argc, 1, 2, &argc) < 0) return -1;
  }
  if (replace) {
    if (calc_argc_mul_add(argc, 1, 1, &argc) < 0) return -1;
  }
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  int idx = 0;
  argv[idx++] = "COPY";
  argv[idx++] = src;
  argv[idx++] = dst;
  if (dest_db >= 0) {
    fmt(db_str, sizeof(db_str), "{}", dest_db);
    argv[idx++] = "DB"; argv[idx++] = db_str;
  }
  if (replace) argv[idx++] = "REPLACE";
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_object_encoding(redis_client_t *client, const char *key,
                          redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"OBJECT", "ENCODING", key};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_object_refcount(redis_client_t *client, const char *key,
                          redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"OBJECT", "REFCOUNT", key};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_object_idletime(redis_client_t *client, const char *key,
                          redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"OBJECT", "IDLETIME", key};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

/* =========================================================================
 * Server Commands API
 * ========================================================================= */

int redis_select(redis_client_t *client, int db,
                 redis_command_cb_t callback, void *user_data) {
  char db_str[32];
  fmt(db_str, sizeof(db_str), "{}", db);
  const char *argv[] = {"SELECT", db_str};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_dbsize(redis_client_t *client,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"DBSIZE"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_flushdb(redis_client_t *client, int async,
                  redis_command_cb_t callback, void *user_data) {
  if (async) {
    const char *argv[] = {"FLUSHDB", "ASYNC"};
    return redis_commandv(client, 2, argv, NULL, callback, user_data);
  }
  const char *argv[] = {"FLUSHDB"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_flushall(redis_client_t *client, int async,
                   redis_command_cb_t callback, void *user_data) {
  if (async) {
    const char *argv[] = {"FLUSHALL", "ASYNC"};
    return redis_commandv(client, 2, argv, NULL, callback, user_data);
  }
  const char *argv[] = {"FLUSHALL"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_info(redis_client_t *client, const char *section,
               redis_command_cb_t callback, void *user_data) {
  if (section) {
    const char *argv[] = {"INFO", section};
    return redis_commandv(client, 2, argv, NULL, callback, user_data);
  }
  const char *argv[] = {"INFO"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_config_get(redis_client_t *client, const char *parameter,
                     redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"CONFIG", "GET", parameter};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_config_set(redis_client_t *client, const char *parameter,
                     const char *value,
                     redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"CONFIG", "SET", parameter, value};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_config_resetstat(redis_client_t *client,
                           redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"CONFIG", "RESETSTAT"};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_debug_sleep(redis_client_t *client, double seconds,
                      redis_command_cb_t callback, void *user_data) {
  char sec_str[64];
  fmt(sec_str, sizeof(sec_str), "{:.6f}", seconds);
  const char *argv[] = {"DEBUG", "SLEEP", sec_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_time(redis_client_t *client,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"TIME"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_lastsave(redis_client_t *client,
                   redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"LASTSAVE"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_bgsave(redis_client_t *client,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"BGSAVE"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_bgrewriteaof(redis_client_t *client,
                       redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"BGREWRITEAOF"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_save(redis_client_t *client,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"SAVE"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

/* =========================================================================
 * Transaction API
 * ========================================================================= */

int redis_multi(redis_client_t *client,
                redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"MULTI"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_exec(redis_client_t *client,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"EXEC"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_discard(redis_client_t *client,
                  redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"DISCARD"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_watch(redis_client_t *client, int key_count, const char **keys,
                redis_command_cb_t callback, void *user_data) {
  if (!client || !keys || key_count <= 0) return -1;
  const char *prefix[] = {"WATCH"};
  return dispatch_prefix_array(client, prefix, 1, keys, key_count,
                               callback, user_data);
}

int redis_unwatch(redis_client_t *client,
                  redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"UNWATCH"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

/* =========================================================================
 * Scripting API
 * ========================================================================= */

int redis_eval(redis_client_t *client,
               const char *script,
               int key_count, const char **keys,
               int arg_count, const char **args,
               redis_command_cb_t callback, void *user_data) {
  return eval_common(client, "EVAL", script, key_count, keys,
                     arg_count, args, callback, user_data);
}

int redis_eval_result(redis_client_t *client,
                      const char *script,
                      int key_count, const char **keys,
                      int arg_count, const char **args,
                      redis_command_result_t *out) {
  return eval_common_result(client, "EVAL", script, key_count, keys,
                            arg_count, args, out);
}

int redis_evalsha(redis_client_t *client,
                  const char *sha1,
                  int key_count, const char **keys,
                  int arg_count, const char **args,
                  redis_command_cb_t callback, void *user_data) {
  return eval_common(client, "EVALSHA", sha1, key_count, keys,
                     arg_count, args, callback, user_data);
}

int redis_evalsha_result(redis_client_t *client,
                         const char *sha1,
                         int key_count, const char **keys,
                         int arg_count, const char **args,
                         redis_command_result_t *out) {
  return eval_common_result(client, "EVALSHA", sha1, key_count, keys,
                            arg_count, args, out);
}

int redis_script_load(redis_client_t *client, const char *script,
                      redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"SCRIPT", "LOAD", script};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_script_load_result(redis_client_t *client, const char *script,
                             redis_command_result_t *out) {
  const char *argv[] = {"SCRIPT", "LOAD", script};
  if (!out) return TURBO_EINVAL;
  *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
  if (!client || !script) {
    out->status = TURBO_EINVAL;
    return out->status;
  }
  return redis_commandv_result(client, 3, argv, NULL, out);
}

int redis_script_exists(redis_client_t *client,
                        int sha_count, const char **sha1s,
                        redis_command_cb_t callback, void *user_data) {
  if (!client || !sha1s || sha_count <= 0) return -1;
  const char *prefix[] = {"SCRIPT", "EXISTS"};
  return dispatch_prefix_array(client, prefix, 2, sha1s, sha_count,
                               callback, user_data);
}

int redis_script_flush(redis_client_t *client,
                       redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"SCRIPT", "FLUSH"};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

/* =========================================================================
 * HyperLogLog API
 * ========================================================================= */

int redis_pfadd(redis_client_t *client, const char *key,
                int element_count, const char **elements,
                redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !elements || element_count <= 0) return -1;
  const char *prefix[] = {"PFADD", key};
  return dispatch_prefix_array(client, prefix, 2, elements, element_count,
                               callback, user_data);
}

int redis_pfcount(redis_client_t *client, int key_count, const char **keys,
                  redis_command_cb_t callback, void *user_data) {
  if (!client || !keys || key_count <= 0) return -1;
  const char *prefix[] = {"PFCOUNT"};
  return dispatch_prefix_array(client, prefix, 1, keys, key_count,
                               callback, user_data);
}

int redis_pfmerge(redis_client_t *client, const char *destkey,
                  int src_key_count, const char **src_keys,
                  redis_command_cb_t callback, void *user_data) {
  if (!client || !destkey || !src_keys || src_key_count <= 0) return -1;
  const char *prefix[] = {"PFMERGE", destkey};
  return dispatch_prefix_array(client, prefix, 2, src_keys, src_key_count,
                               callback, user_data);
}

/* =========================================================================
 * Geo API
 * ========================================================================= */

int redis_geoadd(redis_client_t *client, const char *key,
                 int item_count, const double *longitudes, const double *latitudes, const char **members,
                 redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !longitudes || !latitudes || !members || item_count <= 0) return -1;
  int argc = 0;
  if (calc_argc_mul_add(2, item_count, 3, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  argv[0] = "GEOADD";
  argv[1] = key;
  if ((size_t)item_count > SIZE_MAX / sizeof(char[2][64])) return -1;
  char (*coords_str)[2][64] = malloc((size_t)item_count * sizeof(*coords_str));
  if (!coords_str) {
    redis_argv_free(argv);
    return -1;
  }
  for (int i = 0; i < item_count; i++) {
    fmt(coords_str[i][0], 64, "{:.17g}", longitudes[i]);
    fmt(coords_str[i][1], 64, "{:.17g}", latitudes[i]);
    argv[2 + i * 3]     = coords_str[i][0];
    argv[2 + i * 3 + 1] = coords_str[i][1];
    argv[2 + i * 3 + 2] = members[i];
  }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  free(coords_str);
  redis_argv_free(argv);
  return result;
}

int redis_geodist(redis_client_t *client, const char *key,
                  const char *member1, const char *member2, const char *unit,
                  redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !member1 || !member2) return -1;
  if (unit) {
    const char *argv[] = {"GEODIST", key, member1, member2, unit};
    return redis_commandv(client, 5, argv, NULL, callback, user_data);
  }
  const char *argv[] = {"GEODIST", key, member1, member2};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_geopos(redis_client_t *client, const char *key,
                 int member_count, const char **members,
                 redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !members || member_count <= 0) return -1;
  const char *prefix[] = {"GEOPOS", key};
  return dispatch_prefix_array(client, prefix, 2, members, member_count,
                               callback, user_data);
}

int redis_georadius(redis_client_t *client, const char *key,
                    double longitude, double latitude, double radius, const char *unit,
                    int withcoord, int withdist, int withhash,
                    int count, const char *order,
                    redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !unit) return -1;
  char lon_str[64], lat_str[64], rad_str[64], count_str[32];
  fmt(lon_str, sizeof(lon_str), "{:.17g}", longitude);
  fmt(lat_str, sizeof(lat_str), "{:.17g}", latitude);
  fmt(rad_str, sizeof(rad_str), "{:.17g}", radius);

  const char *argv[15];
  int idx = 0;
  argv[idx++] = "GEORADIUS";
  argv[idx++] = key;
  argv[idx++] = lon_str;
  argv[idx++] = lat_str;
  argv[idx++] = rad_str;
  argv[idx++] = unit;
  if (withcoord) argv[idx++] = "WITHCOORD";
  if (withdist) argv[idx++] = "WITHDIST";
  if (withhash) argv[idx++] = "WITHHASH";
  if (order) argv[idx++] = order;
  if (count > 0) {
    fmt(count_str, sizeof(count_str), "{}", count);
    argv[idx++] = "COUNT";
    argv[idx++] = count_str;
  }
  return redis_commandv(client, idx, argv, NULL, callback, user_data);
}

int redis_georadiusbymember(redis_client_t *client, const char *key,
                            const char *member, double radius, const char *unit,
                            int withcoord, int withdist, int withhash,
                            int count, const char *order,
                            redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !member || !unit) return -1;
  char rad_str[64], count_str[32];
  fmt(rad_str, sizeof(rad_str), "{:.17g}", radius);

  const char *argv[15];
  int idx = 0;
  argv[idx++] = "GEORADIUSBYMEMBER";
  argv[idx++] = key;
  argv[idx++] = member;
  argv[idx++] = rad_str;
  argv[idx++] = unit;
  if (withcoord) argv[idx++] = "WITHCOORD";
  if (withdist) argv[idx++] = "WITHDIST";
  if (withhash) argv[idx++] = "WITHHASH";
  if (order) argv[idx++] = order;
  if (count > 0) {
    fmt(count_str, sizeof(count_str), "{}", count);
    argv[idx++] = "COUNT";
    argv[idx++] = count_str;
  }
  return redis_commandv(client, idx, argv, NULL, callback, user_data);
}

int redis_geosearch(redis_client_t *client, const char *key,
                    const char *from_member, const double *from_lonlat,
                    const double *by_radius, const char *radius_unit,
                    const double *by_box, const char *box_unit,
                    const char *order, int count,
                    redis_command_cb_t callback, void *user_data) {
  if (!client || !key) return -1;
  const char *argv[20];
  int idx = 0;
  argv[idx++] = "GEOSEARCH";
  argv[idx++] = key;

  char lon_str[64], lat_str[64], rad_str[64], w_str[64], h_str[64], count_str[32];

  if (from_member) {
    argv[idx++] = "FROMMEMBER";
    argv[idx++] = from_member;
  } else if (from_lonlat) {
    fmt(lon_str, sizeof(lon_str), "{:.17g}", from_lonlat[0]);
    fmt(lat_str, sizeof(lat_str), "{:.17g}", from_lonlat[1]);
    argv[idx++] = "FROMLONLAT";
    argv[idx++] = lon_str;
    argv[idx++] = lat_str;
  }

  if (by_radius && radius_unit) {
    fmt(rad_str, sizeof(rad_str), "{:.17g}", *by_radius);
    argv[idx++] = "BYRADIUS";
    argv[idx++] = rad_str;
    argv[idx++] = radius_unit;
  } else if (by_box && box_unit) {
    fmt(w_str, sizeof(w_str), "{:.17g}", by_box[0]);
    fmt(h_str, sizeof(h_str), "{:.17g}", by_box[1]);
    argv[idx++] = "BYBOX";
    argv[idx++] = w_str;
    argv[idx++] = h_str;
    argv[idx++] = box_unit;
  }

  if (order) argv[idx++] = order;
  if (count > 0) {
    fmt(count_str, sizeof(count_str), "{}", count);
    argv[idx++] = "COUNT";
    argv[idx++] = count_str;
  }
  return redis_commandv(client, idx, argv, NULL, callback, user_data);
}

/* =========================================================================
 * Bitmaps API
 * ========================================================================= */

int redis_setbit(redis_client_t *client, const char *key, int64_t offset, int value,
                 redis_command_cb_t callback, void *user_data) {
  char off_str[32], val_str[16];
  fmt(off_str, sizeof(off_str), "{}", offset);
  fmt(val_str, sizeof(val_str), "{}", value);
  const char *argv[] = {"SETBIT", key, off_str, val_str};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_getbit(redis_client_t *client, const char *key, int64_t offset,
                 redis_command_cb_t callback, void *user_data) {
  char off_str[32];
  fmt(off_str, sizeof(off_str), "{}", offset);
  const char *argv[] = {"GETBIT", key, off_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_bitcount(redis_client_t *client, const char *key,
                   int has_range, int64_t start, int64_t end, const char *unit,
                   redis_command_cb_t callback, void *user_data) {
  if (!client || !key) return -1;
  char s_str[32], e_str[32];
  if (has_range) {
    fmt(s_str, sizeof(s_str), "{}", start);
    fmt(e_str, sizeof(e_str), "{}", end);
    if (unit) {
      const char *argv[] = {"BITCOUNT", key, s_str, e_str, unit};
      return redis_commandv(client, 5, argv, NULL, callback, user_data);
    }
    const char *argv[] = {"BITCOUNT", key, s_str, e_str};
    return redis_commandv(client, 4, argv, NULL, callback, user_data);
  }
  const char *argv[] = {"BITCOUNT", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_bitop(redis_client_t *client, const char *operation, const char *destkey,
                int key_count, const char **keys,
                redis_command_cb_t callback, void *user_data) {
  if (!client || !operation || !destkey || !keys || key_count <= 0) return -1;
  const char *prefix[] = {"BITOP", operation, destkey};
  return dispatch_prefix_array(client, prefix, 3, keys, key_count,
                               callback, user_data);
}

int redis_bitpos(redis_client_t *client, const char *key, int bit,
                 int has_range, int64_t start, int64_t end, const char *unit,
                 redis_command_cb_t callback, void *user_data) {
  if (!client || !key) return -1;
  char bit_str[16], s_str[32], e_str[32];
  fmt(bit_str, sizeof(bit_str), "{}", bit);
  if (has_range) {
    fmt(s_str, sizeof(s_str), "{}", start);
    fmt(e_str, sizeof(e_str), "{}", end);
    if (unit) {
      const char *argv[] = {"BITPOS", key, bit_str, s_str, e_str, unit};
      return redis_commandv(client, 6, argv, NULL, callback, user_data);
    }
    const char *argv[] = {"BITPOS", key, bit_str, s_str, e_str};
    return redis_commandv(client, 5, argv, NULL, callback, user_data);
  }
  const char *argv[] = {"BITPOS", key, bit_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_bitfield(redis_client_t *client, const char *key,
                   int command_count, const char **commands,
                   redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !commands || command_count <= 0) return -1;
  const char *prefix[] = {"BITFIELD", key};
  return dispatch_prefix_array(client, prefix, 2, commands, command_count,
                               callback, user_data);
}

/* =========================================================================
 * Client/Connection API
 * ========================================================================= */

int redis_auth(redis_client_t *client, const char *username, const char *password,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !password) return -1;
  if (username) {
    const char *argv[] = {"AUTH", username, password};
    return redis_commandv(client, 3, argv, NULL, callback, user_data);
  }
  const char *argv[] = {"AUTH", password};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_client_setname(redis_client_t *client, const char *name,
                         redis_command_cb_t callback, void *user_data) {
  if (!client || !name) return -1;
  const char *argv[] = {"CLIENT", "SETNAME", name};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_client_getname(redis_client_t *client,
                         redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"CLIENT", "GETNAME"};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_client_list(redis_client_t *client,
                      redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"CLIENT", "LIST"};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_client_kill(redis_client_t *client, int filter_count, const char **filters, const char **values,
                      redis_command_cb_t callback, void *user_data) {
  if (!client || !filters || !values || filter_count <= 0) return -1;
  int argc = 0;
  if (calc_argc_mul_add(2, filter_count, 2, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  argv[0] = "CLIENT";
  argv[1] = "KILL";
  for (int i = 0; i < filter_count; i++) {
    argv[2 + i * 2]     = filters[i];
    argv[2 + i * 2 + 1] = values[i];
  }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_hello(redis_client_t *client, const char *protover, const char *username, const char *password, const char *clientname,
                 redis_command_cb_t callback, void *user_data) {
  if (!client) return -1;
  const char *argv[10];
  int idx = 0;
  argv[idx++] = "HELLO";
  if (protover) argv[idx++] = protover;
  if (username && password) {
    argv[idx++] = "AUTH";
    argv[idx++] = username;
    argv[idx++] = password;
  }
  if (clientname) {
    argv[idx++] = "SETNAME";
    argv[idx++] = clientname;
  }
  return redis_commandv(client, idx, argv, NULL, callback, user_data);
}

int redis_shutdown(redis_client_t *client, const char *save_mode,
                   redis_command_cb_t callback, void *user_data) {
  if (!client) return -1;
  if (save_mode) {
    const char *argv[] = {"SHUTDOWN", save_mode};
    return redis_commandv(client, 2, argv, NULL, callback, user_data);
  }
  const char *argv[] = {"SHUTDOWN"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}
