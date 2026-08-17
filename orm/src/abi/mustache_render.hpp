#ifndef ORM_MUSTACHE_RENDER_HPP
#define ORM_MUSTACHE_RENDER_HPP

#include "orm_c_internal.hpp"
#include "mustache/mustache.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace orm_c_detail {
namespace mustache_detail {

enum class node_kind { map, string, list };

struct node {
    node_kind kind = node_kind::map;
    std::string name;
    std::string value;
    std::vector<std::unique_ptr<node>> children;
};

inline void add_string(std::vector<std::unique_ptr<node>>& children,
                       std::string name,
                       std::string value)
{
    auto item = std::make_unique<node>();
    item->kind = node_kind::string;
    item->name = std::move(name);
    item->value = std::move(value);
    children.push_back(std::move(item));
}

inline void add_list(std::vector<std::unique_ptr<node>>& children,
                     std::string name,
                     std::vector<std::string> items)
{
    auto item = std::make_unique<node>();
    item->kind = node_kind::list;
    item->name = std::move(name);
    for (std::string& value : items) {
        auto child = std::make_unique<node>();
        child->kind = node_kind::string;
        child->value = std::move(value);
        item->children.push_back(std::move(child));
    }
    children.push_back(std::move(item));
}

inline void append_bounded(std::string& output,
                           std::string_view value,
                           std::size_t maximum)
{
    if (value.size() > maximum || output.size() > maximum - value.size())
        fail(ORM_STATUS_LIMIT_EXCEEDED, "rendered query exceeds its byte limit");
    output.append(value.data(), value.size());
}

struct render_state {
    std::string& output;
    std::size_t maximum;
    bool failed = false;
    orm_status_t status = ORM_STATUS_OK;
    std::string message;
};

inline int dump(void* node_ptr,
                int (*out_fn)(const char*, size_t, void*),
                void* renderer_data,
                void*)
{
    auto* current = static_cast<node*>(node_ptr);
    if (current->kind == node_kind::string)
        return out_fn(current->value.data(), current->value.size(), renderer_data);
    return 0;
}

inline void* get_root(void* provider_data)
{
    return provider_data;
}

inline void* get_child(void* node_ptr,
                       const char* name,
                       size_t size,
                       void*)
{
    auto* current = static_cast<node*>(node_ptr);
    if (current->kind != node_kind::map)
        return nullptr;
    const std::string_view key(name, size);
    for (const auto& child : current->children) {
        if (child->name == key)
            return child.get();
    }
    return nullptr;
}

inline void* get_index(void* node_ptr, unsigned index, void*)
{
    auto* current = static_cast<node*>(node_ptr);
    if (current->kind == node_kind::list)
        return index < current->children.size() ? current->children[index].get() : nullptr;
    return index == 0 ? node_ptr : nullptr;
}

inline int verbatim(const char* output, size_t size, void* data)
{
    auto* state = static_cast<render_state*>(data);
    try {
        append_bounded(state->output, {output, size}, state->maximum);
    } catch (const status_error& error) {
        state->failed = true;
        state->status = error.status();
        state->message = error.what();
        return 1;
    } catch (...) {
        state->failed = true;
        state->status = ORM_STATUS_INTERNAL_ERROR;
        state->message = "unexpected failure while rendering a template";
        return 1;
    }
    return 0;
}

inline void render(const MUSTACHE_TEMPLATE* templ,
                   node& root,
                   std::string& output,
                   std::size_t maximum)
{
    require(templ != nullptr, ORM_STATUS_INTERNAL_ERROR,
            "Mustache template is null");
    MUSTACHE_RENDERER renderer{verbatim, verbatim};
    MUSTACHE_DATAPROVIDER provider{
        dump, get_root, get_child, get_index, nullptr, nullptr, nullptr};
    render_state state{output, maximum};

    const int status =
        mustache_process(templ, &renderer, &state, &provider, &root);
    if (state.failed)
        fail(state.status, std::move(state.message));
    require(status == 0, ORM_STATUS_INTERNAL_ERROR,
            "Mustache rendering failed");
}

} // namespace mustache_detail
} // namespace orm_c_detail

#endif
