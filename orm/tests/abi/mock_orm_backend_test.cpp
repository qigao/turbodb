// <tinytest.h> must be included before mock_orm_backend.hpp: the mock header
// pulls in tinymock.hpp, which defines TINYTEST_NO_MAIN to suppress the
// built-in runner main().  Including tinytest.h first keeps the suite/it
// runner available to this executable.
#include <tinytest.hpp>

#include "mock_orm_backend.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using orm_c_detail::bound_parameter;
using orm_c_detail::connection_limits;

} // namespace

suite("orm mock backend") {
 
    it("serves canned rows, columns, nulls, and cells") {
        orm_mock::mock_result_backend result(
            1, 2, 0, {{false, "A"}, {true, ""}});
        check_equal(result.rows(), 1u);
        check_equal(result.columns(), 2u);
        check_true(result.is_null(0, 1));
        check(std::string(result.cell(0, 0).data, result.cell(0, 0).len) == "A");
    }

    it("routes SQL through a scripted tinymock expectation") {
        orm_mock::mock_database_backend db;
        db.execute_sql_mock.on(
            [](std::string_view sql, const std::vector<bound_parameter>&, bool,
               const connection_limits&) { return sql == "select 1"; },
            [](std::string_view, const std::vector<bound_parameter>&, bool,
               const connection_limits&) {
                return std::make_unique<orm_mock::mock_result_backend>(0, 0, 1);
            });

        auto query_result =
            db.execute_sql("select 1", {}, false, connection_limits{});
        check(query_result != nullptr);
        if (query_result != nullptr)
            check_equal(query_result->affected_rows(), 1u);
        db.execute_sql_mock.verify();
    }

    it("returns a transaction that tracks commit state") {
        orm_mock::mock_database_backend db;
        db.begin_transaction_mock.then_return([](orm_isolation_t) {
            return std::make_unique<orm_mock::mock_transaction_backend>();
        });

        auto transaction = db.begin_transaction(ORM_ISOLATION_SERIALIZABLE);
        check(transaction != nullptr);
        if (transaction == nullptr)
            return;
        auto* mock_tx =
            static_cast<orm_mock::mock_transaction_backend*>(transaction.get());
        check_true(mock_tx->active());
        transaction->commit();
        check_false(mock_tx->active());
        check_true(mock_tx->committed());
        db.begin_transaction_mock.verify();
    }
}
