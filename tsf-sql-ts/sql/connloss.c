/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Connection loss: the server ends a session under a client that still
 * holds it (an administrator kills it, as a restart or a failover
 * would). The next statement must say "the connection is gone" - not
 * hang, not report a syntax error - a ping must agree, a reconnection
 * must bring a working session back, and what the dead session held on
 * the agent must be released.
 */

#define TE_TEST_NAME    "sql/connloss"

#include "te_config.h"

#include "tapi_test.h"
#include "te_sleep.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define TABLE   "t_loss"

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TABLE };

    tsapi_sql_sess sess = { 0 };
    tapi_sql_conn *victim = NULL;
    tapi_sql_conn *killer = NULL;
    tapi_sql_stmt *stmt = NULL;
    const char    *backend;
    int64_t        id = 0;
    unsigned int   conns = 0;
    unsigned int   stmts = 0;
    double         steps[3];

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open two sessions; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_loss"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &victim));
    if (!tapi_sql_has_cap(victim, TAPI_SQL_CAP_NETWORK))
        TEST_SKIP("The engine has no server to lose a connection to");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_ADMIN, &killer));

    TEST_STEP("The session holds a table, an open transaction and a "
              "prepared statement");
    tsapi_sql_drop(victim, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(victim, "CREATE TABLE " TABLE
                              " (id INTEGER PRIMARY KEY)", NULL));
    CHECK_RC(tapi_sql_execute(victim, "INSERT INTO " TABLE " VALUES (1)",
                              NULL));
    CHECK_RC(tapi_sql_prepare(victim, "SELECT id FROM " TABLE, &stmt));
    CHECK_RC(tapi_sql_begin(victim));
    CHECK_RC(tapi_sql_execute(victim, "INSERT INTO " TABLE " VALUES (2)",
                              NULL));

    TEST_STEP("The administrator ends the session on the server");
    CHECK_RC(tapi_sql_query_int(victim,
                                sess.engine == TAPI_SQL_ENGINE_POSTGRES ?
                                "SELECT pg_backend_pid()" :
                                "SELECT CONNECTION_ID()", &id));
    if (sess.engine == TAPI_SQL_ENGINE_POSTGRES)
        CHECK_RC(tapi_sql_execute_fmt(killer, NULL,
                                      "SELECT pg_terminate_backend(%lld)",
                                      (long long)id));
    else
        CHECK_RC(tapi_sql_execute_fmt(killer, NULL, "KILL %lld",
                                      (long long)id));
    te_motivated_msleep(300, "let the server end the session");

    TEST_STEP("The next statement reports the loss");
    rc = tapi_sql_execute(victim, "INSERT INTO " TABLE " VALUES (3)", NULL);
    RING("After the kill: rc=%r class=%s: %s", rc,
         tapi_sql_err_class2str(tapi_sql_last_error(victim)->cls),
         tapi_sql_last_error(victim)->message);
    if (rc == 0)
        TEST_VERDICT("A statement succeeded on a killed session");
    if (tapi_sql_last_error(victim)->cls != TAPI_SQL_ERR_CONN_LOST)
        TEST_VERDICT("The lost connection was reported as '%s'",
                     tapi_sql_err_class2str(
                         tapi_sql_last_error(victim)->cls));
    steps[0] = 1.0;

    TEST_STEP("A ping agrees");
    if (tapi_sql_ping(victim) == 0)
        TEST_VERDICT("A ping succeeded on a killed session");
    steps[1] = 1.0;

    TEST_STEP("A reconnection brings a working session back");
    CHECK_RC(tapi_sql_statement_free(stmt));
    stmt = NULL;
    CHECK_RC(tapi_sql_reconnect(victim));
    CHECK_RC(tapi_sql_ping(victim));
    steps[2] = 1.0;

    TEST_STEP("The transaction the dead session had open left nothing");
    CHECK_RC(tapi_sql_assert_row_count(victim, TABLE, 1));

    TEST_STEP("The agent holds the two live sessions and no statement");
    CHECK_RC(tapi_sql_stats(sess.pco, &conns, &stmts, NULL));
    if (conns != 2 || stmts != 0)
        TEST_VERDICT("After the loss the agent holds %u connection(s) and "
                     "%u statement(s)", conns, stmts);

    tsapi_sql_chart("Loss reported by: statement, ping; then reconnected",
                    NULL, NULL, "ok", steps, 3);

    TEST_SUCCESS;

cleanup:
    tapi_sql_statement_free(stmt);
    tapi_sql_disconnect(killer);
    tsapi_sql_drop(victim, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(victim);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
