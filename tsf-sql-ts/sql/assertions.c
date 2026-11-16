/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * The database assertions on a database in good order: every property
 * holds and every assertion says so. Its counterpart, assert_violation,
 * breaks each property in turn.
 */

#define TE_TEST_NAME    "sql/assertions"

#include "te_config.h"

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define PARENT  "t_as_order"
#define CHILD   "t_as_line"

int
main(int argc, char **argv)
{
    static const char *const tables[] = { PARENT, CHILD };
    static const tapi_sql_column_spec spec[] = {
        { "id", TAPI_SQL_INT, TAPI_SQL_NO, TAPI_SQL_YES },
        { "order_id", TAPI_SQL_INT, TAPI_SQL_NO, TAPI_SQL_NO },
        { "price", TAPI_SQL_NULL, TAPI_SQL_ANY, TAPI_SQL_NO },
    };
    static const char *const failing[] = {
        "INSERT INTO " CHILD " VALUES (10, 1, 1.0)",
        "INSERT INTO " CHILD " VALUES (11, 999, 1.0)",
    };

    tsapi_sql_sess    sess = { 0 };
    tapi_sql_conn    *conn = NULL;
    tapi_sql_cmp_opts bag = TAPI_SQL_CMP_OPTS_DEFAULT;
    const char       *backend;
    double            held[9];
    unsigned int      n = 0;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    bag.ordered = false;

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_assert"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("Orders and their lines, consistent");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " PARENT " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "customer VARCHAR(20) NOT NULL)", NULL));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " CHILD " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "order_id INTEGER NOT NULL, "
                              "price DOUBLE PRECISION, "
                              "FOREIGN KEY (order_id) REFERENCES " PARENT
                              " (id))", NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " PARENT " VALUES "
                              "(1, 'ann'), (2, 'bob')", NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " CHILD " VALUES "
                              "(1, 1, 0.1), (2, 1, 0.2), (3, 2, 9.7)",
                              NULL));

    TEST_STEP("Row count");
    CHECK_RC(tapi_sql_assert_row_count(conn, CHILD, 3));
    CHECK_RC(tapi_sql_assert_row_count(conn, CHILD " WHERE order_id = 1",
                                       2));
    held[n++] = 1;

    TEST_STEP("Primary key uniqueness");
    CHECK_RC(tapi_sql_assert_pk_unique(conn, CHILD, "id"));
    CHECK_RC(tapi_sql_assert_pk_unique(conn, CHILD, "order_id, id"));
    held[n++] = 1;

    TEST_STEP("Referential integrity");
    CHECK_RC(tapi_sql_assert_referential(conn, CHILD, "order_id", PARENT,
                                         "id"));
    held[n++] = 1;

    TEST_STEP("NULL constraints");
    CHECK_RC(tapi_sql_assert_not_null(conn, CHILD, "id, order_id, price"));
    held[n++] = 1;

    TEST_STEP("Schema compatibility");
    CHECK_RC(tapi_sql_assert_schema(conn, CHILD, spec, TE_ARRAY_LEN(spec),
                                    true));
    held[n++] = 1;

    TEST_STEP("Query result equivalence, rows in any order");
    CHECK_RC(tapi_sql_assert_query_equal(
                 conn, "SELECT order_id, count(*) FROM " CHILD
                       " GROUP BY order_id",
                 conn, "SELECT o.id, (SELECT count(*) FROM " CHILD
                       " l WHERE l.order_id = o.id) FROM " PARENT " o",
                 &bag));
    held[n++] = 1;

    TEST_STEP("A number within a tolerance: 0.1 + 0.2 is 0.3, nearly");
    CHECK_RC(tapi_sql_assert_number(conn, "SELECT sum(price) FROM " CHILD
                                    " WHERE order_id = 1", 0.3, 1e-9, 0.0));
    CHECK_RC(tapi_sql_assert_number(conn, "SELECT sum(price) FROM " CHILD,
                                    10.0, 0.0, 1e-9));
    held[n++] = 1;

    TEST_STEP("Transaction atomicity");
    CHECK_RC(tapi_sql_assert_atomic(conn, failing, TE_ARRAY_LEN(failing),
                                    true, "SELECT id, order_id FROM " CHILD
                                    " ORDER BY id"));
    held[n++] = 1;

    TEST_STEP("Persistent state after reconnection");
    CHECK_RC(tapi_sql_assert_persistent(conn, "SELECT id, order_id, price "
                                        "FROM " CHILD " ORDER BY id"));
    held[n++] = 1;

    tsapi_sql_chart("Assertions that held", NULL, NULL, "held", held, n);

    TEST_SUCCESS;

cleanup:
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
