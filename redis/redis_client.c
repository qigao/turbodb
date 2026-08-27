#include "redis_client.h"
#include "redis_internal.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_socket.h"
#include "turbo_str.h"
#include <fmt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tlog.h"

#define calc_argc_mul_add redis_argc_mul_add
#define alloc_argv redis_argv_alloc

/* Default configuration */
static redis_config_t default_config = {.host = "127.0.0.1",
                                         .port = 6379,
                                        .username = NULL,
                                        .password = NULL,
                                        .database = 0,
                                        .timeout_ms = 5000,
                                        .command_timeout_ms = 5000,
                                        .max_pipeline = 100,
                                        .cluster_readonly = 0};

struct redis_command_stream_s {
  redis_client_t *client;
  tstr command;
  redis_resp_array_reader reader;
  size_t max_buffer_bytes;
  int sent;
  int terminal;
};

/* Forward declarations */
static char *build_resp_command(int argc, const char **argv, const size_t *argvlen,
                                size_t *out_len);
static void reset_command_queue_state(redis_client_t *client);
static void redis_client_socket_publish(redis_client_t *client, coro_socket_t *socket,
                                        int owned, int connected);
static coro_socket_t *redis_client_socket_take(redis_client_t *client, int *owned);

static void reset_command_queue_state(redis_client_t *client) {
  if (!client) return;

  client->command_queue = NULL;
  client->command_queue_tail = NULL;
  client->queued_commands = 0;
}

redis_client_t *redis_client_create(const char *host, uint16_t port) {
  redis_config_t config = default_config;
  config.host = host;
  config.port = port;
  return redis_client_create_with_config(&config);
}

redis_client_t *redis_client_create_with_config(const redis_config_t *config) {
  redis_config_t normalized;
  if (!config || !config->host || !config->host[0] || config->port == 0 ||
      config->database < 0 || config->database > 15 ||
      (config->username && !config->password))
    return NULL;

  normalized = *config;
  if (normalized.timeout_ms == 0) normalized.timeout_ms = default_config.timeout_ms;
  if (normalized.command_timeout_ms == 0)
    normalized.command_timeout_ms = normalized.timeout_ms;
  if (normalized.max_pipeline == 0) normalized.max_pipeline = default_config.max_pipeline;

  redis_client_t *client = calloc(1, sizeof(redis_client_t));
  if (!client)
    return NULL;
  turbo_mutex_init(&client->socket_mutex);

  /* Copy configuration */
  client->config = normalized;
  client->config.host = tstr_dup(normalized.host);
  client->config.username = normalized.username ? tstr_dup(normalized.username) : NULL;
  client->config.password = normalized.password ? tstr_dup(normalized.password) : NULL;
  if (!client->config.host || (normalized.username && !client->config.username) ||
      (normalized.password && !client->config.password)) {
    tstr_free((tstr)client->config.host);
    tstr_free((tstr)client->config.username);
    tstr_free((tstr)client->config.password);
    turbo_mutex_destroy(&client->socket_mutex);
    free(client);
    return NULL;
  }

  if (redis_recv_buffer_init(client) != 0) {
    tstr_free((tstr)client->config.host);
    tstr_free((tstr)client->config.username);
    tstr_free((tstr)client->config.password);
    turbo_mutex_destroy(&client->socket_mutex);
    free(client);
    return NULL;
  }

  return client;
}

int redis_client_attach_socket(redis_client_t *client,
                               coro_context_t *ctx,
                               coro_socket_t *socket,
                               int take_ownership) {
  if (!client || !socket) {
    return -1;
  }

  redis_client_disconnect(client);
  reset_command_queue_state(client);
  client->recv_buffer_used = 0;
  client->ctx = ctx;
  redis_client_socket_publish(client, socket, take_ownership ? 1 : 0, 1);
  coro_socket_set_timeout(socket, client->config.command_timeout_ms);
  return 0;
}

coro_socket_t *redis_client_detach_socket(redis_client_t *client) {
  coro_socket_t *socket;

  if (!client) return NULL;
  socket = redis_client_socket_take(client, NULL);
  return socket;
}

static int redis_reply_is_ok(const redis_reply_t *reply) {
  return reply && reply->type == REDIS_REPLY_STRING && reply->str &&
         reply->len == 2u && memcmp(reply->str, "OK", 2u) == 0;
}

int redis_client_prepare(redis_client_t *client) {
  redis_command_result_t command = REDIS_COMMAND_RESULT_INIT;
  int status = TURBO_OK;

  if (!client) return TURBO_EINVAL;
  if (!client->is_connected || !client->socket) return TURBO_ENOTCONN;

  if (client->config.password && !client->is_authenticated) {
    const char *auth2[] = {"AUTH", client->config.password};
    const char *auth3[] = {"AUTH", client->config.username, client->config.password};
    status = redis_commandv_result(client, client->config.username ? 3 : 2,
                                   client->config.username ? auth3 : auth2,
                                   NULL, &command);
    if (status == TURBO_OK && !redis_reply_is_ok(command.reply)) status = TURBO_EPROTO;
    redis_command_result_clear(&command);
    if (status != TURBO_OK) return status;
    client->is_authenticated = 1;
  }

  if (client->selected_db != client->config.database) {
    char database[16];
    const char *select_argv[] = {"SELECT", database};
    if (fmt(database, sizeof(database), "{}", client->config.database) < 0)
      return TURBO_EIO;
    status = redis_commandv_result(client, 2, select_argv, NULL, &command);
    if (status == TURBO_OK && !redis_reply_is_ok(command.reply)) status = TURBO_EPROTO;
    redis_command_result_clear(&command);
    if (status != TURBO_OK) return status;
    client->selected_db = client->config.database;
  }

  if (client->config.cluster_readonly && !client->is_cluster_readonly) {
    const char *readonly_argv[] = {"READONLY"};
    status = redis_commandv_result(client, 1, readonly_argv, NULL, &command);
    if (status == TURBO_OK && !redis_reply_is_ok(command.reply)) status = TURBO_EPROTO;
    redis_command_result_clear(&command);
    if (status != TURBO_OK) return status;
    client->is_cluster_readonly = 1;
  }

  return TURBO_OK;
}

int redis_client_connect(redis_client_t *client, redis_connect_cb_t callback, void *user_data) {
  int status;

  if (!client)
    return TURBO_EINVAL;

  redis_client_disconnect(client);
  reset_command_queue_state(client);
  client->recv_buffer_used = 0;
  client->ctx = coro_context_current();
  client->connect_cb = callback;
  client->connect_user_data = user_data;
  client->is_connected = 0;
  client->is_authenticated = 0;
  client->selected_db = 0;
  client->is_cluster_readonly = 0;

  if (!client->ctx) {
    if (client->connect_cb) {
      client->connect_cb(client, TURBO_EINVAL, client->connect_user_data);
    }
    return TURBO_EINVAL;
  }

  {
    coro_socket_t *socket = coro_socket_create_tcpv4(client->ctx);
    if (socket) redis_client_socket_publish(client, socket, 1, 0);
  }
  if (!client->socket) {
    if (client->connect_cb) {
      client->connect_cb(client, TURBO_ENOMEM, client->connect_user_data);
    }
    return TURBO_ENOMEM;
  }

  coro_socket_set_timeout(client->socket, client->config.timeout_ms);
  status = coro_socket_connect(client->socket, client->config.host, (int)client->config.port);
  if (status != 0) {
    int owned = 0;
    coro_socket_t *socket = redis_client_socket_take(client, &owned);
    if (socket && owned) coro_socket_destroy(socket);
    if (client->connect_cb) {
      client->connect_cb(client, status, client->connect_user_data);
    }
    return status;
  }

  turbo_mutex_lock(&client->socket_mutex);
  client->is_connected = 1;
  turbo_mutex_unlock(&client->socket_mutex);
  coro_socket_set_timeout(client->socket, client->config.command_timeout_ms);
  status = redis_client_prepare(client);
  if (status != TURBO_OK) goto handshake_failed;

  if (client->connect_cb) {
    client->connect_cb(client, TURBO_OK, client->connect_user_data);
  }

  return TURBO_OK;

handshake_failed:
  redis_client_disconnect(client);
  if (client->connect_cb)
    client->connect_cb(client, status, client->connect_user_data);
  return status;
}

