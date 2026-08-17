#include "orm_schema_validator.hpp"

extern "C" {
#include <schema_parser_dsl.h>
#include <tbe_error.h>
#include <turbo_fs.h>
}

#include <cstdio>
#include <vector>

namespace {

void print_diagnostic(const orm::schema::diagnostic& diagnostic) {
  std::fputs("orm schema: ", stderr);
  if (!diagnostic.entity.empty()) {
    std::fputs(diagnostic.entity.c_str(), stderr);
  }
  if (!diagnostic.field.empty()) {
    std::fprintf(stderr, ".%s", diagnostic.field.c_str());
  }
  if (!diagnostic.entity.empty() || !diagnostic.field.empty()) {
    std::fputs(": ", stderr);
  }
  std::fputs(diagnostic.message.c_str(), stderr);
  std::fputc('\n', stderr);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <schema-file>\n", argc > 0 ? argv[0] : "orm_schema_validate");
    return 2;
  }

  turbo_fs_buf_t buffer{};
  if (turbo_fs_read_file(argv[1], &buffer) != 0) {
    std::fprintf(stderr, "orm schema: failed to read '%s'\n", argv[1]);
    return 2;
  }
  Node* root = create_node_map("root");
  if (root == nullptr) {
    turbo_fs_buf_free(&buffer);
    std::fputs("orm schema: failed to allocate parser root\n", stderr);
    return 2;
  }

  tbe_error_t parse_error{};
  const int parse_result = parse_schema(buffer.base, buffer.len, root, &parse_error);
  turbo_fs_buf_free(&buffer);
  if (parse_result != 0) {
    std::fprintf(stderr, "orm schema: parse error at %d:%d: %s\n", parse_error.line,
                 parse_error.column, parse_error.message);
    node_free(root);
    return 1;
  }

  std::vector<orm::schema::diagnostic> diagnostics;
  const bool valid = orm::schema::validate(root, diagnostics);
  for (const auto& diagnostic : diagnostics) {
    print_diagnostic(diagnostic);
  }
  node_free(root);
  return valid ? 0 : 1;
}
