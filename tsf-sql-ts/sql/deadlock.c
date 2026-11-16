/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Deadlock detection: two sessions each lock a row and then ask for
 * the other's. The engine must notice, fail exactly one of them with a
 * deadlock error, and let the other finish; the survivor's changes are
 * what the table ends up with.
 */

#define TE_TEST_NAME    "sql/deadlock"

#include "te_config.h"

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define TABLE   "t_dl"

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TABLE };

    tsapi_sql_sess sess = { 0 };
    tapi_sql_conn *a = NULL;
    tapi_sql_conn *b = NULL;
    const char    *backend;
    te_errno       rc_a;
    te_errno       rc_b;
    bool           done = false;
    bool           a_victim;
    bool           b_victim;
    double         detect_ms;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open two sessions; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_dl"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &a));
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &b));
    if (!tapi_sql_has_cap(a, TAPI_SQL_CAP_DEADLOCK))
        TEST_SKIP("The engine locks the whole database: no deadlocks");

    TEST_STEP("Two rows");
    tsapi_sql_drop(a, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(a, "CREATE TABLE " TABLE
                              " (id INTEGER PRIMARY KEY, who VARCHAR(4))",
                              NULL));
    CHECK_RC(tapi_sql_execute(a, "INSERT INTO " TABLE
                              " VALUES (1, '-'), (2, '-')", NULL));

    TEST_STEP("A locks row 1, B locks row 2");
    CHECK_RC(tapi_sql_begin(a));
    CHECK_RC(tapi_sql_begin(b));
    CHECK_RC(tapi_sql_execute(a, "UPDATE " TABLE
                              " SET who = 'a' WHERE id = 1", NULL));
    CHECK_RC(tapi_sql_execute(b, "UPDATE " TABLE
                              " SET who = 'b' WHERE id = 2", NULL));

    TEST_STEP("B asks for row 1 and waits");
    CHECK_RC(tapi_sql_execute_start(b, "UPDATE " TABLE
                                    " SET who = 'b' WHERE id = 1"));
    rc_b = tapi_sql_execute_wait(b, 500, &done, NULL);
    if (done)
        TEST_VERDICT("A writer did not wait for a locked row");

    TEST_STEP("A asks for row 2: a deadlock");
    rc_a = tapi_sql_execute(a, "UPDATE " TABLE " SET who = 'a' WHERE id = 2",
                            NULL);
    a_victim = rc_a != 0 &&
               tapi_sql_last_error(a)->cls == TAPI_SQL_ERR_DEADLOCK;
    if (rc_a != 0 && !a_victim)
        TEST_VERDICT("The deadlock was reported to A as '%s'",
                     tapi_sql_err_class2str(tapi_sql_last_error(a)->cls));
    detect_ms = (double)tapi_sql_last_elapsed_us(a) / 1000.0;

    /* Whoever lost has to let go before the other can go on. */
    if (a_victim)
        CHECK_RC(tapi_sql_rollback(a));

    rc_b = tapi_sql_execute_wait(b, 8000, &done, NULL);
    if (!done)
        TEST_VERDICT("The deadlock was not broken: a session still waits");
    b_victim = rc_b != 0 &&
               tapi_sql_last_error(b)->cls == TAPI_SQL_ERR_DEADLOCK;
    if (rc_b != 0 && !b_victim)
        TEST_VERDICT("The deadlock was reported to B as '%s'",
                     tapi_sql_err_class2str(tapi_sql_last_error(b)->cls));

    TEST_STEP("Exactly one session was the victim");
    RING("A: %s, B: %s; A's statement returned after %.0f ms",
         a_victim ? "victim" : "survivor", b_victim ? "victim" : "survivor",
         detect_ms);
    if (a_victim == b_victim)
        TEST_VERDICT("%s session was failed for the deadlock",
                     a_victim ? "Each" : "Neither");

    TEST_STEP("The survivor commits and its rows are what remains");
    if (b_victim)
    {
        CHECK_RC(tapi_sql_rollback(b));
        CHECK_RC(tapi_sql_commit(a));
    }
    else
    {
        CHECK_RC(tapi_sql_commit(b));
    }
    CHECK_RC(tapi_sql_assert_row_count(
                 a, a_victim ? TABLE " WHERE who = 'b'" :
                               TABLE " WHERE who = 'a'", 2));

    tsapi_sql_chart("Time for the deadlock to be detected, ms", NULL, NULL,
                    "ms", &detect_ms, 1);

    TEST_SUCCESS;

cleanup:
    if (a != NULL)
        tapi_sql_rollback(a);
    tapi_sql_disconnect(b);
    tsapi_sql_drop(a, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(a);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
