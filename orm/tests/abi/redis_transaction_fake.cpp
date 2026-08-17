#include "redis_fake_support.hpp"

#include <redis_client.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct client_state {
    bool in_transaction = false;
    std::vector<std::vector<std::string>> queued;
};

struct scripted_redis_call {
    bool has_outcome = false;
    redis_command_outcome_t outcome = REDIS_COMMAND_NOT_SENT;
    bool has_server_error = false;
    redis_server_error_t server_error = REDIS_SERVER_ERROR_NONE;
    std::string error_text;
    bool has_null_reply = false;
    bool has_exec_reply_count = false;
    std::size_t exec_reply_count = 0;
    std::vector<std::string> exec_errors;
};

std::unordered_map<redis_client_t*, client_state> clients;
std::vector<std::vector<std::string>> recorded_commands;
scripted_redis_call next_redis_call;

redis_command_result_t empty_result()
{
    return redis_command_result_t{
        0, REDIS_COMMAND_NOT_SENT, REDIS_SERVER_ERROR_NONE, nullptr};
}

redis_reply_t* reply(redis_reply_type_t type)
{
    auto* value = static_cast<redis_reply_t*>(std::calloc(1, sizeof(redis_reply_t)));
    if (value != nullptr)
        value->type = type;
    return value;
}

redis_reply_t* string_reply(redis_reply_type_t type, const char* text)
{
    redis_reply_t* value = reply(type);
    if (value == nullptr)
        return nullptr;
    value->len = std::strlen(text);
    value->str = static_cast<char*>(std::malloc(value->len + 1));
    if (value->str == nullptr) {
        std::free(value);
        return nullptr;
    }
    std::memcpy(value->str, text, value->len + 1);
    return value;
}

redis_reply_t* integer_reply(std::int64_t integer)
{
    redis_reply_t* value = reply(REDIS_REPLY_INTEGER);
    if (value != nullptr)
        value->integer = integer;
    return value;
}

redis_reply_t* array_reply(std::vector<redis_reply_t*> elements)
{
    redis_reply_t* value = reply(REDIS_REPLY_ARRAY);
    if (value == nullptr)
        return nullptr;
    value->element_count = elements.size();
    if (!elements.empty()) {
        value->elements = static_cast<redis_reply_t**>(
            std::calloc(elements.size(), sizeof(redis_reply_t*)));
        if (value->elements == nullptr) {
            std::free(value);
            return nullptr;
        }
        for (std::size_t index = 0; index < elements.size(); ++index)
            value->elements[index] = elements[index];
    }
    return value;
}

void free_reply(redis_reply_t* value)
{
    if (value == nullptr)
        return;
    for (std::size_t index = 0; index < value->element_count; ++index)
        free_reply(value->elements[index]);
    std::free(value->elements);
    std::free(value->str);
    std::free(value);
}

std::vector<std::string> copy_command(int argc,
                                      const char** argv,
                                      const std::size_t* lengths)
{
    std::vector<std::string> command;
    command.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        const std::size_t length = lengths != nullptr
            ? lengths[index]
            : std::strlen(argv[index]);
        command.emplace_back(argv[index], length);
    }
    return command;
}

redis_reply_t* search_result()
{
    return array_reply({
        integer_reply(1),
        string_reply(REDIS_REPLY_BULK_STRING, "orm:{person}:4"),
        array_reply({
            string_reply(REDIS_REPLY_BULK_STRING, "id"),
            string_reply(REDIS_REPLY_BULK_STRING, "4"),
            string_reply(REDIS_REPLY_BULK_STRING, "name"),
            string_reply(REDIS_REPLY_BULK_STRING, "Dora")})});
}

redis_reply_t* execute_queued(const std::vector<std::string>& command)
{
    if (!command.empty() && command.front() == "FT.SEARCH")
        return search_result();
    for (const std::string& argument : command) {
        if (argument.find("duplicate") != std::string::npos)
            return string_reply(REDIS_REPLY_ERROR, "ORM_DUPLICATE_KEY");
    }
    return integer_reply(1);
}

void complete(redis_command_result_t* out, redis_reply_t* value)
{
    out->reply = value;
    out->outcome = REDIS_COMMAND_REPLIED;
    out->server_error = redis_server_error_classify(value);
    out->status = out->server_error == REDIS_SERVER_ERROR_NONE ? 0 : -1;
}

} // namespace

