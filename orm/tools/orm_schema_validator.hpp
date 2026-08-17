#ifndef ORM_SCHEMA_VALIDATOR_HPP
#define ORM_SCHEMA_VALIDATOR_HPP

#include "orm_schema_model.hpp"

namespace orm::schema {

/*
 * Validate the ORM annotations carried by a parsed TBE schema.
 *
 * The validator is intentionally build-time only. It reads the public TBE
 * Node tree and never retains pointers into it after the call returns.
 * Schemas without [orm(1)] are ordinary TBE schemas and validate successfully
 * without applying ORM rules.
 */
bool validate(const Node* root, std::vector<diagnostic>& diagnostics);

/* Validate an already-normalized model without revisiting the TBE AST. */
bool validate(const schema_model& model, std::vector<diagnostic>& diagnostics);

}  // namespace orm::schema

#endif  // ORM_SCHEMA_VALIDATOR_HPP
