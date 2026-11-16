/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Transactions: a commit is seen by another session and a rollback by
 * nobody; uncommitted work is invisible outside its session; a
 * savepoint rolls back part of a transaction and keeps the rest; a
 * transaction with a failing statement leaves no trace (atomicity).
 */

#define TE_TEST_NAME    "sql/transactions"

#include "te_config.h"

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define TABLE   "t_tx"
#define TOTAL   "SELECT sum(balance) FROM " TABLE

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TABLE };
    /* A transfer whose second half breaks the primary key. */
    static const char *const broken[] = {
        "UPDATE " TABLE " SET balance = balance - 50 WHERE id = 1",
        "INSERT INTO " TABLE " VALUES (2, 50)",
    };

    tsapi_sql_sess sess = { 0 };
    tapi_sql_conn *conn = NULL;
    tapi_sql_conn *other = NULL;
    const char    *backend;
    int64_t        total = 0;
    double         totals[5];

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_tx"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &other));

    TEST_STEP("Two accounts, 100 each");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " TABLE " ("
                              "id INTEGER PRIMARY KEY, "
                              "balance INTEGER NOT NULL)", NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE
                              " VALUES (1, 100), (2, 100)", NULL));
    CHECK_RC(tapi_sql_query_int(conn, TOTAL, &total));
    totals[0] = (double)total;

    TEST_STEP("Uncommitted work is not seen by another session");
    CHECK_RC(tapi_sql_begin(conn));
    CHECK_RC(tapi_sql_execute(conn, "UPDATE " TABLE
                              " SET balance = balance + 1000 WHERE id = 1",
                              NULL));
    CHECK_RC(tapi_sql_query_int(other, TOTAL, &total));
    if (total != 200)
        TEST_VERDICT("Another session saw uncommitted work at the default "
                     "isolation level");

    TEST_STEP("A rollback undoes it");
    CHECK_RC(tapi_sql_rollback(conn));
    CHECK_RC(tapi_sql_query_int(conn, TOTAL, &total));
    totals[1] = (double)total;
    if (total != 200)
        TEST_VERDICT("A rolled back update is still there");

    TEST_STEP("A commit is seen by another session");
    CHECK_RC(tapi_sql_begin(conn));
    CHECK_RC(tapi_sql_execute(conn, "UPDATE " TABLE
                              " SET balance = balance - 30 WHERE id = 1",
                              NULL));
    CHECK_RC(tapi_sql_execute(conn, "UPDATE " TABLE
                              " SET balance = balance + 30 WHERE id = 2",
                              NULL));
    CHECK_RC(tapi_sql_commit(conn));
    CHECK_RC(tapi_sql_assert_number(other, "SELECT balance FROM " TABLE
                                    " WHERE id = 2", 130.0, 0.0, 0.0));
    CHECK_RC(tapi_sql_query_int(other, TOTAL, &total));
    totals[2] = (double)total;

    TEST_STEP("A savepoint rolls back part and keeps the rest");
    CHECK_RC(tapi_sql_begin(conn));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE " VALUES (3, 7)",
                              NULL));
    CHECK_RC(tapi_sql_savepoint(conn, "sp_part"));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE " VALUES (4, 9000)",
                              NULL));
    CHECK_RC(tapi_sql_rollback_to(conn, "sp_part"));
    CHECK_RC(tapi_sql_savepoint(conn, "sp_kept"));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE " VALUES (5, 3)",
                              NULL));
    CHECK_RC(tapi_sql_release(conn, "sp_kept"));
    CHECK_RC(tapi_sql_commit(conn));
    CHECK_RC(tapi_sql_assert_row_count(conn, TABLE, 4));
    CHECK_RC(tapi_sql_assert_row_count(conn, TABLE " WHERE id = 4", 0));
    CHECK_RC(tapi_sql_query_int(conn, TOTAL, &total));
    totals[3] = (double)total;
    if (total != 210)
        TEST_VERDICT("The savepoints left a wrong total");

    TEST_STEP("A savepoint name cannot carry a statement");
    CHECK_RC(tapi_sql_begin(conn));
    rc = tapi_sql_savepoint(conn, "x; DROP TABLE " TABLE);
    CHECK_RC(tapi_sql_rollback(conn));
    if (rc == 0)
        TEST_VERDICT("A savepoint name with a statement in it was accepted");

    TEST_STEP("A transaction with a failing statement leaves no trace");
    CHECK_RC(tapi_sql_assert_atomic(conn, broken, TE_ARRAY_LEN(broken), true,
                                    "SELECT id, balance FROM " TABLE
                                    " ORDER BY id"));
    CHECK_RC(tapi_sql_query_int(conn, TOTAL, &total));
    totals[4] = (double)total;

    tsapi_sql_chart("Total balance after: set-up, rollback, commit, "
                    "savepoints, failed transaction", NULL, NULL, "total",
                    totals, 5);

    TEST_SUCCESS;

cleanup:
    /* A failed step may have left a transaction open. */
    if (conn != NULL)
        tapi_sql_rollback(conn);
    tapi_sql_disconnect(other);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
