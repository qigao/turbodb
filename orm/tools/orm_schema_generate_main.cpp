#include "orm_schema_generator.hpp"

extern "C" {
#include <schema_parser_dsl.h>
#include <tbe_error.h>
#include <turbo_fs.h>
}

#include <cstdio>
#include <cstring>
#include <string>
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
  std::fprintf(stderr, "%s\n", diagnostic.message.c_str());
}

void print_usage(const char* executable) {
  std::fprintf(
      stderr,
      "usage: %s [--language cpp|c] [--model-header <header>] "
      "<schema-file> <output-header>\n",
      executable != nullptr ? executable : "orm_schema_generate");
}

}  // namespace

int main(int argc, char** argv) {
  std::string language = "cpp";
  std::string model_header;
  std::vector<const char*> positional;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--language") == 0) {
      if (++index >= argc) {
        print_usage(argc > 0 ? argv[0] : nullptr);
        return 2;
      }
      language = argv[index];
    } else if (std::strcmp(argv[index], "--model-header") == 0) {
      if (++index >= argc) {
        print_usage(argc > 0 ? argv[0] : nullptr);
        return 2;
      }
      model_header = argv[index];
    } else if (argv[index][0] == '-') {
      print_usage(argc > 0 ? argv[0] : nullptr);
      return 2;
    } else {
      positional.push_back(argv[index]);
    }
  }
  if (positional.size() != 2u || (language != "cpp" && language != "c") ||
      (language == "c" && model_header.empty()) ||
      (language == "cpp" && !model_header.empty())) {
    print_usage(argc > 0 ? argv[0] : nullptr);
    return 2;
  }

  turbo_fs_buf_t input{};
  if (turbo_fs_read_file(positional[0], &input) != 0) {
    std::fprintf(stderr, "orm schema: failed to read '%s'\n", positional[0]);
    return 2;
  }
  Node* root = create_node_map("root");
  if (root == nullptr) {
    turbo_fs_buf_free(&input);
    std::fputs("orm schema: failed to allocate parser root\n", stderr);
    return 2;
  }

  tbe_error_t parse_error{};
  const int parse_result = parse_schema(input.base, input.len, root, &parse_error);
  turbo_fs_buf_free(&input);
  if (parse_result != 0) {
    std::fprintf(stderr, "orm schema: parse error at %d:%d: %s\n", parse_error.line,
                 parse_error.column, parse_error.message);
    node_free(root);
    return 1;
  }

  std::string generated;
  std::vector<orm::schema::diagnostic> diagnostics;
  const bool valid = language == "c"
                         ? orm::schema::generate_c(root, model_header, generated,
                                                   diagnostics)
                         : orm::schema::generate_cpp(root, generated, diagnostics);
  node_free(root);
  if (!valid) {
    for (const auto& diagnostic : diagnostics) {
      print_diagnostic(diagnostic);
    }
    return 1;
  }

  turbo_fs_buf_t output{generated.data(), generated.size()};
  if (turbo_fs_write_file(positional[1], &output) != 0) {
    std::fprintf(stderr, "orm schema: failed to write '%s'\n", positional[1]);
    return 2;
  }
  return 0;
}