void redis_command_result_clear(redis_command_result_t *result) {
  if (!result) return;
  redis_reply_free(result->reply);
  *result = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
}

static int redis_error_token_is(const redis_reply_t *reply, const char *token) {
  size_t token_len;
  if (!reply || reply->type != REDIS_REPLY_ERROR || !reply->str || !token) return 0;
  token_len = strlen(token);
  return reply->len >= token_len && memcmp(reply->str, token, token_len) == 0 &&
         (reply->len == token_len || reply->str[token_len] == ' ');
}

redis_server_error_t redis_server_error_classify(const redis_reply_t *reply) {
  if (!reply || reply->type != REDIS_REPLY_ERROR) return REDIS_SERVER_ERROR_NONE;
  if (redis_error_token_is(reply, "ERR")) return REDIS_SERVER_ERROR_ERR;
  if (redis_error_token_is(reply, "BUSYGROUP")) return REDIS_SERVER_ERROR_BUSY_GROUP;
  if (redis_error_token_is(reply, "NOGROUP")) return REDIS_SERVER_ERROR_NO_GROUP;
  if (redis_error_token_is(reply, "WRONGTYPE")) return REDIS_SERVER_ERROR_WRONG_TYPE;
  if (redis_error_token_is(reply, "NOAUTH")) return REDIS_SERVER_ERROR_NO_AUTH;
  if (redis_error_token_is(reply, "MOVED")) return REDIS_SERVER_ERROR_MOVED;
  if (redis_error_token_is(reply, "ASK")) return REDIS_SERVER_ERROR_ASK;
  if (redis_error_token_is(reply, "TRYAGAIN")) return REDIS_SERVER_ERROR_TRY_AGAIN;
  if (redis_error_token_is(reply, "CLUSTERDOWN")) return REDIS_SERVER_ERROR_CLUSTER_DOWN;
  if (redis_error_token_is(reply, "READONLY")) return REDIS_SERVER_ERROR_READ_ONLY;
  if (redis_error_token_is(reply, "NOSCRIPT")) return REDIS_SERVER_ERROR_NO_SCRIPT;
  if (redis_error_token_is(reply, "LOADING")) return REDIS_SERVER_ERROR_LOADING;
  if (redis_error_token_is(reply, "OOM")) return REDIS_SERVER_ERROR_OOM;
  if (redis_error_token_is(reply, "EXECABORT")) return REDIS_SERVER_ERROR_EXEC_ABORT;
  if (redis_error_token_is(reply, "MASTERDOWN")) return REDIS_SERVER_ERROR_MASTER_DOWN;
  if (redis_error_token_is(reply, "MISCONF")) return REDIS_SERVER_ERROR_MISCONF;
  return REDIS_SERVER_ERROR_UNKNOWN;
}

/* Build RESP command */
static char *build_resp_command(int argc, const char **argv, const size_t *argvlen,
                                size_t *out_len) {
  tstr command;
  tstr next;
  if (argc <= 0 || !argv || !out_len) {
    return NULL;
  }

  /* Calculate total size */
  size_t total = 1u + 20u + 2u; /* *<count>\r\n */

  for (int i = 0; i < argc; i++) {
    size_t len = argvlen ? argvlen[i] : strlen(argv[i]);
    const size_t framing = 1u + 20u + 2u + 2u; /* $<len>\r\n<data>\r\n */
    if (len > SIZE_MAX - framing || total > SIZE_MAX - framing - len) return NULL;
    total += framing + len;
  }

  command = tstr_new();
  if (!command) return NULL;
  next = tstr_reserve(command, total);
  if (!next) {
    tstr_free(command);
    return NULL;
  }
  command = next;
  next = tstr_append_format(command, "*{}\r\n", argc);
  if (!next) {
    tstr_free(command);
    return NULL;
  }
  command = next;

  for (int i = 0; i < argc; i++) {
    size_t len = argvlen ? argvlen[i] : strlen(argv[i]);
    next = tstr_append_format(command, "${}\r\n", len);
    if (!next) {
      tstr_free(command);
      return NULL;
    }
    command = next;
    next = tstr_cat_v(command, vstr_from_buf(argv[i], len));
    if (!next) {
      tstr_free(command);
      return NULL;
    }
    command = next;
    next = tstr_cat_len(command, "\r\n", 2u);
    if (!next) {
      tstr_free(command);
      return NULL;
    }
    command = next;
  }

  *out_len = tstr_len(command);
  return command;
}

int redis_commandv_result(redis_client_t *client, int argc, const char **argv,
                          const size_t *argvlen, redis_command_result_t *out) {
  size_t cmd_len;
  char *cmd_str;
  int rc;

  if (!out) return TURBO_EINVAL;
  *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;

  if (client && client->active_stream != NULL) {
    out->status = TURBO_EBUSY;
    return out->status;
  }
  if (!client || argc <= 0 || !argv) {
    out->status = TURBO_EINVAL;
    return out->status;
  }
  if (!client->is_connected || !client->socket) {
    out->status = TURBO_ENOTCONN;
    return out->status;
  }
  for (int i = 0; i < argc; ++i) {
    if (!argv[i]) {
      out->status = TURBO_EINVAL;
      return out->status;
    }
  }

  cmd_str = build_resp_command(argc, argv, argvlen, &cmd_len);
  if (!cmd_str) {
    out->status = TURBO_ENOMEM;
    return out->status;
  }

  rc = coro_socket_send(client->socket, cmd_str, cmd_len);
  if (rc != 0) {
    tstr_free((tstr)cmd_str);
    client->is_connected = 0;
    out->status = rc;
    out->outcome = REDIS_COMMAND_SEND_UNCERTAIN;
    return out->status;
  }
  tstr_free((tstr)cmd_str);

  for (;;) {
    redis_reply_t *reply = NULL;
    int parsed = redis_parse_resp_reply(client, &reply);
    if (parsed < 0) {
      client->is_connected = 0;
      out->status = TURBO_EPROTO;
      out->outcome = REDIS_COMMAND_REPLY_UNKNOWN;
      return out->status;
    }
    if (parsed > 0) {
      memmove(client->recv_buffer, client->recv_buffer + parsed,
              client->recv_buffer_used - (size_t)parsed);
      client->recv_buffer_used -= (size_t)parsed;
      out->reply = reply;
      out->outcome = REDIS_COMMAND_REPLIED;
      out->server_error = redis_server_error_classify(reply);
      out->status = out->server_error == REDIS_SERVER_ERROR_NONE ? TURBO_OK : TURBO_EIO;
      return out->status;
    }

    char *data = NULL;
    size_t len = 0;
    int recv_rc = coro_socket_recv(client->socket, &data, &len);
    if (data != NULL && len > 0U) {
      recv_rc = 0;
    }
    if (recv_rc != 0) {
      if (data != NULL) {
        coro_socket_free_recv(data);
      }
      client->is_connected = 0;
      out->status = recv_rc;
      out->outcome = REDIS_COMMAND_REPLY_UNKNOWN;
      return out->status;
    }

    if (redis_recv_buffer_append(client, data, len) != 0) {
      coro_socket_free_recv(data);
      client->is_connected = 0;
      out->status = TURBO_ENOMEM;
      out->outcome = REDIS_COMMAND_REPLY_UNKNOWN;
      return out->status;
    }
    coro_socket_free_recv(data);
  }
}

int redis_commandv_stream_open(redis_client_t *client, int argc,
                               const char **argv, const size_t *argvlen,
                               size_t max_buffer_bytes, size_t max_items,
                               redis_command_stream_t **out_stream) {
  redis_command_stream_t *stream;
  size_t command_size = 0u;
  int index;
  if (!out_stream) return TURBO_EINVAL;
  *out_stream = NULL;
  if (!client || !client->is_connected || !client->socket || argc <= 0 ||
      !argv || max_buffer_bytes == 0u || max_items == 0u ||
      client->active_stream != NULL)
    return client && client->active_stream ? TURBO_EBUSY : TURBO_EINVAL;
  if (client->recv_buffer_used > max_buffer_bytes)
    return TURBO_ENOBUFS;
  for (index = 0; index < argc; ++index)
    if (!argv[index]) return TURBO_EINVAL;
  stream = (redis_command_stream_t *)calloc(1u, sizeof(*stream));
  if (!stream) return TURBO_ENOMEM;
  stream->command = build_resp_command(argc, argv, argvlen, &command_size);
  if (!stream->command) {
    free(stream);
    return TURBO_ENOMEM;
  }
  stream->client = client;
  stream->max_buffer_bytes = max_buffer_bytes;
  redis_resp_array_reader_init(&stream->reader, max_items,
                               max_buffer_bytes);
  client->active_stream = stream;
  *out_stream = stream;
  return TURBO_OK;
}

