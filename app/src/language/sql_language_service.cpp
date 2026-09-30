#include "language/sql_language_service.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace turbodb::app {

namespace {

constexpr const char* kSqlKeywords =
    "select from where join inner left right full on as and or not null "
    "insert into values update set delete create alter drop table view index "
    "with recursive union all distinct group by having order limit offset "
    "begin commit rollback savepoint release explain analyze pragma returning";

constexpr const char* kSqliteKeywords =
    "attach detach vacuum reindex without rowid strict temp temporary";

constexpr const char* kMysqlKeywords =
    "show describe use database databases engine engines delimiter replace "
    "straight_join sql_calc_found_rows";

constexpr const char* kPostgresqlKeywords =
    "returning lateral ilike serial bigserial jsonb materialized concurrently "
    "tablespace unlogged generated identity";

std::string Lower(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (unsigned char ch : text) {
    result.push_back(static_cast<char>(std::tolower(ch)));
  }
  return result;
}

bool HasPrefixInsensitive(std::string_view value, std::string_view prefix) {
  if (prefix.size() > value.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    const auto left = static_cast<unsigned char>(value[i]);
    const auto right = static_cast<unsigned char>(prefix[i]);
    if (std::tolower(left) != std::tolower(right)) return false;
  }
  return true;
}

void AddWords(std::set<std::string>& output, std::string_view words,
              std::string_view prefix) {
  std::size_t start = 0;
  while (start < words.size()) {
    while (start < words.size() && words[start] == ' ') ++start;
    if (start >= words.size()) break;
    const auto end = words.find(' ', start);
    const auto word = words.substr(
        start, end == std::string_view::npos ? words.size() - start
                                             : end - start);
    if (HasPrefixInsensitive(word, prefix)) output.emplace(word);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
}

}  // namespace

std::string SqlLanguageService::LexerName(SqlProvider provider) const {
  if (provider == SqlProvider::mysql) return "mysql";
  return "sql";
}

std::string SqlLanguageService::KeywordList(SqlProvider provider) const {
  std::string result = kSqlKeywords;
  switch (provider) {
    case SqlProvider::sqlite:
      result += ' ';
      result += kSqliteKeywords;
      break;
    case SqlProvider::mysql:
      result += ' ';
      result += kMysqlKeywords;
      break;
    case SqlProvider::postgresql:
      result += ' ';
      result += kPostgresqlKeywords;
      break;
    default:
      break;
  }
  return result;
}

std::vector<std::string> SqlLanguageService::Complete(
    const SqlWorkspaceSession& session, std::string_view prefix) const {
  const std::string normalized_prefix = Lower(prefix);
  std::set<std::string> matches;

  AddWords(matches, KeywordList(session.provider()), normalized_prefix);

  for (const auto& relation : session.relations()) {
    if (HasPrefixInsensitive(relation.name, normalized_prefix)) {
      matches.emplace(relation.name);
    }
    if (!relation.schema.empty()) {
      const std::string qualified = relation.schema + "." + relation.name;
      if (HasPrefixInsensitive(qualified, normalized_prefix)) {
        matches.emplace(qualified);
      }
    }
    for (const auto& column : relation.columns) {
      if (HasPrefixInsensitive(column, normalized_prefix)) {
        matches.emplace(column);
      }
      const std::string qualified = relation.name + "." + column;
      if (HasPrefixInsensitive(qualified, normalized_prefix)) {
        matches.emplace(qualified);
      }
    }
  }

  return {matches.begin(), matches.end()};
}

}  // namespace turbodb::app
