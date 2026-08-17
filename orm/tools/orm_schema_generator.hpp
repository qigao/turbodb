#ifndef ORM_SCHEMA_GENERATOR_HPP
#define ORM_SCHEMA_GENERATOR_HPP

#include "orm_schema_validator.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace orm::schema {

/* Generate entity-model metadata for existing C++ structs declared by the user. */
bool generate_cpp(const Node* root, std::string& output,
                  std::vector<diagnostic>& diagnostics);

/* Generate a C11 CRUD facade over tbe_compiler owning Type_t models. */
bool generate_c(const Node* root, std::string_view model_header,
                std::string& output,
                std::vector<diagnostic>& diagnostics);

}  // namespace orm::schema

#endif  // ORM_SCHEMA_GENERATOR_HPP