static redis_command_stream_step_t redis_command_stream_error(
    redis_command_stream_t *stream, int status,
    redis_command_outcome_t outcome, redis_reply_t *item) {
  redis_command_stream_step_t step = REDIS_COMMAND_STREAM_STEP_INIT;
  step.kind = REDIS_COMMAND_STREAM_ERROR;
  step.status = status;
  step.outcome = outcome;
  step.item = item;
  step.server_error = redis_server_error_classify(item);
  stream->terminal = 1;
  if (stream->client->active_stream == stream)
    stream->client->active_stream = NULL;
  return step;
}

redis_command_stream_step_t redis_command_stream_next(
    redis_command_stream_t *stream) {
  redis_command_stream_step_t step = REDIS_COMMAND_STREAM_STEP_INIT;
  redis_client_t *client;
  if (!stream || !stream->client) {
    step.kind = REDIS_COMMAND_STREAM_ERROR;
    step.status = TURBO_EINVAL;
    return step;
  }
  client = stream->client;
  if (stream->terminal)
    return step;
  if (!client->is_connected || client->socket == NULL)
    return redis_command_stream_error(
        stream, TURBO_ENOTCONN,
        stream->sent ? REDIS_COMMAND_REPLY_UNKNOWN : REDIS_COMMAND_NOT_SENT,
        NULL);
  if (!stream->sent) {
    int send_status = coro_socket_send(client->socket, stream->command,
                                       tstr_len(stream->command));
    if (send_status != TURBO_OK) {
      client->is_connected = 0;
      redis_client_disconnect(client);
      client->recv_buffer_used = 0u;
      return redis_command_stream_error(
          stream, send_status, REDIS_COMMAND_SEND_UNCERTAIN, NULL);
    }
    stream->sent = 1;
    tstr_freep(&stream->command);
  }
  for (;;) {
    redis_reply_t *item = NULL;
    redis_resp_array_step parsed =
        redis_resp_array_reader_next(client, &stream->reader, &item);
    if (parsed == REDIS_RESP_ARRAY_ITEM) {
      step.kind = REDIS_COMMAND_STREAM_ITEM;
      step.status = TURBO_OK;
      step.outcome = REDIS_COMMAND_REPLIED;
      step.item = item;
      return step;
    }
    if (parsed == REDIS_RESP_ARRAY_DONE) {
      stream->terminal = 1;
      if (client->active_stream == stream)
        client->active_stream = NULL;
      step.outcome = REDIS_COMMAND_REPLIED;
      return step;
    }
    if (parsed == REDIS_RESP_ARRAY_SERVER_ERROR)
      return redis_command_stream_error(stream, TURBO_EIO,
                                        REDIS_COMMAND_REPLIED, item);
    if (parsed == REDIS_RESP_ARRAY_LIMIT) {
      client->is_connected = 0;
      redis_client_disconnect(client);
      client->recv_buffer_used = 0u;
      return redis_command_stream_error(stream, TURBO_ENOBUFS,
                                        REDIS_COMMAND_REPLY_UNKNOWN, NULL);
    }
    if (parsed == REDIS_RESP_ARRAY_ERROR) {
      client->is_connected = 0;
      redis_client_disconnect(client);
      client->recv_buffer_used = 0u;
      return redis_command_stream_error(stream, TURBO_EPROTO,
                                        REDIS_COMMAND_REPLY_UNKNOWN, NULL);
    }
    {
      char *data = NULL;
      size_t length = 0u;
      int receive_status = coro_socket_recv(client->socket, &data, &length);
      if (data != NULL && length != 0u)
        receive_status = TURBO_OK;
      if (receive_status != TURBO_OK) {
        coro_socket_free_recv(data);
        client->is_connected = 0;
        redis_client_disconnect(client);
        client->recv_buffer_used = 0u;
        return redis_command_stream_error(
            stream, receive_status, REDIS_COMMAND_REPLY_UNKNOWN, NULL);
      }
      receive_status = redis_recv_buffer_append_bounded(
          client, data, length, stream->max_buffer_bytes);
      coro_socket_free_recv(data);
      if (receive_status != 0) {
        client->is_connected = 0;
        redis_client_disconnect(client);
        client->recv_buffer_used = 0u;
        return redis_command_stream_error(
            stream, receive_status == -2 ? TURBO_ENOBUFS : TURBO_ENOMEM,
            REDIS_COMMAND_REPLY_UNKNOWN, NULL);
      }
    }
  }
}

void redis_command_stream_destroy(redis_command_stream_t *stream) {
  if (!stream) return;
  if (stream->client && stream->client->active_stream == stream) {
    if (stream->sent && !stream->terminal) {
      redis_client_disconnect(stream->client);
      stream->client->recv_buffer_used = 0u;
    }
    stream->client->active_stream = NULL;
  }
  tstr_free(stream->command);
  free(stream);
}

int redis_commandv(redis_client_t *client, int argc, const char **argv, const size_t *argvlen,
                   redis_command_cb_t callback, void *user_data) {
  redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
  int rc = redis_commandv_result(client, argc, argv, argvlen, &result);
  if (result.outcome == REDIS_COMMAND_REPLIED && callback)
    callback(client, result.reply, user_data);
  rc = result.outcome == REDIS_COMMAND_REPLIED ? 0 : -1;
  redis_command_result_clear(&result);
  return rc;
}

int redis_command(redis_client_t *client, redis_command_cb_t callback, void *user_data,
                  const char *format, ...) {
  va_list ap;
  int argc = 0;
  const char *argv[32];
  char arg_buf[32][256];

  if (!format) {
    return -1;
  }

  /* Parse format string */
  va_start(ap, format);
  const char *p = format;

  while (*p && argc < 32) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;

    if (*p == '%') {
      p++;
      if (*p == 's') {
        argv[argc++] = va_arg(ap, const char *);
      } else if (*p == 'd') {
        int val = va_arg(ap, int);
        fmt(arg_buf[argc], sizeof(arg_buf[argc]), "{}", val);
        argv[argc++] = arg_buf[argc - 1];
      }
      p++;
    } else {
      const char *start = p;
      while (*p && *p != ' ' && *p != '%')
        p++;
      size_t len = p - start;
      if (len < sizeof(arg_buf[argc])) {
        memcpy(arg_buf[argc], start, len);
        arg_buf[argc][len] = '\0';
        argv[argc++] = arg_buf[argc - 1];
      }
    }
  }

  va_end(ap);

  return redis_commandv(client, argc, argv, NULL, callback, user_data);
}

void redis_client_disconnect(redis_client_t *client) {
  int owned = 0;
  coro_socket_t *socket;
  if (!client) return;
  socket = redis_client_socket_take(client, &owned);
  if (socket && owned) coro_socket_destroy(socket);
}

int redis_client_interrupt(redis_client_t *client, int status) {
  int rc;
  if (!client) return TURBO_EINVAL;
  turbo_mutex_lock(&client->socket_mutex);
  rc = client->socket ? coro_socket_interrupt_wait(client->socket, status) : TURBO_ENOTCONN;
  turbo_mutex_unlock(&client->socket_mutex);
  return rc;
}

void redis_client_destroy(redis_client_t *client) {
  if (!client)
    return;

  TLOG_DEBUGF("Destroying Redis client for {:s}:{:d}", client->config.host, client->config.port);
  if (client->active_stream != NULL) {
    client->active_stream->client = NULL;
    client->active_stream = NULL;
  }
  redis_client_disconnect(client);
  reset_command_queue_state(client);

  redis_recv_buffer_destroy(client);
  tstr_free((tstr)client->config.host);
  tstr_free((tstr)client->config.username);
  tstr_free((tstr)client->config.password);
  turbo_mutex_destroy(&client->socket_mutex);
  free(client);
}

const char *redis_client_get_error(redis_client_t *client) {
  return client ? "Error" : "Invalid client";
}

/* Convenience functions */

