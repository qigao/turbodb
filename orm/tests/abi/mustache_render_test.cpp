#include "mustache_render.hpp"
#include "mustache/mustache.h"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void test_basic_rendering() {
    constexpr const char raw_template[] = "SELECT {{{columns}}} FROM {{{table}}};";
    std::unique_ptr<MUSTACHE_TEMPLATE, void (*)(MUSTACHE_TEMPLATE*)> templ(
        mustache_compile(raw_template, sizeof(raw_template) - 1, nullptr, nullptr, 0),
        &mustache_release);
    assert(templ != nullptr);

    orm_c_detail::mustache_detail::node root;
    orm_c_detail::mustache_detail::add_string(root.children, "columns", "id, name");
    orm_c_detail::mustache_detail::add_string(root.children, "table", "users");

    std::string output;
    orm_c_detail::mustache_detail::render(templ.get(), root, output, 1024);
    assert(output == "SELECT id, name FROM users;");
}

void test_list_rendering() {
    constexpr const char raw_template[] = "JOIN {{#joins}}{{{.}}}{{/joins}}";
    std::unique_ptr<MUSTACHE_TEMPLATE, void (*)(MUSTACHE_TEMPLATE*)> templ(
        mustache_compile(raw_template, sizeof(raw_template) - 1, nullptr, nullptr, 0),
        &mustache_release);
    assert(templ != nullptr);

    orm_c_detail::mustache_detail::node root;
    orm_c_detail::mustache_detail::add_list(root.children, "joins", {"t1 ON a=b", " t2 ON c=d"});

    std::string output;
    orm_c_detail::mustache_detail::render(templ.get(), root, output, 1024);
    assert(output == "JOIN t1 ON a=b t2 ON c=d");
}

void test_size_limit_exceeded() {
    constexpr const char raw_template[] = "SELECT {{{big_string}}}";
    std::unique_ptr<MUSTACHE_TEMPLATE, void (*)(MUSTACHE_TEMPLATE*)> templ(
        mustache_compile(raw_template, sizeof(raw_template) - 1, nullptr, nullptr, 0),
        &mustache_release);
    assert(templ != nullptr);

    orm_c_detail::mustache_detail::node root;
    orm_c_detail::mustache_detail::add_string(root.children, "big_string", "0123456789ABCDEF");

    std::string output;
    bool threw = false;
    try {
        orm_c_detail::mustache_detail::render(templ.get(), root, output, 10);
    } catch (const orm_c_detail::status_error& err) {
        (void)err;
        threw = true;
        assert(err.status() == ORM_STATUS_LIMIT_EXCEEDED);
    }
    assert(threw);
}

void test_null_template() {
    orm_c_detail::mustache_detail::node root;
    std::string output;
    bool threw = false;
    try {
        orm_c_detail::mustache_detail::render(nullptr, root, output, 1024);
    } catch (const orm_c_detail::status_error& err) {
        (void)err;
        threw = true;
        assert(err.status() == ORM_STATUS_INTERNAL_ERROR);
    }
    assert(threw);
}

} // namespace

int main() {
    test_basic_rendering();
    test_list_rendering();
    test_size_limit_exceeded();
    test_null_template();
    std::cout << "All mustache_render_test assertions passed successfully!\n";
    return 0;
}
