/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Deadlines, cancellation and what is left afterwards. A statement
 * that runs past its deadline is stopped by the engine and reported as
 * a timeout; a statement cancelled from outside is reported as
 * cancelled; a writer that waits for a lock gives up at the lock
 * deadline. After each of these the connection still works, and when
 * everything is closed the agent holds nothing - not the statements
 * that failed, not the results that were never read to the end.
 */

#define TE_TEST_NAME    "sql/timeouts"

#include "te_config.h"

#include "tapi_test.h"
#include "te_string.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define DIGITS      "t_to_digits"
#define TABLE       "t_to"
#define DEADLINE_MS 400
#define LOCK_MS     1000

int
main(int argc, char **argv)
{
    static const char *const tables[] = { DIGITS, TABLE };

    tsapi_sql_sess  sess = { 0 };
    tapi_sql_conn  *conn = NULL;
    tapi_sql_conn  *other = NULL;
    tapi_sql_stmt  *stmts[4] = { NULL };
    tapi_sql_result res = TAPI_SQL_RESULT_INIT;
    te_string       slow = TE_STRING_INIT;
    const char     *backend;
    unsigned int    nconns = 0;
    unsigned int    nstmts = 0;
    double          took_ms[3] = { 0.0 };
    bool            done = false;
    bool            eof = false;
    unsigned int    i;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open two sessions; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_to"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &other));

    TEST_STEP("A query slow enough to outlive any deadline here");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tsapi_sql_digits(conn, DIGITS));
    tsapi_sql_slow_query(DIGITS, &slow);
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " TABLE
                              " (id INTEGER PRIMARY KEY, v INTEGER)", NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE " VALUES (1, 0)",
                              NULL));

    TEST_STEP("A statement deadline stops it, as a timeout");
    CHECK_RC(tapi_sql_set_timeout(conn, TAPI_SQL_TIMEOUT_STATEMENT,
                                  DEADLINE_MS));
    rc = tapi_sql_execute(conn, slow.ptr, NULL);
    took_ms[0] = (double)tapi_sql_last_elapsed_us(conn) / 1000.0;
    RING("Deadline %d ms: rc=%r class=%s after %.0f ms: %s", DEADLINE_MS, rc,
         tapi_sql_err_class2str(tapi_sql_last_error(conn)->cls), took_ms[0],
         tapi_sql_last_error(conn)->message);
    if (TE_RC_GET_ERROR(rc) != TE_ETIMEDOUT ||
        tapi_sql_last_error(conn)->cls != TAPI_SQL_ERR_TIMEOUT)
        TEST_VERDICT("A statement past its deadline was reported as '%s'",
                     rc == 0 ? "success" : tapi_sql_err_class2str(
                                        tapi_sql_last_error(conn)->cls));
    if (took_ms[0] > 5000.0)
        TEST_VERDICT("The deadline was enforced far too late");
    CHECK_RC(tapi_sql_set_timeout(conn, TAPI_SQL_TIMEOUT_STATEMENT, 0));
    CHECK_RC(tapi_sql_ping(conn));

    TEST_STEP("A running statement is cancelled from outside");
    CHECK_RC(tapi_sql_execute_start(conn, slow.ptr));
    rc = tapi_sql_execute_wait(conn, 300, &done, NULL);
    if (done)
        TEST_VERDICT("The slow query finished before it could be "
                     "cancelled");
    if (TE_RC_GET_ERROR(tapi_sql_execute(conn, "SELECT 1", NULL)) !=
        TE_EBUSY)
        TEST_VERDICT("A connection that is running a statement took "
                     "another one");
    CHECK_RC(tapi_sql_cancel(conn));
    rc = tapi_sql_execute_wait(conn, 8000, &done, NULL);
    took_ms[1] = (double)tapi_sql_last_elapsed_us(conn) / 1000.0;
    if (!done)
        TEST_VERDICT("A cancelled statement never returned");
    if (TE_RC_GET_ERROR(rc) != TE_ECANCELED ||
        tapi_sql_last_error(conn)->cls != TAPI_SQL_ERR_CANCELLED)
        TEST_VERDICT("A cancelled statement was reported as '%s'",
                     rc == 0 ? "success" : tapi_sql_err_class2str(
                                        tapi_sql_last_error(conn)->cls));
    CHECK_RC(tapi_sql_ping(conn));

    TEST_STEP("A writer gives up at the lock deadline");
    CHECK_RC(tapi_sql_set_timeout(other, TAPI_SQL_TIMEOUT_LOCK, LOCK_MS));
    CHECK_RC(tapi_sql_begin(conn));
    CHECK_RC(tapi_sql_execute(conn, "UPDATE " TABLE " SET v = 1 WHERE id = 1",
                              NULL));
    rc = tapi_sql_execute(other, "UPDATE " TABLE " SET v = 2 WHERE id = 1",
                          NULL);
    took_ms[2] = (double)tapi_sql_last_elapsed_us(other) / 1000.0;
    if (TE_RC_GET_ERROR(rc) != TE_ETIMEDOUT ||
        tapi_sql_last_error(other)->cls != TAPI_SQL_ERR_LOCK_TIMEOUT)
        TEST_VERDICT("A writer blocked by a lock was reported as '%s'",
                     rc == 0 ? "success" : tapi_sql_err_class2str(
                                        tapi_sql_last_error(other)->cls));
    CHECK_RC(tapi_sql_commit(conn));
    CHECK_RC(tapi_sql_assert_number(other, "SELECT v FROM " TABLE, 1.0, 0.0,
                                    0.0));

    TEST_STEP("Statements left in every state: unexecuted, failed, half "
              "read");
    CHECK_RC(tapi_sql_prepare(conn, "SELECT d FROM " DIGITS, &stmts[0]));
    CHECK_RC(tapi_sql_prepare(conn, "INSERT INTO " TABLE " VALUES (?, 0)",
                              &stmts[1]));
    CHECK_RC(tapi_sql_bind(stmts[1], 1, tapi_sql_int(1)));
    if (tapi_sql_stmt_execute(stmts[1], NULL) == 0)
        TEST_VERDICT("A duplicate key was accepted");
    CHECK_RC(tapi_sql_prepare(conn, "SELECT a.d, b.d FROM " DIGITS " a, "
                              DIGITS " b", &stmts[2]));
    CHECK_RC(tapi_sql_stmt_execute(stmts[2], NULL));
    CHECK_RC(tapi_sql_fetch(stmts[2], 7, &res, &eof));
    if (res.nrows != 7 || eof)
        TEST_VERDICT("A fetch of 7 rows out of 100 returned %u%s", res.nrows,
                     eof ? " and the end" : "");
    CHECK_RC(tapi_sql_prepare(other, "SELECT 1", &stmts[3]));

    CHECK_RC(tapi_sql_stats(sess.pco, &nconns, &nstmts, NULL));
    if (nconns != 2 || nstmts != 4)
        TEST_VERDICT("The agent counts %u connection(s) and %u "
                     "statement(s), not 2 and 4", nconns, nstmts);

    TEST_STEP("The half-read result does not block the connection");
    CHECK_RC(tapi_sql_execute(conn, "UPDATE " TABLE " SET v = 5 WHERE id = 1",
                              NULL));

    TEST_STEP("Closing the connections releases every one of them");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_disconnect(other));
    other = NULL;
    CHECK_RC(tapi_sql_disconnect(conn));
    conn = NULL;
    CHECK_RC(tapi_sql_stats(sess.pco, &nconns, &nstmts, NULL));
    if (nconns != 0 || nstmts != 0)
        TEST_VERDICT("After closing everything the agent still holds %u "
                     "connection(s) and %u statement(s)", nconns, nstmts);
    for (i = 0; i < TE_ARRAY_LEN(stmts); i++)
    {
        CHECK_RC(tapi_sql_statement_free(stmts[i]));
        stmts[i] = NULL;
    }

    tsapi_sql_chart("Time to: statement deadline, cancel, lock deadline "
                    "(ms)", NULL, NULL, "ms", took_ms, 3);

    TEST_SUCCESS;

cleanup:
    for (i = 0; i < TE_ARRAY_LEN(stmts); i++)
        tapi_sql_statement_free(stmts[i]);
    tapi_sql_result_free(&res);
    te_string_free(&slow);
    if (conn != NULL)
        tapi_sql_rollback(conn);
    tapi_sql_disconnect(other);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
