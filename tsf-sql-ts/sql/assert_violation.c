/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * The database assertions on a database that is deliberately wrong: one
 * property is broken per iteration and the assertion of that property
 * must fail, with its verdict. These iterations are expected to FAIL -
 * conf/trc.xml carries the verdict each one must produce - which is
 * what shows that an assertion is able to say "no". An assertion that
 * lets the violation through produces a different verdict and the run
 * reports it as unexpected.
 */

#define TE_TEST_NAME    "sql/assert_violation"

#include "te_config.h"

#include <string.h>

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define PARENT  "t_av"
#define CHILD   "t_av_child"

int
main(int argc, char **argv)
{
    static const char *const tables[] = { PARENT, CHILD };
    static const tapi_sql_column_spec spec[] = {
        { "id", TAPI_SQL_TEXT, TAPI_SQL_ANY, TAPI_SQL_ANY },
        { "missing", TAPI_SQL_INT, TAPI_SQL_ANY, TAPI_SQL_ANY },
    };
    static const char *const harmless[] = {
        "UPDATE " PARENT " SET v = v WHERE id = 2",
    };

    tsapi_sql_sess sess = { 0 };
    tapi_sql_conn *conn = NULL;
    const char    *backend;
    const char    *property;
    double         reported = 0.0;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    TEST_GET_STRING_PARAM(property);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_av"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("Tables with no constraints, holding what constraints "
              "would have refused");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " PARENT " (id INTEGER, "
                              "name VARCHAR(20), v DOUBLE PRECISION)",
                              NULL));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " CHILD " (id INTEGER, "
                              "parent_id INTEGER)", NULL));
    /* A duplicate key, a NULL name. */
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " PARENT " VALUES "
                              "(1, 'a', 1.0), (1, 'dup', 2.0), "
                              "(2, NULL, 3.0)", NULL));
    /* A reference to a row that does not exist. */
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " CHILD " VALUES "
                              "(1, 1), (2, 77)", NULL));

    TEST_STEP("Assert the broken property");
    if (strcmp(property, "row_count") == 0)
        rc = tapi_sql_assert_row_count(conn, PARENT, 5);
    else if (strcmp(property, "pk_unique") == 0)
        rc = tapi_sql_assert_pk_unique(conn, PARENT, "id");
    else if (strcmp(property, "referential") == 0)
        rc = tapi_sql_assert_referential(conn, CHILD, "parent_id", PARENT,
                                         "id");
    else if (strcmp(property, "not_null") == 0)
        rc = tapi_sql_assert_not_null(conn, PARENT, "name");
    else if (strcmp(property, "schema") == 0)
        rc = tapi_sql_assert_schema(conn, PARENT, spec, TE_ARRAY_LEN(spec),
                                    false);
    else if (strcmp(property, "equal") == 0)
        rc = tapi_sql_assert_query_equal(
                 conn, "SELECT id FROM " PARENT " ORDER BY id, name",
                 conn, "SELECT id FROM " CHILD " ORDER BY id", NULL);
    else if (strcmp(property, "number") == 0)
        rc = tapi_sql_assert_number(conn, "SELECT sum(v) FROM " PARENT, 7.0,
                                    0.5, 0.0);
    else if (strcmp(property, "atomic") == 0)
        rc = tapi_sql_assert_atomic(conn, harmless, TE_ARRAY_LEN(harmless),
                                    true, "SELECT id, v FROM " PARENT
                                    " ORDER BY id, v");
    else
        TEST_FAIL("Unknown property '%s'", property);

    if (rc == 0)
        TEST_VERDICT("The violation went unnoticed");
    if (TE_RC_GET_ERROR(rc) != TE_EFAIL)
        TEST_VERDICT("The assertion could not be evaluated");

    reported = 1.0;
    tsapi_sql_chart("Violations reported", NULL, NULL, "reported",
                    &reported, 1);

    /* The assertion has emitted its verdict; the test fails with it. */
    TEST_STOP;

cleanup:
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
