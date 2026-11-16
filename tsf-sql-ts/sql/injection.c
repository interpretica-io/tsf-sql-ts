/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * SQL injection, tested where it lives: in an application's input. The
 * "application" here is a login lookup written twice. One version
 * pastes the user's input into the statement text - the known-bad
 * control; the other binds it as a parameter. Each is handed the same
 * hostile inputs. The pasted version must fall for some of them (an
 * audit that cannot break it would prove nothing about the other); the
 * parameterised version must fall for none, and must still find the
 * user whose name is itself an injection string.
 *
 * Nothing here assumes a driver is vulnerable: the database only ever
 * does what the statement it is given says.
 */

#define TE_TEST_NAME    "sql/injection"

#include "te_config.h"

#include <string.h>

#include "tapi_test.h"
#include "te_string.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tapi_sql_audit.h"
#include "tsapi_sql.h"

#define TABLE   "t_inj_users"

/** The application under test: a connection and how it builds SQL. */
typedef struct app {
    tapi_sql_conn *conn;
    bool           pasting;
} app;

/*
 * The application's login lookup: how many users have this name? A
 * careful application answers 0 or 1 and never fails on its input.
 * Returns the count, or -1 when the database rejected the statement.
 */
static int
app_lookup(app *a, const char *name)
{
    tapi_sql_result res = TAPI_SQL_RESULT_INIT;
    te_string       sql = TE_STRING_INIT;
    tapi_sql_value  param = tapi_sql_text(name);
    te_errno        rc;
    int             n;

    if (a->pasting)
    {
        te_string_append(&sql, "SELECT name FROM " TABLE
                         " WHERE name = '%s'", name);
        rc = tapi_sql_query(a->conn, sql.ptr, &res);
    }
    else
    {
        rc = tapi_sql_query_params(a->conn, "SELECT name FROM " TABLE
                                   " WHERE name = ?", &param, 1, &res, NULL);
    }
    n = rc == 0 ? (int)res.nrows : -1;
    te_string_free(&sql);
    tapi_sql_result_free(&res);
    return n;
}

/*
 * Did the application misbehave on this input? None of the inputs is a
 * user's name, so any row returned was injected, and a statement the
 * database rejects means the input reached its parser.
 */
static bool
probe(const char *input, void *opaque)
{
    return app_lookup(opaque, input) != 0;
}

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TABLE };

    tsapi_sql_sess  sess = { 0 };
    tapi_sql_conn  *conn = NULL;
    tapi_sql_report bad = TAPI_SQL_REPORT_INIT;
    tapi_sql_report good = TAPI_SQL_REPORT_INIT;
    tapi_sql_value  odd = tapi_sql_text("' OR '1'='1");
    app             pasted;
    app             bound;
    const char     *backend;
    unsigned int    npayloads = 0;
    double          hits[2];

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_inj"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("The application's users");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " TABLE
                              " (id INTEGER PRIMARY KEY, name VARCHAR(40))",
                              NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE
                              " VALUES (1, 'alice'), (2, 'bob')", NULL));
    pasted.conn = conn;
    pasted.pasting = true;
    bound.conn = conn;
    bound.pasting = false;
    tapi_sql_injection_payloads(&npayloads);

    TEST_STEP("Both versions find a real user and miss an unknown one");
    if (app_lookup(&pasted, "alice") != 1 || app_lookup(&bound, "alice") != 1)
        TEST_VERDICT("The application cannot find a real user");
    if (app_lookup(&pasted, "mallory") != 0 ||
        app_lookup(&bound, "mallory") != 0)
        TEST_VERDICT("The application finds a user that does not exist");

    TEST_STEP("The control: the version that pastes its input is broken "
              "by it");
    hits[0] = (double)tapi_sql_audit_injection("login name (pasted)", probe,
                                               &pasted, &bad);
    tapi_sql_report_log(&bad);
    if (!tapi_sql_report_has(&bad, "sql.injection"))
        TEST_VERDICT("None of the hostile inputs broke a statement built "
                     "by pasting");

    TEST_STEP("The version that binds its input falls for none");
    hits[1] = (double)tapi_sql_audit_injection("login name (bound)", probe,
                                               &bound, &good);
    tapi_sql_report_log(&good);
    if (good.n != 0)
        TEST_VERDICT("A hostile input changed the meaning of a "
                     "parameterised statement");

    TEST_STEP("A name that looks like an attack is just a name");
    CHECK_RC(tapi_sql_query_params(conn, "INSERT INTO " TABLE
                                   " VALUES (3, ?)", &odd, 1, NULL, NULL));
    if (app_lookup(&bound, "' OR '1'='1") != 1)
        TEST_VERDICT("A bound value with quotes in it was not matched as "
                     "data");
    CHECK_RC(tapi_sql_assert_row_count(conn, TABLE, 3));

    RING("%u hostile inputs: %.0f broke the pasted statement, %.0f the "
         "bound one", npayloads, hits[0], hits[1]);
    tsapi_sql_chart("Hostile inputs that worked: pasted, bound", NULL, NULL,
                    "inputs", hits, 2);

    TEST_SUCCESS;

cleanup:
    tapi_sql_report_free(&bad);
    tapi_sql_report_free(&good);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