extern "C" {

void fake_redis_reset(void)
{
    recorded_commands.clear();
    clients.clear();
    next_redis_call = scripted_redis_call{};
}

void fake_redis_script_next_outcome(redis_command_outcome_t outcome)
{
    next_redis_call.has_outcome = true;
    next_redis_call.outcome = outcome;
}

void fake_redis_script_next_server_error(redis_server_error_t server_error,
                                         const char* text)
{
    next_redis_call.has_server_error = true;
    next_redis_call.server_error = server_error;
    next_redis_call.error_text = text != nullptr ? text : "";
}

void fake_redis_script_next_null_reply(void)
{
    next_redis_call.has_null_reply = true;
}

void fake_redis_script_exec_reply_count(size_t count)
{
    next_redis_call.has_exec_reply_count = true;
    next_redis_call.exec_reply_count = count;
}

void fake_redis_script_exec_error_at(size_t index, const char* message)
{
    if (next_redis_call.exec_errors.size() <= index)
        next_redis_call.exec_errors.resize(index + 1);
    next_redis_call.exec_errors[index] = message != nullptr ? message : "";
}

redis_client_t* redis_client_create_with_config(const redis_config_t*)
{
    auto* client = new redis_client_t{};
    clients.emplace(client, client_state{});
    return client;
}

void redis_client_destroy(redis_client_t* client)
{
    clients.erase(client);
    delete client;
}

int redis_client_connect(redis_client_t* client,
                         redis_connect_cb_t,
                         void*)
{
    if (client == nullptr)
        return -1;
    client->is_connected = 1;
    return 0;
}

int redis_commandv_result(redis_client_t* client,
                          int argc,
                          const char** argv,
                          const size_t* lengths,
                          redis_command_result_t* out)
{
    *out = empty_result();
    if (client == nullptr || client->is_connected == 0 || argc <= 0)
        return -1;
    client_state& state = clients.at(client);
    const std::vector<std::string> command = copy_command(argc, argv, lengths);
    recorded_commands.push_back(command);

    if (next_redis_call.has_outcome) {
        out->outcome = next_redis_call.outcome;
        out->status = out->outcome == REDIS_COMMAND_REPLIED ? 0 : -1;
        next_redis_call = scripted_redis_call{};
        return out->status;
    }
    if (next_redis_call.has_server_error) {
        out->outcome = REDIS_COMMAND_REPLIED;
        out->server_error = next_redis_call.server_error;
        out->status = -1;
        out->reply = string_reply(REDIS_REPLY_ERROR,
                                  next_redis_call.error_text.c_str());
        next_redis_call = scripted_redis_call{};
        return out->status;
    }
    if (next_redis_call.has_null_reply) {
        out->outcome = REDIS_COMMAND_REPLIED;
        next_redis_call = scripted_redis_call{};
        return out->status;
    }

    if (command.front() == "FT._LIST") {
        complete(out, array_reply({}));
    } else if (command.front() == "MULTI") {
        state.in_transaction = true;
        state.queued.clear();
        complete(out, string_reply(REDIS_REPLY_STRING, "OK"));
    } else if (command.front() == "DISCARD") {
        state.in_transaction = false;
        state.queued.clear();
        complete(out, string_reply(REDIS_REPLY_STRING, "OK"));
    } else if (command.front() == "EXEC") {
        std::vector<redis_reply_t*> replies;
        const std::size_t reply_count =
            next_redis_call.has_exec_reply_count
                ? next_redis_call.exec_reply_count
                : state.queued.size();
        replies.reserve(reply_count);
        for (std::size_t index = 0; index < reply_count; ++index) {
            if (index < state.queued.size()) {
                if (index < next_redis_call.exec_errors.size() &&
                    !next_redis_call.exec_errors[index].empty()) {
                    replies.push_back(
                        string_reply(REDIS_REPLY_ERROR,
                                     next_redis_call.exec_errors[index].c_str()));
                } else {
                    replies.push_back(execute_queued(state.queued[index]));
                }
            } else {
                replies.push_back(integer_reply(1));
            }
        }
        state.in_transaction = false;
        state.queued.clear();
        complete(out, array_reply(std::move(replies)));
        next_redis_call = scripted_redis_call{};
    } else if (state.in_transaction) {
        state.queued.push_back(command);
        complete(out, string_reply(REDIS_REPLY_STRING, "QUEUED"));
    } else {
        complete(out, integer_reply(1));
    }
    return out->status;
}

void redis_command_result_clear(redis_command_result_t* result)
{
    if (result == nullptr)
        return;
    free_reply(result->reply);
    *result = empty_result();
}

redis_server_error_t redis_server_error_classify(const redis_reply_t* value)
{
    return value != nullptr && value->type == REDIS_REPLY_ERROR
        ? REDIS_SERVER_ERROR_ERR
        : REDIS_SERVER_ERROR_NONE;
}

int redis_evalsha_result(redis_client_t*, const char*, int, const char**, int,
                         const char**, redis_command_result_t* out)
{
    *out = empty_result();
    complete(out, integer_reply(1));
    return 0;
}

int redis_script_load_result(redis_client_t*, const char*, redis_command_result_t* out)
{
    *out = empty_result();
    complete(out, string_reply(REDIS_REPLY_BULK_STRING, "fake-digest"));
    return 0;
}

} // extern "C"

std::vector<std::vector<std::string>> fake_redis_commands()
{
    return recorded_commands;
}
