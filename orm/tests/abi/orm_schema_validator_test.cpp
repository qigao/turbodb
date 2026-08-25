#include "orm_schema_validator.hpp"
#include "orm_schema_generator.hpp"

extern "C" {
#include <schema_parser_dsl.h>
}
#include <tinytest.hpp>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

bool validate_text(const char* text, std::vector<orm::schema::diagnostic>& diagnostics) {
  Node* root = create_node_map("root");
  if (root == nullptr) {
    return false;
  }
  const int parse_result = parse_schema(text, std::strlen(text), root, nullptr);
  const bool valid = parse_result == 0 && orm::schema::validate(root, diagnostics);
  node_free(root);
  return parse_result == 0 && valid;
}

bool has_message(const std::vector<orm::schema::diagnostic>& diagnostics,
                 const std::string& fragment) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.message.find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

suite("ORM Schema Validator") {
  group("Profile and entities") {
    given("a valid ORM profile") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users)] message User { [id(1)] uint64 id; string name; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it validates") { check(validate_text(schema, diagnostics)); }
      then("it emits no diagnostics") { check_equal(diagnostics.size(), 0); }
    }

    given("an ordinary TBE schema") {
      const char* schema = "message Wire { uint32 value; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it remains outside ORM validation") { check(validate_text(schema, diagnostics)); }
    }

    given("an invalid C++ namespace mapping") {
      const char* schema =
          "schema Store [orm(1), cpp_namespace(\"app::class\")]; "
          "[table(users)] message User { [id(1)] uint64 id; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it fails before generating C++") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "cpp_namespace"));
      }
    }

    given("entity lifecycle callback attributes") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users), pre_persist(before_save), post_load(after_load)] "
          "message User { [id(1)] uint64 id; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it validates and generates one static lifecycle dispatcher") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        check(generated.find("static void lifecycle(") != std::string::npos);
        check(generated.find("lifecycle_event::pre_persist") !=
              std::string::npos);
        check(generated.find("entity.before_save()") != std::string::npos);
        check(generated.find("lifecycle_event::post_load") !=
              std::string::npos);
        check(generated.find("entity.after_load()") != std::string::npos);
      }
      node_free(root);
    }

    given("an invalid lifecycle callback member name") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users), pre_persist(delete)] message User { "
          "[id(1)] uint64 id; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it fails before emitting invalid C++") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics,
                          "callback must name a valid C++ member function"));
      }
    }

    given("a parsed ORM schema normalized into an owning model") {
      const char* schema =
          "schema Store [orm(1), cpp_namespace(\"app::model\")]; "
          "[table(users)] message User { [id(1), column(user_id)] uint64 id; "
          "string name; }";
      Node* root = create_node_map("root");
      orm::schema::schema_model model;
      std::vector<orm::schema::diagnostic> diagnostics;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("validator and generator can consume values after the AST is released") {
        check(orm::schema::normalize(root, model, diagnostics));
        node_free(root);
        root = nullptr;

        check(model.orm_enabled);
        check_equal(model.cpp_namespace.c_str(), "app::model");
        check_equal(model.entities.size(), 1u);
        check_equal(model.entities.front().table.c_str(), "users");
        check_equal(model.entities.front().fields.front().column.c_str(), "user_id");
        check(orm::schema::validate(model, diagnostics));
      }
      if (root != nullptr) {
        node_free(root);
      }
    }
  }

  group("Entity constraints") {
    given("optional primary-key and version fields") {
      const char* schema =
          "schema Store [orm(1)]; [table(users)] message User { "
          "optional [id(1)] uint64 id; optional [version(1)] uint64 version; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects nullable identity and optimistic-lock state") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "primary-key field cannot be optional"));
        check(has_message(diagnostics, "version field cannot be optional"));
      }
    }

    given("an unsupported optional object field") {
      const char* schema =
          "schema Store [orm(1)]; [table(users)] message User { "
          "[id(1)] uint64 id; optional Address address; } "
          "[table(addresses)] message Address { [id(1)] uint64 id; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it fails before typed fetch instantiates an optional nested record") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "optional ORM field must use a supported"));
      }
    }

    given("an enum with a non-integer underlying type") {
      const char* schema =
          "schema Store [orm(1)]; enum State <string> { active = 1; } "
          "[table(users)] message User { [id(1)] uint64 id; State state; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects an unusable C++ enum contract") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "enum underlying type must be a scalar integer"));
      }
    }

    given("a missing table and primary key") {
      const char* schema = "schema Store [orm(1)]; message User { string name; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it fails with both diagnostics") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "requires [table(name)]"));
        check(has_message(diagnostics, "primary key"));
      }
    }

    given("duplicate columns") {
      const char* schema =
          "schema Store [orm(1)]; [table(users)] message User { "
          "[id(1), column(user_id)] uint64 id; [column(user_id)] string name; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it fails") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "duplicate ORM column name"));
      }
    }

    given("multiple direct primary-key fields") {
      const char* schema =
          "schema Store [orm(1)]; [table(users)] message User { "
          "[id(1)] uint64 id; [primary_key(1)] uint64 legacy_id; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it accepts a composite identifier") {
        check(validate_text(schema, diagnostics));
        check_equal(diagnostics.size(), 0u);
      }
    }

    given("an embedded composite identifier") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[embeddable(1)] message UserKey { "
          "[column(tenant_id)] uint64 tenant; [column(user_id)] uint64 user; } "
          "[table(users)] message User { [embedded_id(1)] UserKey key; "
          "string name; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it validates the embeddable key contract") {
        check(validate_text(schema, diagnostics));
        check_equal(diagnostics.size(), 0u);
      }
    }

    given("direct and embedded identifiers on the same entity") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[embeddable(1)] message UserKey { uint64 tenant; uint64 user; } "
          "[table(users)] message User { [id(1)] uint64 legacy_id; "
          "[embedded_id(1)] UserKey key; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects two competing identity contracts") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "either direct id fields or one embedded_id"));
      }
    }

    given("a nullable embedded-id component") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[embeddable(1)] message UserKey { optional uint64 tenant; "
          "uint64 user; } "
          "[table(users)] message User { [embedded_id(1)] UserKey key; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects an identity that cannot always be materialized") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics,
                          "embedded_id components must use required"));
      }
    }

    given("an embedded value that collides with an entity column") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[embeddable(1)] message Address { [column(name)] string city; } "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[embedded(1)] Address address; string name; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects duplicate flattened columns") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "duplicate flattened ORM column"));
      }
    }

    given("one scalar integer version field") {
      const char* schema =
          "schema Store [orm(1)]; [table(users)] message User { "
          "[id(1)] uint64 id; [version(1)] uint64 version; string name; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it enables optimistic locking metadata") {
        check(validate_text(schema, diagnostics));
        check_equal(diagnostics.size(), 0u);
      }
    }

    given("multiple or non-integer version fields") {
      const char* schema =
          "schema Store [orm(1)]; [table(users)] message User { "
          "[id(1)] uint64 id; [version(1)] string revision; "
          "[version(1)] uint64 generation; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects ambiguous optimistic locking") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "scalar integer type"));
        check(has_message(diagnostics, "multiple version fields"));
      }
    }

    given("an entity graph wider than its generated mask") {
      orm::schema::schema_model model;
      model.orm_enabled = true;
      orm::schema::entity_model owner;
      owner.name = "Owner";
      owner.table = "owners";
      owner.table_declared = true;
      orm::schema::field_model owner_id;
      owner_id.name = "id";
      owner_id.type = "uint64";
      owner_id.column = "id";
      owner_id.primary_key = true;
      owner.fields.push_back(owner_id);
      for (std::size_t index = 0;
           index <= orm::schema::max_entity_relations; ++index) {
        orm::schema::field_model relation;
        relation.name = "relation_" + std::to_string(index);
        relation.type = "Target";
        relation.relation = true;
        relation.relation_target = "Target";
        owner.fields.push_back(std::move(relation));
      }
      orm::schema::entity_model target;
      target.name = "Target";
      target.table = "targets";
      target.table_declared = true;
      orm::schema::field_model target_id = owner_id;
      target.fields.push_back(std::move(target_id));
      model.entities.push_back(std::move(owner));
      model.entities.push_back(std::move(target));
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it fails before shifting beyond the 64-bit graph representation") {
        check(!orm::schema::validate(model, diagnostics));
        check(has_message(diagnostics, "64-relation graph limit"));
      }
    }
  }

  group("Single-table inheritance") {
    given("a valid generated hierarchy") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(payments), inheritance(single_table), "
          "discriminator_column(kind), discriminator_value(base)] "
          "message Payment { [id(1)] uint64 id; string label; } "
          "[extends(Payment), discriminator_value(card)] "
          "message CardPayment { string card_last4; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it emits flattened models and a variant hierarchy") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        check(generated.find("std::is_base_of_v<Payment, CardPayment>") !=
              std::string::npos);
        check(generated.find("std::variant<Payment, CardPayment>") !=
              std::string::npos);
        check(generated.find("discriminator_column()") != std::string::npos);
        check(generated.find("&CardPayment::id") != std::string::npos);
        check(generated.find("&CardPayment::card_last4") !=
              std::string::npos);
      }
      node_free(root);
    }

    given("invalid hierarchy contracts") {
      const char* schemas[] = {
          "schema Store [orm(1)]; "
          "[table(roots), inheritance(mapped_superclass), discriminator_column(kind), "
          "discriminator_value(root)] message Root { [id(1)] uint64 id; }",
          "schema Store [orm(1)]; "
          "[table(roots), inheritance(single_table), discriminator_column(kind), "
          "discriminator_value(same)] message Root { [id(1)] uint64 id; } "
          "[extends(Root), discriminator_value(same)] message Child { string value; }",
          "schema Store [orm(1)]; "
          "[table(roots), inheritance(single_table), discriminator_column(kind), "
          "discriminator_value(root)] message Root { [id(1)] uint64 id; string name; } "
          "[extends(Root), discriminator_value(child)] message Child { "
          "[column(name)] string alias; }",
          "schema Store [orm(1)]; "
          "[extends(Missing), discriminator_value(child)] message Child { string value; }"};

      then("each fails before invalid static metadata is emitted") {
        for (const char* schema : schemas) {
          std::vector<orm::schema::diagnostic> diagnostics;
          check(!validate_text(schema, diagnostics));
          check(!diagnostics.empty());
        }
      }
    }

    given("base and subtype callbacks for the same event") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(roots), inheritance(single_table), discriminator_column(kind), "
          "discriminator_value(root), pre_persist(before_root)] "
          "message Root { [id(1)] uint64 id; } "
          "[extends(Root), discriminator_value(child), "
          "pre_persist(before_child)] message Child { string value; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it emits one switch case that calls base before subtype") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        const std::size_t child_model =
            generated.find("struct orm_generated_Child_model");
        const std::size_t event = generated.find(
            "case orm::model::lifecycle_event::pre_persist:", child_model);
        const std::size_t root_callback =
            generated.find("entity.before_root()", event);
        const std::size_t child_callback =
            generated.find("entity.before_child()", event);
        check(child_model != std::string::npos);
        check(event != std::string::npos);
        check(root_callback < child_callback);
        check(generated.find(
                  "case orm::model::lifecycle_event::pre_persist:",
                  event + 1) == std::string::npos);
      }
      node_free(root);
    }

    given("a table-per-class hierarchy") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(assets), inheritance(table_per_class)] "
          "message Asset { [id(1)] uint64 id; string label; } "
          "[extends(Asset), table(images)] "
          "message Image { uint32 width; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it flattens inherited members into the concrete table model") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        const std::size_t image_model =
            generated.find("struct orm_generated_Image_model");
        check(image_model != std::string::npos);
        check(generated.find("return \"images\"", image_model) !=
              std::string::npos);
        check(generated.find("&Image::id", image_model) != std::string::npos);
        check(generated.find("&Image::width", image_model) !=
              std::string::npos);
        check(generated.find("inheritance_strategy::table_per_class",
                             image_model) != std::string::npos);
      }
      node_free(root);
    }

    given("a joined hierarchy") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(documents), inheritance(joined), discriminator_column(kind), "
          "discriminator_value(document)] message Document { "
          "[id(1)] uint64 id; [version(1)] uint64 version; string title; } "
          "[extends(Document), table(pdf_documents), discriminator_value(pdf)] "
          "message PdfDocument { uint32 pages; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it assigns inherited and local columns to separate fragments") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        const std::size_t pdf_model =
            generated.find("struct orm_generated_PdfDocument_model");
        check(pdf_model != std::string::npos);
        check(generated.find("inheritance_strategy::joined", pdf_model) !=
              std::string::npos);
        check(generated.find("\"documents\", \"pdf_documents\"",
                             pdf_model) != std::string::npos);
        check(generated.find(
                  "\"documents\", \"documents\", \"documents\", "
                  "\"pdf_documents\"",
                  pdf_model) != std::string::npos);
      }
      node_free(root);
    }

    given("a table-per-class subtype without a table") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(assets), inheritance(table_per_class)] "
          "message Asset { [id(1)] uint64 id; } "
          "[extends(Asset)] message Image { uint32 width; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects the incomplete physical layout") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "requires its own [table(name)]"));
      }
    }

    given("a C facade request for a C++ hierarchy") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(roots), inheritance(single_table), discriminator_column(kind), "
          "discriminator_value(root)] message Root { [id(1)] uint64 id; } "
          "[extends(Root), discriminator_value(child)] message Child { string value; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it rejects the incompatible C object layout explicitly") {
        check(!orm::schema::generate_c(root, "store.h", generated,
                                       diagnostics));
        check(has_message(diagnostics, "does not support C++ single-table"));
      }
      node_free(root);
    }
  }

  group("Relations") {
    given("a relation with a valid mapping") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[relation(Order), mapped_by(user_id), cascade(all)] Order order; } "
          "[table(orders)] message Order { [id(1)] uint64 id; uint64 user_id; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it validates") { check(validate_text(schema, diagnostics)); }
    }

    given("lifecycle cascade values") {
      const char* values[] = {"merge", "refresh", "detach"};

      then("each value validates as relation metadata") {
        for (const char* value : values) {
          const std::string schema =
              "schema Store [orm(1)]; "
              "[table(users)] message User { [id(1)] uint64 id; "
              "[relation(Order), cascade(" +
              std::string(value) +
              ")] Order order; } "
              "[table(orders)] message Order { [id(1)] uint64 id; }";
          std::vector<orm::schema::diagnostic> diagnostics;
          check(validate_text(schema.c_str(), diagnostics));
        }
      }
    }

    given("an unknown target and invalid cascade") {
      const char* schema =
          "schema Store [orm(1)]; [table(users)] message User { [id(1)] uint64 id; "
          "[relation(Missing), cascade(explode)] Order orders; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it fails with relation diagnostics") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "relation target entity does not exist"));
        check(has_message(diagnostics, "invalid cascade value"));
      }
    }

    given("a missing relation mapping field") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[relation(Order), mapped_by(user_id)] Order order; } "
          "[table(orders)] message Order { [id(1)] uint64 id; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects the mapping") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "mapping field does not exist"));
      }
    }

    given("a relation mapping with a different key type") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[relation(Order), mapped_by(user_id)] Order order; } "
          "[table(orders)] message Order { [id(1)] uint64 id; uint32 user_id; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects the type mismatch") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "must match the owner primary key type"));
      }
    }

    given("orphan removal on a scalar field") {
      const char* schema =
          "schema Store [orm(1)]; [table(users)] message User { "
          "[id(1)] uint64 id; [orphan_removal(1)] string name; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects the annotation") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "only valid on a relation field"));
      }
    }

    given("a set-valued relation") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[relation(Order)] set<Order> orders; } "
          "[table(orders)] message Order { [id(1)] uint64 id; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it is rejected because the runtime relation container is a vector") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "must use list<T>"));
      }
    }

    given("valid relation fetch strategies") {
      const char* values[] = {"lazy", "eager"};

      then("each strategy validates with an explicit relation mapping") {
        for (const char* value : values) {
          const std::string schema =
              "schema Store [orm(1)]; "
              "[table(users)] message User { [id(1)] uint64 id; "
              "[relation(Order), mapped_by(user_id), fetch(" +
              std::string(value) +
              ")] Order order; } "
              "[table(orders)] message Order { [id(1)] uint64 id; uint64 user_id; }";
          std::vector<orm::schema::diagnostic> diagnostics;
          check(validate_text(schema.c_str(), diagnostics));
        }
      }
    }

    given("an invalid or unmapped fetch declaration") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[relation(Order), fetch(batch)] Order order; "
          "[fetch(lazy)] string name; } "
          "[table(orders)] message Order { [id(1)] uint64 id; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it fails before generating an unusable accessor") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "fetch must be lazy or eager"));
        check(has_message(diagnostics, "fetch is only valid on a relation field"));
      }
    }

    given("a valid fetch strategy without a relation mapping") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[relation(Order), fetch(lazy)] Order order; } "
          "[table(orders)] message Order { [id(1)] uint64 id; }";
      std::vector<orm::schema::diagnostic> diagnostics;

      then("it rejects a relation that cannot construct a foreign-key query") {
        check(!validate_text(schema, diagnostics));
        check(has_message(diagnostics, "fetch relation requires mapped_by or foreign_key"));
      }
    }
  }

  group("C++ metadata generation") {
    given("optional and enum ORM fields") {
      const char* schema =
          "schema Store [orm(1)]; enum ProfileState <uint8> { inactive = 0; "
          "active = 1; } [table(profiles)] message Profile { "
          "[id(1)] uint64 id; ProfileState state; optional string nickname; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it emits compile-time member type checks") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        check(generated.find("decltype(Profile::state), ProfileState") !=
              std::string::npos);
        check(generated.find("decltype(Profile::nickname), std::optional<std::string>") !=
              std::string::npos);
        check(generated.find("std::underlying_type_t<ProfileState>, std::uint8_t") !=
              std::string::npos);
      }
      node_free(root);
    }

    given("an ORM schema with mapped column names") {
      const char* schema =
          "schema Store [orm(1)]; [table(users)] message User { "
          "[primary_key(1), column(user_id)] uint64 id; "
          "[version(1), column(entity_version)] uint64 version; "
          "[column(display_name)] string name; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it generates member, column, table, and primary-key metadata") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        check(generated.find("&User::id") != std::string::npos);
        check(generated.find("\"user_id\"") != std::string::npos);
        check(generated.find("\"display_name\"") != std::string::npos);
        check(generated.find("entity_model_func(User const&)") != std::string::npos);
        check(generated.find("reflect_members_func") == std::string::npos);
        check(generated.find("primary_key()") != std::string::npos);
        check(generated.find("version()") != std::string::npos);
        check(generated.find("\"entity_version\"") != std::string::npos);
      }
      node_free(root);
    }

    given("a non-ORM TBE schema") {
      const char* schema = "message Wire { uint32 value; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("generation fails instead of silently emitting empty metadata") {
        check(!orm::schema::generate_cpp(root, generated, diagnostics));
        check(has_message(diagnostics, "requires schema [orm(1)]"));
      }
      node_free(root);
    }

    given("a namespaced ORM relation") {
      const char* schema =
          "schema Store [orm(1), cpp_namespace(\"app::model\")]; "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[relation(Order), mapped_by(user_id), cascade(all), orphan_removal(1)] "
          "Order order; } "
          "[table(orders)] message Order { [id(1)] uint64 id; uint64 user_id; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it generates the namespace foreign-key binding and cascade policy") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        check(generated.find("namespace app::model") != std::string::npos);
        check(generated.find("static auto order()") != std::string::npos);
        check(generated.find("static auto relations()") != std::string::npos);
        check(generated.find("&User::id") != std::string::npos);
        check(generated.find("&Order::user_id") != std::string::npos);
        check(generated.find("cascade_policy::all | ") != std::string::npos);
        check(generated.find("cascade_policy::orphan_remove") != std::string::npos);
      }
      node_free(root);
    }

    given("mapped lazy and eager fetch relations") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[relation(Order), mapped_by(user_id), fetch(lazy)] Order order; "
          "[relation(Order), mapped_by(user_id), fetch(eager)] list<Order> orders; } "
          "[table(orders)] message Order { [id(1)] uint64 id; "
          "[column(owner_id)] uint64 user_id; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it generates typed accessors with resolved database column names") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        check(generated.find("fetch_order") != std::string::npos);
        check(generated.find("defer_one<Order>") != std::string::npos);
        check(generated.find("fetch_orders") != std::string::npos);
        check(generated.find("find_many<Order>") != std::string::npos);
        check(generated.find("\"owner_id\", owner.id") != std::string::npos);
        check(generated.find("static void load_eager") != std::string::npos);
        check(generated.find("loaded_orders") != std::string::npos);
        check(generated.find("owner.orders.push_back(*child)") != std::string::npos);
        check(generated.find("static auto eager_relations()") != std::string::npos);
        check(generated.find("return std::make_tuple(orders())") != std::string::npos);
        check(generated.find("initialize_order") != std::string::npos);
        check(generated.find("order_graph()") != std::string::npos);
        check(generated.find("loadable_graph()") != std::string::npos);
        check(generated.find("for_each_relation") != std::string::npos);
      }
      node_free(root);
    }

    given("a list-valued ORM relation") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[relation(Order)] list<Order> orders; } "
          "[table(orders)] message Order { [id(1)] uint64 id; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it generates a many descriptor") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        check(generated.find("orm::relation::many") != std::string::npos);
        check(generated.find("cascade_policy::none") != std::string::npos);
      }
      node_free(root);
    }

    given("a lifecycle cascade relation") {
      const char* schema =
          "schema Store [orm(1)]; "
          "[table(users)] message User { [id(1)] uint64 id; "
          "[relation(Order), cascade(detach)] Order order; } "
          "[table(orders)] message Order { [id(1)] uint64 id; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it emits the lifecycle policy in the descriptor") {
        check(orm::schema::generate_cpp(root, generated, diagnostics));
        check(generated.find("cascade_policy::detach") != std::string::npos);
      }
      node_free(root);
    }
  }

  group("C facade generation") {
    given("an ORM schema backed by a generated TBE C model") {
      const char* schema =
          "schema CStore [orm(1)]; enum State <uint8> { inactive = 0; active = 1; } "
          "[table(users)] message User { "
          "[id(1), column(user_id)] uint64 id; "
          "[version(1), column(entity_version)] uint64 version; "
          "State state; [column(display_name)] string name; "
          "optional string nickname; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it emits typed CRUD and optimistic-lock operations") {
        check(orm::schema::generate_c(root, "c_store.tbe.h", generated,
                                      diagnostics));
        check(generated.find("#include \"c_store.tbe.h\"") != std::string::npos);
        check(generated.find("CStore_User_orm_find") != std::string::npos);
        check(generated.find("CStore_User_orm_insert") != std::string::npos);
        check(generated.find("CStore_User_orm_update") != std::string::npos);
        check(generated.find("CStore_User_orm_remove") != std::string::npos);
        check(generated.find("Profile_OPTIONAL") == std::string::npos);
        check(generated.find("User_OPTIONAL_nickname") != std::string::npos);
        check(generated.find("ORM optimistic lock conflict") != std::string::npos);
      }
      node_free(root);
    }

    given("a TBE C member mapped onto a C keyword") {
      const char* schema =
          "schema CStore [orm(1)]; [table(users)] message User { "
          "[id(1)] uint64 id; [c(restrict)] string name; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("it rejects the facade before emitting invalid C") {
        check(!orm::schema::generate_c(root, "c_store.tbe.h", generated,
                                       diagnostics));
        check(has_message(diagnostics, "valid C identifier"));
      }
      node_free(root);
    }

    given("a direct composite identity contract") {
      const char* schema =
          "schema CStore [orm(1)]; [table(users)] message User { "
          "[id(1)] uint64 tenant; [id(1)] uint64 user; string name; }";
      Node* root = create_node_map("root");
      std::vector<orm::schema::diagnostic> diagnostics;
      std::string generated;
      check_not_null(root);
      check_equal(parse_schema(schema, std::strlen(schema), root, nullptr), 0);

      then("the C facade emits every key parameter and predicate") {
        check(orm::schema::generate_c(root, "c_store.tbe.h", generated,
                                      diagnostics));
        check(diagnostics.empty());
        check(generated.find("uint64_t primary_tenant, uint64_t primary_user") !=
              std::string::npos);
        check(generated.find("orm_u64((uint64_t)(primary_tenant))") !=
              std::string::npos);
        check(generated.find("orm_u64((uint64_t)(primary_user))") !=
              std::string::npos);
        check(generated.find("orm_u64((uint64_t)(entity->tenant))") !=
              std::string::npos);
        check(generated.find("orm_u64((uint64_t)(entity->user))") !=
              std::string::npos);
      }
      node_free(root);
    }
  }
}
