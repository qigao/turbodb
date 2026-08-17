#ifndef ORM_MONGO_QUERY_HPP
#define ORM_MONGO_QUERY_HPP

/*
 * Pure translation of the ORM query plan into MongoDB BSON documents.
 *
 * These functions never touch a live server; they are shared by the
 * mongoc-backed backend and the server-less unit test. The configured
 * id_column is stored under MongoDB's reserved "_id" field so inserts get
 * native duplicate-key rejection and point lookups stay indexed; all other
 * columns map to same-named document fields.
 */

#include "orm_c_internal.hpp"

#include <bson/bson.h>

#include <string>
#include <string_view>

namespace orm_c_detail {

struct mongo_settings {
    std::string uri = "mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=5000";
    std::string database; // required
    std::string id_column = "id";
};

/* Translates a SQL LIKE pattern into an anchored PCRE pattern. */
std::string mongo_like_pattern(std::string_view sql_like);

/* "count(*)", "sum(col)", ... -- the names HAVING predicates refer to. */
std::string mongo_aggregate_sql_name(const aggregate_expression& aggregate);

/* Internal pipeline output field names shared with the result materializer. */
inline std::string mongo_group_key_name(std::size_t index)
{
    return "k" + std::to_string(index);
}

inline std::string mongo_aggregate_value_name(std::size_t index)
{
    return "a" + std::to_string(index);
}

/* Fills out with the find filter for the where condition tree. */
void mongo_append_filter(bson_t* out,
                         const condition_node& root,
                         const mongo_settings& settings);

/* Fills out with projection/sort/skip/limit options for a non-aggregate find. */
void mongo_append_find_options(bson_t* out,
                               const query_plan& plan,
                               const mongo_settings& settings);

/* Fills out with the aggregation pipeline for group/aggregate/having plans. */
void mongo_append_pipeline(bson_t* out,
                           const query_plan& plan,
                           const mongo_settings& settings);

/* Fills out with the stored document for an INSERT assignment list. */
void mongo_append_insert_document(bson_t* out,
                                  const query_plan& plan,
                                  const mongo_settings& settings);

/* Fills out with the $set update document for an UPDATE assignment list. */
void mongo_append_update_document(bson_t* out,
                                  const query_plan& plan,
                                  const mongo_settings& settings);

/* Fills out with the {"_id": value} filter for a validated id predicate. */
void mongo_append_id_filter(bson_t* out,
                            const bound_parameter& id,
                            const mongo_settings& settings);

} // namespace orm_c_detail

#endif
