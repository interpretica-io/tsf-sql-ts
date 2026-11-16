/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Create, read, update, delete: each statement changes the rows it
 * says it changes, reports how many, and a query shows the result.
 */

#define TE_TEST_NAME    "sql/crud"

#include "te_config.h"

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define TABLE   "t_crud"

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TABLE };

    tsapi_sql_sess  sess = { 0 };
    tapi_sql_conn  *conn = NULL;
    tapi_sql_result got = TAPI_SQL_RESULT_INIT;
    const char     *backend;
    int64_t         affected = 0;
    double          changed[4];

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_crud"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("Create a table and insert rows");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " TABLE " ("
                              "id INTEGER PRIMARY KEY, "
                              "name VARCHAR(40) NOT NULL, "
                              "qty INTEGER NOT NULL)", NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE " VALUES "
                              "(1, 'bolt', 10), (2, 'nut', 20), "
                              "(3, 'washer', 30), (4, 'rivet', 40)",
                              &affected));
    if (affected != 4)
        TEST_VERDICT("An insert of 4 rows reported %lld",
                     (long long)affected);
    changed[0] = (double)affected;
    CHECK_RC(tapi_sql_assert_row_count(conn, TABLE, 4));

    TEST_STEP("Read them back");
    {
        tapi_sql_value cells[] = {
            tapi_sql_int(1), tapi_sql_text("bolt"), tapi_sql_int(10),
            tapi_sql_int(2), tapi_sql_text("nut"), tapi_sql_int(20),
            tapi_sql_int(3), tapi_sql_text("washer"), tapi_sql_int(30),
            tapi_sql_int(4), tapi_sql_text("rivet"), tapi_sql_int(40),
        };
        tapi_sql_result expected = { .ncols = 3, .nrows = 4,
                                     .cells = cells };

        CHECK_RC(tapi_sql_query(conn, "SELECT id, name, qty FROM " TABLE
                                " ORDER BY id", &got));
        tapi_sql_result_log(&got, "after the insert", 10);
        CHECK_RC(tapi_sql_assert_result(&got, &expected, NULL,
                                        "the inserted rows"));
    }

    TEST_STEP("Update some of them");
    CHECK_RC(tapi_sql_execute(conn, "UPDATE " TABLE
                              " SET qty = qty + 5 WHERE qty >= 30",
                              &affected));
    if (affected != 2)
        TEST_VERDICT("An update of 2 rows reported %lld",
                     (long long)affected);
    changed[1] = (double)affected;
    CHECK_RC(tapi_sql_assert_number(conn, "SELECT sum(qty) FROM " TABLE,
                                    110.0, 0.0, 0.0));

    TEST_STEP("An update that matches nothing changes nothing");
    CHECK_RC(tapi_sql_execute(conn, "UPDATE " TABLE
                              " SET qty = 0 WHERE id = 99", &affected));
    if (affected != 0)
        TEST_VERDICT("An update of no rows reported %lld",
                     (long long)affected);
    changed[2] = (double)affected;

    TEST_STEP("Delete one");
    CHECK_RC(tapi_sql_execute(conn, "DELETE FROM " TABLE " WHERE id = 2",
                              &affected));
    if (affected != 1)
        TEST_VERDICT("A delete of 1 row reported %lld", (long long)affected);
    changed[3] = (double)affected;
    CHECK_RC(tapi_sql_assert_row_count(conn, TABLE, 3));
    CHECK_RC(tapi_sql_assert_row_count(conn, TABLE " WHERE name = 'nut'",
                                       0));

    tsapi_sql_chart("Rows changed: insert, update, empty update, delete",
                    NULL, NULL, "rows", changed, 4);

    TEST_SUCCESS;

cleanup:
    tapi_sql_result_free(&got);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