int redis_set(redis_client_t *client, const char *key, const char *value,
              redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"SET", key, value};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_get(redis_client_t *client, const char *key, redis_command_cb_t callback,
              void *user_data) {
  const char *argv[] = {"GET", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_del(redis_client_t *client, int key_count, const char **keys, redis_command_cb_t callback,
              void *user_data) {
  if (!keys || key_count <= 0) {
    return -1;
  }

  int argc = 0;
  if (calc_argc_mul_add(1, key_count, 1, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) {
    return -1;
  }
  argv[0] = "DEL";
  for (int i = 0; i < key_count; i++) {
    argv[i + 1] = keys[i];
  }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_exists(redis_client_t *client, const char *key, redis_command_cb_t callback,
                 void *user_data) {
  const char *argv[] = {"EXISTS", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_expire(redis_client_t *client, const char *key, int seconds, redis_command_cb_t callback,
                 void *user_data) {
  char seconds_str[32];
  fmt(seconds_str, sizeof(seconds_str), "{}", seconds);
  const char *argv[] = {"EXPIRE", key, seconds_str};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_incr(redis_client_t *client, const char *key, redis_command_cb_t callback,
               void *user_data) {
  const char *argv[] = {"INCR", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_ping(redis_client_t *client, redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"PING"};
  return redis_commandv(client, 1, argv, NULL, callback, user_data);
}

int redis_lpush(redis_client_t *client, const char *key, int value_count, const char **values,
                redis_command_cb_t callback, void *user_data) {
  if (!key || !values || value_count <= 0) {
    return -1;
  }

  int argc = 0;
  if (calc_argc_mul_add(2, value_count, 1, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) {
    return -1;
  }
  argv[0] = "LPUSH";
  argv[1] = key;
  for (int i = 0; i < value_count; i++) {
    argv[i + 2] = values[i];
  }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_rpush(redis_client_t *client, const char *key, int value_count, const char **values,
                redis_command_cb_t callback, void *user_data) {
  if (!key || !values || value_count <= 0) {
    return -1;
  }

  int argc = 0;
  if (calc_argc_mul_add(2, value_count, 1, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) {
    return -1;
  }
  argv[0] = "RPUSH";
  argv[1] = key;
  for (int i = 0; i < value_count; i++) {
    argv[i + 2] = values[i];
  }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_lpop(redis_client_t *client, const char *key, redis_command_cb_t callback,
               void *user_data) {
  const char *argv[] = {"LPOP", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_rpop(redis_client_t *client, const char *key, redis_command_cb_t callback,
               void *user_data) {
  const char *argv[] = {"RPOP", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_hset(redis_client_t *client, const char *key, const char *field, const char *value,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"HSET", key, field, value};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_hget(redis_client_t *client, const char *key, const char *field,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"HGET", key, field};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_sadd(redis_client_t *client, const char *key, int member_count, const char **members,
               redis_command_cb_t callback, void *user_data) {
  if (!key || !members || member_count <= 0) {
    return -1;
  }

  int argc = 0;
  if (calc_argc_mul_add(2, member_count, 1, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) {
    return -1;
  }
  argv[0] = "SADD";
  argv[1] = key;
  for (int i = 0; i < member_count; i++) {
    argv[i + 2] = members[i];
  }
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_smembers(redis_client_t *client, const char *key, redis_command_cb_t callback,
                   void *user_data) {
  const char *argv[] = {"SMEMBERS", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

/* =============================================================================
 * Bloom Filter Implementation
 * =============================================================================
 */

int redis_bf_reserve(redis_client_t *client, const char *key, double error_rate, int capacity,
                     redis_command_cb_t callback, void *user_data) {
  char error_rate_str[32];
  char capacity_str[32];
  fmt(error_rate_str, sizeof(error_rate_str), "{:.4f}", error_rate);
  fmt(capacity_str, sizeof(capacity_str), "{}", capacity);
  
  const char *argv[] = {"BF.RESERVE", key, error_rate_str, capacity_str};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_bf_add(redis_client_t *client, const char *key, const char *item,
                 redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"BF.ADD", key, item};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_bf_exists(redis_client_t *client, const char *key, const char *item,
                    redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"BF.EXISTS", key, item};
  return redis_commandv(client, 3, argv, NULL, callback, user_data);
}

int redis_bf_madd(redis_client_t *client, const char *key, int item_count, const char **items,
                  redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !items || item_count <= 0) return -1;
  int argc = 0;
  if (calc_argc_mul_add(2, item_count, 1, &argc) < 0) return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;
  
  argv[0] = "BF.MADD";
  argv[1] = key;
  for (int i = 0; i < item_count; i++) {
    argv[i + 2] = items[i];
  }
  
  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

/* =============================================================================
 * Redis Streams Implementation
 * =============================================================================
 */

int redis_xadd_result(redis_client_t *client, const char *key, size_t maxlen,
                      size_t field_count, const char **fields,
                      const char **values, const size_t *value_lens,
                      redis_command_result_t *out) {
  if (!out) return TURBO_EINVAL;
  *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
  if (!client || !key || !fields || !values || field_count == 0 ||
      field_count > (size_t)INT_MAX) {
    out->status = TURBO_EINVAL;
    return out->status;
  }
  for (size_t i = 0; i < field_count; ++i) {
    if (!fields[i] || !values[i]) {
      out->status = TURBO_EINVAL;
      return out->status;
    }
  }

  /* Calculate argc: XADD key [MAXLEN ~ count] * field value [...] */
  int argc;
  if (calc_argc_mul_add(3, (int)field_count, 2, &argc) < 0) {
    out->status = TURBO_EINVAL;
    return out->status;
  }
  if (maxlen > 0) {
    if (calc_argc_mul_add(argc, 3, 1, &argc) < 0) {
      out->status = TURBO_EINVAL;
      return out->status;
    }
  }

  const char **argv = alloc_argv(argc);
  if ((size_t)argc > SIZE_MAX / sizeof(size_t)) {
    redis_argv_free(argv);
    out->status = TURBO_ENOMEM;
    return out->status;
  }
  size_t *argvlen = malloc(argc * sizeof(size_t));
  if (!argv || !argvlen) {
    redis_argv_free(argv);
    free(argvlen);
    out->status = TURBO_ENOMEM;
    return out->status;
  }

  char maxlen_str[32];
  size_t idx = 0;

  argv[idx] = "XADD";
  argvlen[idx++] = 4;

  argv[idx] = key;
  argvlen[idx++] = strlen(key);

  if (maxlen > 0) {
    argv[idx] = "MAXLEN";
    argvlen[idx++] = 6;
    argv[idx] = "~";
    argvlen[idx++] = 1;
    fmt(maxlen_str, sizeof(maxlen_str), "{}", maxlen);
    argv[idx] = maxlen_str;
    argvlen[idx++] = strlen(maxlen_str);
  }

  argv[idx] = "*";
  argvlen[idx++] = 1;

  for (size_t i = 0; i < field_count; i++) {
    argv[idx] = fields[i];
    argvlen[idx++] = strlen(fields[i]);
    argv[idx] = values[i];
    argvlen[idx++] = value_lens ? value_lens[i] : strlen(values[i]);
  }

  int result = redis_commandv_result(client, (int)idx, argv, argvlen, out);

  redis_argv_free(argv);
  free(argvlen);
  return result;
}

int redis_xadd(redis_client_t *client, const char *key, size_t maxlen,
               size_t field_count, const char **fields,
               const char **values, const size_t *value_lens,
               redis_command_cb_t callback, void *user_data) {
  redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
  int rc = redis_xadd_result(client, key, maxlen, field_count, fields, values,
                             value_lens, &result);
  if (result.outcome == REDIS_COMMAND_REPLIED && callback)
    callback(client, result.reply, user_data);
  rc = result.outcome == REDIS_COMMAND_REPLIED ? 0 : -1;
  redis_command_result_clear(&result);
  return rc;
}

typedef struct {
  redis_stream_cb_t callback;
  void *user_data;
} redis_stream_cb_ctx_t;

int redis_stream_reply_decode(const redis_reply_t *reply,
                              redis_stream_result_t **out,
                              size_t *out_count) {
  redis_stream_result_t *results = NULL;
  size_t result_count;
  int rc = TURBO_EPROTO;

  if (!out || !out_count) return TURBO_EINVAL;
  *out = NULL;
  *out_count = 0u;
  if (!reply || reply->type == REDIS_REPLY_NULL) return TURBO_OK;
  if (reply->type != REDIS_REPLY_ARRAY) return TURBO_EPROTO;
  result_count = reply->element_count;
  if (result_count == 0u) return TURBO_OK;
  if (result_count > SIZE_MAX / sizeof(*results)) return TURBO_ERANGE;
  results = calloc(result_count, sizeof(*results));
  if (!results) return TURBO_ENOMEM;

  for (size_t i = 0; i < result_count; ++i) {
    const redis_reply_t *stream = reply->elements[i];
    const redis_reply_t *entries;
    if (!stream || stream->type != REDIS_REPLY_ARRAY || stream->element_count != 2u ||
        !stream->elements[0] || stream->elements[0]->type != REDIS_REPLY_BULK_STRING ||
        !stream->elements[1] || stream->elements[1]->type != REDIS_REPLY_ARRAY)
      goto cleanup;

    results[i].stream_name = tstr_new_len(stream->elements[0]->str,
                                           stream->elements[0]->len);
    if (!results[i].stream_name) {
      rc = TURBO_ENOMEM;
      goto cleanup;
    }
    entries = stream->elements[1];
    results[i].entry_count = entries->element_count;
    if (results[i].entry_count == 0u) continue;
    if (results[i].entry_count > SIZE_MAX / sizeof(*results[i].entries)) {
      rc = TURBO_ERANGE;
      goto cleanup;
    }
    results[i].entries = calloc(results[i].entry_count, sizeof(*results[i].entries));
    if (!results[i].entries) {
      rc = TURBO_ENOMEM;
      goto cleanup;
    }

    for (size_t j = 0; j < results[i].entry_count; ++j) {
      const redis_reply_t *entry = entries->elements[j];
      const redis_reply_t *fields;
      redis_stream_entry_t *target = &results[i].entries[j];
      if (!entry || entry->type != REDIS_REPLY_ARRAY || entry->element_count != 2u ||
          !entry->elements[0] || entry->elements[0]->type != REDIS_REPLY_BULK_STRING ||
          !entry->elements[1] || entry->elements[1]->type != REDIS_REPLY_ARRAY)
        goto cleanup;
      fields = entry->elements[1];
      if ((fields->element_count & 1u) != 0u) goto cleanup;
      target->id = tstr_new_len(entry->elements[0]->str, entry->elements[0]->len);
      if (!target->id) {
        rc = TURBO_ENOMEM;
        goto cleanup;
      }
      target->field_count = fields->element_count / 2u;
      if (target->field_count == 0u) continue;
      if (target->field_count > SIZE_MAX / sizeof(*target->fields)) {
        rc = TURBO_ERANGE;
        goto cleanup;
      }
      target->fields = calloc(target->field_count, sizeof(*target->fields));
      target->values = calloc(target->field_count, sizeof(*target->values));
      target->value_lens = calloc(target->field_count, sizeof(*target->value_lens));
      if (!target->fields || !target->values || !target->value_lens) {
        rc = TURBO_ENOMEM;
        goto cleanup;
      }
      for (size_t k = 0; k < target->field_count; ++k) {
        const redis_reply_t *field = fields->elements[k * 2u];
        const redis_reply_t *value = fields->elements[k * 2u + 1u];
        if (!field || field->type != REDIS_REPLY_BULK_STRING || !value ||
            value->type != REDIS_REPLY_BULK_STRING || value->len == SIZE_MAX)
          goto cleanup;
        target->fields[k] = tstr_new_len(field->str, field->len);
        target->values[k] = malloc(value->len + 1u);
        if (!target->fields[k] || !target->values[k]) {
          rc = TURBO_ENOMEM;
          goto cleanup;
        }
        if (value->len > 0u) memcpy(target->values[k], value->str, value->len);
        target->values[k][value->len] = '\0';
        target->value_lens[k] = value->len;
      }
    }
  }

  *out = results;
  *out_count = result_count;
  return TURBO_OK;

cleanup:
  redis_stream_result_free(results, result_count);
  return rc;
}

void redis_stream_read_result_clear(redis_stream_read_result_t *result) {
  if (!result) return;
  redis_stream_result_free(result->streams, result->stream_count);
  redis_command_result_clear(&result->command);
  *result = (redis_stream_read_result_t)REDIS_STREAM_READ_RESULT_INIT;
}

int redis_xread_result(redis_client_t *client, size_t count, int block_ms,
                       size_t stream_count, const char **keys, const char **ids,
                       redis_stream_read_result_t *out) {
  if (!out) return TURBO_EINVAL;
  *out = (redis_stream_read_result_t)REDIS_STREAM_READ_RESULT_INIT;
  if (!client || !keys || !ids || stream_count == 0 ||
      stream_count > (size_t)INT_MAX) {
    out->command.status = TURBO_EINVAL;
    return out->command.status;
  }

  /* XREAD [COUNT count] [BLOCK ms] STREAMS key [key ...] id [id ...] */
  int argc;
  if (calc_argc_mul_add(2, (int)stream_count, 2, &argc) < 0) {
    out->command.status = TURBO_EINVAL;
    return out->command.status;
  }
  if (count > 0) {
    if (calc_argc_mul_add(argc, 2, 1, &argc) < 0) {
      out->command.status = TURBO_EINVAL;
      return out->command.status;
    }
  }
  if (block_ms >= 0) {
    if (calc_argc_mul_add(argc, 2, 1, &argc) < 0) {
      out->command.status = TURBO_EINVAL;
      return out->command.status;
    }
  }

  const char **argv = alloc_argv(argc);
  if (!argv) {
    out->command.status = TURBO_ENOMEM;
    return out->command.status;
  }

  char count_str[32], block_str[32];
  size_t idx = 0;

  argv[idx++] = "XREAD";

  if (count > 0) {
    argv[idx++] = "COUNT";
    fmt(count_str, sizeof(count_str), "{}", count);
    argv[idx++] = count_str;
  }

  if (block_ms >= 0) {
    argv[idx++] = "BLOCK";
    fmt(block_str, sizeof(block_str), "{}", block_ms);
    argv[idx++] = block_str;
  }

  argv[idx++] = "STREAMS";

  for (size_t i = 0; i < stream_count; i++) {
    argv[idx++] = keys[i];
  }
  for (size_t i = 0; i < stream_count; i++) {
    argv[idx++] = ids[i];
  }

  int result = redis_commandv_result(client, (int)idx, argv, NULL, &out->command);
  redis_argv_free(argv);
  if (result == TURBO_OK) {
    result = redis_stream_reply_decode(out->command.reply, &out->streams,
                                       &out->stream_count);
    if (result != TURBO_OK) out->command.status = result;
  }
  return result;
}

int redis_xread(redis_client_t *client, size_t count, int block_ms,
                size_t stream_count, const char **keys, const char **ids,
                redis_stream_cb_t callback, void *user_data) {
  redis_stream_read_result_t result = REDIS_STREAM_READ_RESULT_INIT;
  int rc = redis_xread_result(client, count, block_ms, stream_count, keys, ids, &result);
  if (rc == TURBO_OK && callback)
    callback(client, result.streams, result.stream_count, user_data);
  redis_stream_read_result_clear(&result);
  return rc == TURBO_OK ? 0 : -1;
}

int redis_xgroup_create_result(redis_client_t *client, const char *key,
                               const char *group, const char *id, int mkstream,
                               redis_command_result_t *out) {
  if (!out) return TURBO_EINVAL;
  *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
  if (!client || !key || !group || !id) {
    out->status = TURBO_EINVAL;
    return out->status;
  }

  if (mkstream) {
    const char *argv[] = {"XGROUP", "CREATE", key, group, id, "MKSTREAM"};
    return redis_commandv_result(client, 6, argv, NULL, out);
  } else {
    const char *argv[] = {"XGROUP", "CREATE", key, group, id};
    return redis_commandv_result(client, 5, argv, NULL, out);
  }
}

int redis_xgroup_create(redis_client_t *client, const char *key,
                        const char *group, const char *id, int mkstream,
                        redis_command_cb_t callback, void *user_data) {
  redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
  int rc = redis_xgroup_create_result(client, key, group, id, mkstream, &result);
  if (result.outcome == REDIS_COMMAND_REPLIED && callback)
    callback(client, result.reply, user_data);
  rc = result.outcome == REDIS_COMMAND_REPLIED ? 0 : -1;
  redis_command_result_clear(&result);
  return rc;
}

int redis_xreadgroup_result(redis_client_t *client, const char *group,
                            const char *consumer, size_t count, int block_ms,
                            size_t stream_count, const char **keys,
                            const char **ids, redis_stream_read_result_t *out) {
  if (!out) return TURBO_EINVAL;
  *out = (redis_stream_read_result_t)REDIS_STREAM_READ_RESULT_INIT;
  if (!client || !group || !consumer || !keys || !ids || stream_count == 0 ||
      stream_count > (size_t)INT_MAX) {
    out->command.status = TURBO_EINVAL;
    return out->command.status;
  }

  /* XREADGROUP GROUP group consumer [COUNT count] [BLOCK ms] STREAMS key [key ...] id [...] */
  int argc;
  if (calc_argc_mul_add(6, (int)stream_count, 2, &argc) < 0) {
    out->command.status = TURBO_EINVAL;
    return out->command.status;
  }
  if (count > 0) {
    if (calc_argc_mul_add(argc, 2, 1, &argc) < 0) {
      out->command.status = TURBO_EINVAL;
      return out->command.status;
    }
  }
  if (block_ms >= 0) {
    if (calc_argc_mul_add(argc, 2, 1, &argc) < 0) {
      out->command.status = TURBO_EINVAL;
      return out->command.status;
    }
  }

  const char **argv = alloc_argv(argc);
  if (!argv) {
    out->command.status = TURBO_ENOMEM;
    return out->command.status;
  }

  char count_str[32], block_str[32];
  size_t idx = 0;

  argv[idx++] = "XREADGROUP";
  argv[idx++] = "GROUP";
  argv[idx++] = group;
  argv[idx++] = consumer;

  if (count > 0) {
    argv[idx++] = "COUNT";
    fmt(count_str, sizeof(count_str), "{}", count);
    argv[idx++] = count_str;
  }

  if (block_ms >= 0) {
    argv[idx++] = "BLOCK";
    fmt(block_str, sizeof(block_str), "{}", block_ms);
    argv[idx++] = block_str;
  }

  argv[idx++] = "STREAMS";

  for (size_t i = 0; i < stream_count; i++) {
    argv[idx++] = keys[i];
  }
  for (size_t i = 0; i < stream_count; i++) {
    argv[idx++] = ids[i];
  }

  int result = redis_commandv_result(client, (int)idx, argv, NULL, &out->command);
  redis_argv_free(argv);
  if (result == TURBO_OK) {
    result = redis_stream_reply_decode(out->command.reply, &out->streams,
                                       &out->stream_count);
    if (result != TURBO_OK) out->command.status = result;
  }
  return result;
}

int redis_xreadgroup(redis_client_t *client, const char *group, const char *consumer,
                     size_t count, int block_ms,
                     size_t stream_count, const char **keys, const char **ids,
                     redis_stream_cb_t callback, void *user_data) {
  redis_stream_read_result_t result = REDIS_STREAM_READ_RESULT_INIT;
  int rc = redis_xreadgroup_result(client, group, consumer, count, block_ms,
                                   stream_count, keys, ids, &result);
  if (rc == TURBO_OK && callback)
    callback(client, result.streams, result.stream_count, user_data);
  redis_stream_read_result_clear(&result);
  return rc == TURBO_OK ? 0 : -1;
}

int redis_xack_result(redis_client_t *client, const char *key, const char *group,
                      size_t id_count, const char **ids,
                      redis_command_result_t *out) {
  if (!out) return TURBO_EINVAL;
  *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
  if (!client || !key || !group || !ids || id_count == 0 ||
      id_count > (size_t)INT_MAX) {
    out->status = TURBO_EINVAL;
    return out->status;
  }

  int argc;
  if (calc_argc_mul_add(3, (int)id_count, 1, &argc) < 0) {
    out->status = TURBO_EINVAL;
    return out->status;
  }
  const char **argv = alloc_argv(argc);
  if (!argv) {
    out->status = TURBO_ENOMEM;
    return out->status;
  }

  argv[0] = "XACK";
  argv[1] = key;
  argv[2] = group;
  for (size_t i = 0; i < id_count; i++) {
    argv[3 + i] = ids[i];
  }

  int result = redis_commandv_result(client, (int)argc, argv, NULL, out);
  redis_argv_free(argv);
  return result;
}

int redis_xack(redis_client_t *client, const char *key, const char *group,
               size_t id_count, const char **ids,
               redis_command_cb_t callback, void *user_data) {
  redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
  int rc = redis_xack_result(client, key, group, id_count, ids, &result);
  if (result.outcome == REDIS_COMMAND_REPLIED && callback)
    callback(client, result.reply, user_data);
  rc = result.outcome == REDIS_COMMAND_REPLIED ? 0 : -1;
  redis_command_result_clear(&result);
  return rc;
}

int redis_xdel(redis_client_t *client, const char *key,
               size_t id_count, const char **ids,
               redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !ids || id_count == 0)
    return -1;

  if (id_count > (size_t)INT_MAX)
    return -1;

  int argc;
  if (calc_argc_mul_add(2, (int)id_count, 1, &argc) < 0)
    return -1;

  const char **argv = alloc_argv(argc);
  if (!argv) return -1;

  argv[0] = "XDEL";
  argv[1] = key;
  for (size_t i = 0; i < id_count; i++) {
    argv[2 + i] = ids[i];
  }

  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

int redis_xlen(redis_client_t *client, const char *key,
               redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"XLEN", key};
  return redis_commandv(client, 2, argv, NULL, callback, user_data);
}

int redis_xtrim(redis_client_t *client, const char *key, size_t maxlen,
                redis_command_cb_t callback, void *user_data) {
  char maxlen_str[32];
  fmt(maxlen_str, sizeof(maxlen_str), "{}", maxlen);
  const char *argv[] = {"XTRIM", key, "MAXLEN", "~", maxlen_str};
  return redis_commandv(client, 5, argv, NULL, callback, user_data);
}

void redis_stream_entry_free(redis_stream_entry_t *entry) {
  if (!entry) return;

  tstr_free((tstr)entry->id);
  for (size_t i = 0; i < entry->field_count; i++) {
    if (entry->fields)
      tstr_free((tstr)entry->fields[i]);
    if (entry->values)
      free(entry->values[i]);
  }
  free(entry->fields);
  free(entry->values);
  free(entry->value_lens);
}

int redis_stream_entry_take_value(redis_stream_entry_t *entry, size_t index,
                                  char **out_value, size_t *out_len) {
  if (out_value) *out_value = NULL;
  if (out_len) *out_len = 0u;
  if (!entry || !out_value || !out_len) return TURBO_EINVAL;
  if (index >= entry->field_count) return TURBO_ERANGE;
  if (!entry->values || !entry->value_lens || !entry->values[index]) return TURBO_ENOENT;

  *out_value = entry->values[index];
  *out_len = entry->value_lens[index];
  entry->values[index] = NULL;
  entry->value_lens[index] = 0u;
  return TURBO_OK;
}

void redis_stream_value_free(void *value) { free(value); }

void redis_stream_result_free(redis_stream_result_t *results, size_t count) {
  if (!results) return;

  for (size_t i = 0; i < count; i++) {
    tstr_free((tstr)results[i].stream_name);
    for (size_t j = 0; j < results[i].entry_count; j++) {
      redis_stream_entry_free(&results[i].entries[j]);
    }
    free(results[i].entries);
  }
  free(results);
}

/* ---------------------------------------------------------------------------
 * Helper: wrap a single-stream RESP array (as returned by XRANGE/XREVRANGE/
 * XCLAIM) in a synthetic redis_stream_result_t so callers always receive the
 * same redis_stream_cb_t interface.
 * ---------------------------------------------------------------------------
 */
static void on_single_stream_reply(redis_client_t *client, redis_reply_t *reply,
                                   void *user_data) {
  redis_stream_cb_ctx_t *ctx = (redis_stream_cb_ctx_t *)user_data;

  if (!ctx || !ctx->callback) {
    free(ctx);
    return;
  }

  if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->element_count == 0) {
    ctx->callback(client, NULL, 0, ctx->user_data);
    free(ctx);
    return;
  }

  /* Synthesise a single-element result_count=1 wrapper. */
  redis_stream_result_t result;
  memset(&result, 0, sizeof(result));
  result.entry_count = reply->element_count;
  result.entries     = calloc(result.entry_count, sizeof(redis_stream_entry_t));
  if (!result.entries) {
    ctx->callback(client, NULL, 0, ctx->user_data);
    free(ctx);
    return;
  }

  for (size_t j = 0; j < reply->element_count; j++) {
    redis_reply_t *entry = reply->elements[j];
    if (!entry || entry->type != REDIS_REPLY_ARRAY || entry->element_count < 2)
      continue;

    if (entry->elements[0]->type == REDIS_REPLY_BULK_STRING)
      result.entries[j].id = tstr_dup(entry->elements[0]->str);

    redis_reply_t *fields = entry->elements[1];
    if (fields->type == REDIS_REPLY_ARRAY && fields->element_count >= 2) {
      size_t fc = fields->element_count / 2;
      result.entries[j].field_count = fc;
      result.entries[j].fields     = calloc(fc, sizeof(char *));
      result.entries[j].values     = calloc(fc, sizeof(char *));
      result.entries[j].value_lens = calloc(fc, sizeof(size_t));

      for (size_t k = 0; k < fc; k++) {
        redis_reply_t *f = fields->elements[k * 2];
        redis_reply_t *v = fields->elements[k * 2 + 1];
        if (f->type == REDIS_REPLY_BULK_STRING)
          result.entries[j].fields[k] = tstr_dup(f->str);
        if (v->type == REDIS_REPLY_BULK_STRING) {
          result.entries[j].values[k] = malloc(v->len + 1);
          memcpy(result.entries[j].values[k], v->str, v->len);
          result.entries[j].values[k][v->len] = '\0';
          result.entries[j].value_lens[k] = v->len;
        }
      }
    }
  }

  ctx->callback(client, &result, 1, ctx->user_data);
  redis_stream_result_free(&result, 1);  /* frees entries[] but not &result */
  free(ctx);
}

/* ---------------------------------------------------------------------------
 * XRANGE key start end [COUNT count]
 * ---------------------------------------------------------------------------
 */
int redis_xrange(redis_client_t *client, const char *key,
                 const char *start, const char *end, size_t count,
                 redis_stream_cb_t callback, void *user_data) {
  if (!client || !key || !start || !end)
    return -1;

  char count_str[32];
  size_t argc = count > 0 ? 6 : 4;
  const char *argv_static[6];
  size_t idx = 0;

  argv_static[idx++] = "XRANGE";
  argv_static[idx++] = key;
  argv_static[idx++] = start;
  argv_static[idx++] = end;
  if (count > 0) {
    argv_static[idx++] = "COUNT";
    fmt(count_str, sizeof(count_str), "{}", count);
    argv_static[idx++] = count_str;
  }

  redis_stream_cb_ctx_t *ctx = malloc(sizeof(redis_stream_cb_ctx_t));
  if (!ctx) return -1;
  ctx->callback  = callback;
  ctx->user_data = user_data;

  int result = redis_commandv(client, (int)argc, argv_static, NULL,
                              on_single_stream_reply, ctx);
  if (result != 0) free(ctx);
  return result;
}

/* ---------------------------------------------------------------------------
 * XREVRANGE key end start [COUNT count]
 * ---------------------------------------------------------------------------
 */
int redis_xrevrange(redis_client_t *client, const char *key,
                    const char *end, const char *start, size_t count,
                    redis_stream_cb_t callback, void *user_data) {
  if (!client || !key || !end || !start)
    return -1;

  char count_str[32];
  size_t argc = count > 0 ? 6 : 4;
  const char *argv_static[6];
  size_t idx = 0;

  argv_static[idx++] = "XREVRANGE";
  argv_static[idx++] = key;
  argv_static[idx++] = end;
  argv_static[idx++] = start;
  if (count > 0) {
    argv_static[idx++] = "COUNT";
    fmt(count_str, sizeof(count_str), "{}", count);
    argv_static[idx++] = count_str;
  }

  redis_stream_cb_ctx_t *ctx = malloc(sizeof(redis_stream_cb_ctx_t));
  if (!ctx) return -1;
  ctx->callback  = callback;
  ctx->user_data = user_data;

  int result = redis_commandv(client, (int)argc, argv_static, NULL,
                              on_single_stream_reply, ctx);
  if (result != 0) free(ctx);
  return result;
}

/* ---------------------------------------------------------------------------
 * XPENDING key group [start end count [consumer]]
 *
 * When start == NULL → summary form (2 args: XPENDING key group).
 * When start != NULL → detail form  (4–5 args).
 * The raw RESP reply is forwarded to the generic command callback; callers
 * parse it themselves or use redis_xpending_entry_t as a guide.
 * ---------------------------------------------------------------------------
 */
int redis_xpending(redis_client_t *client, const char *key,
                   const char *group,
                   const char *start, const char *end, size_t count,
                   const char *consumer,
                   redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !group)
    return -1;

  if (!start) {
    /* Summary form: XPENDING key group */
    const char *argv[] = {"XPENDING", key, group};
    return redis_commandv(client, 3, argv, NULL, callback, user_data);
  }

  /* Detail form: XPENDING key group start end count [consumer] */
  char count_str[32];
  fmt(count_str, sizeof(count_str), "{}", count);

  int argc = consumer ? 7 : 6;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;

  size_t idx = 0;
  argv[idx++] = "XPENDING";
  argv[idx++] = key;
  argv[idx++] = group;
  argv[idx++] = start;
  argv[idx++] = end;
  argv[idx++] = count_str;
  if (consumer) argv[idx++] = consumer;

  int result = redis_commandv(client, argc, argv, NULL, callback, user_data);
  redis_argv_free(argv);
  return result;
}

/* ---------------------------------------------------------------------------
 * XCLAIM key group consumer min-idle-time id [id ...]
 * ---------------------------------------------------------------------------
 */
int redis_xclaim(redis_client_t *client, const char *key,
                 const char *group, const char *consumer,
                 int64_t min_idle_ms,
                 size_t id_count, const char **ids,
                 redis_stream_cb_t callback, void *user_data) {
  if (!client || !key || !group || !consumer || !ids || id_count == 0)
    return -1;

  char idle_str[32];
  fmt(idle_str, sizeof(idle_str), "{}", min_idle_ms);

  if (id_count > (size_t)INT_MAX)
    return -1;

  /* XCLAIM key group consumer idle id [id ...] */
  int argc;
  if (calc_argc_mul_add(5, (int)id_count, 1, &argc) < 0)
    return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;

  size_t idx = 0;
  argv[idx++] = "XCLAIM";
  argv[idx++] = key;
  argv[idx++] = group;
  argv[idx++] = consumer;
  argv[idx++] = idle_str;
  for (size_t i = 0; i < id_count; i++)
    argv[idx++] = ids[i];

  redis_stream_cb_ctx_t *ctx = malloc(sizeof(redis_stream_cb_ctx_t));
  if (!ctx) { redis_argv_free(argv); return -1; }
  ctx->callback  = callback;
  ctx->user_data = user_data;

  int result = redis_commandv(client, argc, argv, NULL,
                              on_single_stream_reply, ctx);
  redis_argv_free(argv);
  if (result != 0) free(ctx);
  return result;
}

/* ---------------------------------------------------------------------------
 * XAUTOCLAIM key group consumer min-idle-time start [COUNT count]
 *
 * Redis 6.2+ reply: [next-id, [[id,[f,v,...]], ...], [deleted-ids]]
 * The middle element (index 1) is forwarded via on_single_stream_reply.
 * ---------------------------------------------------------------------------
 */

/* Intermediate ctx to peel the [next-id, entries, deleted] wrapper. */
typedef struct {
  redis_stream_cb_t callback;
  void             *user_data;
} redis_xautoclaim_ctx_t;

static void on_xautoclaim_reply(redis_client_t *client, redis_reply_t *reply,
                                void *user_data) {
  redis_xautoclaim_ctx_t *ctx = (redis_xautoclaim_ctx_t *)user_data;

  if (!ctx || !ctx->callback) { free(ctx); return; }

  /* Redis reply: *3 [next-id, entries-array, deleted-ids-array] */
  if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->element_count < 2) {
    ctx->callback(client, NULL, 0, ctx->user_data);
    free(ctx);
    return;
  }

  /* on_single_stream_reply takes ownership of and free()s its ctx arg.
   * Allocate a heap copy so we never call free() on a stack pointer. */
  redis_stream_cb_ctx_t *inner = malloc(sizeof(redis_stream_cb_ctx_t));
  if (!inner) {
    ctx->callback(client, NULL, 0, ctx->user_data);
    free(ctx);
    return;
  }
  inner->callback  = ctx->callback;
  inner->user_data = ctx->user_data;
  free(ctx);

  on_single_stream_reply(client, reply->elements[1], inner);
  /* inner is freed inside on_single_stream_reply */
}

int redis_xautoclaim(redis_client_t *client, const char *key,
                     const char *group, const char *consumer,
                     int64_t min_idle_ms, const char *start, size_t count,
                     redis_stream_cb_t callback, void *user_data) {
  if (!client || !key || !group || !consumer || !start)
    return -1;

  char idle_str[32], count_str[32];
  fmt(idle_str, sizeof(idle_str), "{}", min_idle_ms);

  size_t argc = count > 0 ? 8 : 6;
  const char *argv_static[8];
  size_t idx = 0;

  argv_static[idx++] = "XAUTOCLAIM";
  argv_static[idx++] = key;
  argv_static[idx++] = group;
  argv_static[idx++] = consumer;
  argv_static[idx++] = idle_str;
  argv_static[idx++] = start;
  if (count > 0) {
    argv_static[idx++] = "COUNT";
    fmt(count_str, sizeof(count_str), "{}", count);
    argv_static[idx++] = count_str;
  }

  redis_xautoclaim_ctx_t *actx = malloc(sizeof(redis_xautoclaim_ctx_t));
  if (!actx) return -1;
  actx->callback  = callback;
  actx->user_data = user_data;

  int result = redis_commandv(client, (int)argc, argv_static, NULL,
                              on_xautoclaim_reply, actx);
  if (result != 0) free(actx);
  return result;
}

/* ---------------------------------------------------------------------------
 * XGROUP sub-commands
 * ---------------------------------------------------------------------------
 */

int redis_xgroup_setid(redis_client_t *client, const char *key,
                       const char *group, const char *id,
                       redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !group || !id)
    return -1;
  const char *argv[] = {"XGROUP", "SETID", key, group, id};
  return redis_commandv(client, 5, argv, NULL, callback, user_data);
}

int redis_xgroup_destroy(redis_client_t *client, const char *key,
                         const char *group,
                         redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !group)
    return -1;
  const char *argv[] = {"XGROUP", "DESTROY", key, group};
  return redis_commandv(client, 4, argv, NULL, callback, user_data);
}

int redis_xgroup_createconsumer(redis_client_t *client, const char *key,
                                const char *group, const char *consumer,
                                redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !group || !consumer)
    return -1;
  const char *argv[] = {"XGROUP", "CREATECONSUMER", key, group, consumer};
  return redis_commandv(client, 5, argv, NULL, callback, user_data);
}

int redis_xgroup_delconsumer(redis_client_t *client, const char *key,
                             const char *group, const char *consumer,
                             redis_command_cb_t callback, void *user_data) {
  if (!client || !key || !group || !consumer)
    return -1;
  const char *argv[] = {"XGROUP", "DELCONSUMER", key, group, consumer};
  return redis_commandv(client, 5, argv, NULL, callback, user_data);
}

/* =============================================================================
 * Redis Pub/Sub Implementation
 * =============================================================================
 */

/* Subscription linked list node */
struct redis_subscription_s {
  char *channel;
  int is_pattern;
  redis_pubsub_cb_t callback;
  void *user_data;
  redis_subscription_t *next;
};

int redis_publish(redis_client_t *client, const char *channel,
                  const void *message, size_t len,
                  redis_command_cb_t callback, void *user_data) {
  if (!client || !channel)
    return -1;

  const char *argv[] = {"PUBLISH", channel, (const char *)message};
  size_t argvlen[] = {7, strlen(channel), len};

  return redis_commandv(client, 3, argv, argvlen, callback, user_data);
}

int redis_subscribe(redis_client_t *client, size_t channel_count,
                    const char **channels, redis_pubsub_cb_t on_message,
                    void *user_data) {
  if (!client || !channels || channel_count == 0)
    return -1;

  if (channel_count > (size_t)INT_MAX)
    return -1;

  int argc;
  if (calc_argc_mul_add(1, (int)channel_count, 1, &argc) < 0)
    return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;

  argv[0] = "SUBSCRIBE";
  for (size_t i = 0; i < channel_count; i++) {
    argv[i + 1] = channels[i];
  }

  int result = redis_commandv(client, argc, argv, NULL, NULL, NULL);
  redis_argv_free(argv);
  if (result == 0) {
    client->is_subscriber = 1;
    client->pubsub_cb = on_message;
    client->pubsub_user_data = user_data;
  }
  return result;
}

int redis_psubscribe(redis_client_t *client, size_t pattern_count,
                     const char **patterns, redis_pubsub_cb_t on_message,
                     void *user_data) {
  if (!client || !patterns || pattern_count == 0)
    return -1;

  if (pattern_count > (size_t)INT_MAX)
    return -1;

  int argc;
  if (calc_argc_mul_add(1, (int)pattern_count, 1, &argc) < 0)
    return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;

  argv[0] = "PSUBSCRIBE";
  for (size_t i = 0; i < pattern_count; i++) {
    argv[i + 1] = patterns[i];
  }

  int result = redis_commandv(client, argc, argv, NULL, NULL, NULL);
  redis_argv_free(argv);
  if (result == 0) {
    client->is_subscriber = 1;
    client->pubsub_cb = on_message;
    client->pubsub_user_data = user_data;
  }
  return result;
}

int redis_unsubscribe(redis_client_t *client, size_t channel_count,
                      const char **channels) {
  if (!client)
    return -1;

  if (channel_count == 0 || !channels) {
    const char *argv[] = {"UNSUBSCRIBE"};
    return redis_commandv(client, 1, argv, NULL, NULL, NULL);
  }

  if (channel_count > (size_t)INT_MAX)
    return -1;

  int argc;
  if (calc_argc_mul_add(1, (int)channel_count, 1, &argc) < 0)
    return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;

  argv[0] = "UNSUBSCRIBE";
  for (size_t i = 0; i < channel_count; i++) {
    argv[i + 1] = channels[i];
  }

  int result = redis_commandv(client, argc, argv, NULL, NULL, NULL);
  redis_argv_free(argv);
  return result;
}

int redis_punsubscribe(redis_client_t *client, size_t pattern_count,
                       const char **patterns) {
  if (!client)
    return -1;

  if (pattern_count == 0 || !patterns) {
    const char *argv[] = {"PUNSUBSCRIBE"};
    return redis_commandv(client, 1, argv, NULL, NULL, NULL);
  }

  if (pattern_count > (size_t)INT_MAX)
    return -1;

  int argc;
  if (calc_argc_mul_add(1, (int)pattern_count, 1, &argc) < 0)
    return -1;
  const char **argv = alloc_argv(argc);
  if (!argv) return -1;

  argv[0] = "PUNSUBSCRIBE";
  for (size_t i = 0; i < pattern_count; i++) {
    argv[i + 1] = patterns[i];
  }

  int result = redis_commandv(client, argc, argv, NULL, NULL, NULL);
  redis_argv_free(argv);
  return result;
}
static void redis_client_socket_publish(redis_client_t *client, coro_socket_t *socket,
                                        int owned, int connected) {
  turbo_mutex_lock(&client->socket_mutex);
  client->socket = socket;
  client->owns_socket = owned;
  client->is_connected = connected;
  turbo_mutex_unlock(&client->socket_mutex);
}

static coro_socket_t *redis_client_socket_take(redis_client_t *client, int *owned) {
  coro_socket_t *socket;
  turbo_mutex_lock(&client->socket_mutex);
  socket = client->socket;
  if (owned) *owned = client->owns_socket;
  client->socket = NULL;
  client->owns_socket = 0;
  client->is_connected = 0;
  client->is_authenticated = 0;
  client->selected_db = 0;
  client->is_cluster_readonly = 0;
  turbo_mutex_unlock(&client->socket_mutex);
  return socket;
}
