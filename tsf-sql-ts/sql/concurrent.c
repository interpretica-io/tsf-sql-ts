/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Concurrent inserts and updates: several sessions write to the same
 * table and bump the same counter at the same time, round after round.
 * Every statement is started on all sessions before any is collected.
 * Nothing may be lost and nothing doubled: the row count, the counter
 * and the key all have to add up.
 */

#define TE_TEST_NAME    "sql/concurrent"

#include "te_config.h"

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define ROWS        "t_cc_rows"
#define COUNTER     "t_cc_counter"
#define SESSIONS    4
#define ROUNDS      5
#define WAIT_MS     8000

int
main(int argc, char **argv)
{
    static const char *const tables[] = { ROWS, COUNTER };

    tsapi_sql_sess sess = { 0 };
    tapi_sql_conn *conn[SESSIONS] = { NULL };
    const char    *backend;
    char           sql[160];
    double         busy_ms[SESSIONS] = { 0.0 };
    bool           done = false;
    int            round;
    int            s;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open the sessions; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_cc"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    for (s = 0; s < SESSIONS; s++)
    {
        CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP,
                                        &conn[s]));
        /* Writers queue for each other instead of giving up. */
        CHECK_RC(tapi_sql_set_timeout(conn[s], TAPI_SQL_TIMEOUT_LOCK,
                                      WAIT_MS));
    }

    TEST_STEP("A table to insert into and a counter to bump");
    tsapi_sql_drop(conn[0], tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn[0], "CREATE TABLE " ROWS " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "writer INTEGER NOT NULL)", NULL));
    CHECK_RC(tapi_sql_execute(conn[0], "CREATE TABLE " COUNTER " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "n INTEGER NOT NULL)", NULL));
    CHECK_RC(tapi_sql_execute(conn[0], "INSERT INTO " COUNTER
                              " VALUES (1, 0)", NULL));

    TEST_STEP("Every session inserts and bumps, all at once, in rounds");
    for (round = 0; round < ROUNDS; round++)
    {
        for (s = 0; s < SESSIONS; s++)
        {
            snprintf(sql, sizeof(sql), "INSERT INTO " ROWS " VALUES (%d, %d)",
                     s * 1000 + round, s);
            CHECK_RC(tapi_sql_execute_start(conn[s], sql));
        }
        for (s = 0; s < SESSIONS; s++)
        {
            rc = tapi_sql_execute_wait(conn[s], WAIT_MS, &done, NULL);
            if (!done || rc != 0)
                TEST_VERDICT("A concurrent insert failed: %s",
                             done ? tapi_sql_err_class2str(
                                        tapi_sql_last_error(conn[s])->cls) :
                                    "it never returned");
            busy_ms[s] += (double)tapi_sql_last_elapsed_us(conn[s]) / 1000.0;
        }

        for (s = 0; s < SESSIONS; s++)
            CHECK_RC(tapi_sql_execute_start(conn[s], "UPDATE " COUNTER
                                            " SET n = n + 1 WHERE id = 1"));
        for (s = 0; s < SESSIONS; s++)
        {
            rc = tapi_sql_execute_wait(conn[s], WAIT_MS, &done, NULL);
            if (!done || rc != 0)
                TEST_VERDICT("A concurrent update failed: %s",
                             done ? tapi_sql_err_class2str(
                                        tapi_sql_last_error(conn[s])->cls) :
                                    "it never returned");
            busy_ms[s] += (double)tapi_sql_last_elapsed_us(conn[s]) / 1000.0;
        }
    }

    TEST_STEP("Everything adds up");
    CHECK_RC(tapi_sql_assert_row_count(conn[0], ROWS, SESSIONS * ROUNDS));
    CHECK_RC(tapi_sql_assert_pk_unique(conn[0], ROWS, "id"));
    CHECK_RC(tapi_sql_assert_number(conn[0], "SELECT n FROM " COUNTER,
                                    SESSIONS * ROUNDS, 0.0, 0.0));
    for (s = 0; s < SESSIONS; s++)
    {
        snprintf(sql, sizeof(sql), ROWS " WHERE writer = %d", s);
        CHECK_RC(tapi_sql_assert_row_count(conn[0], sql, ROUNDS));
    }

    tsapi_sql_chart("Time each session spent in its statements, ms", NULL,
                    NULL, "ms", busy_ms, SESSIONS);

    TEST_SUCCESS;

cleanup:
    for (s = SESSIONS - 1; s > 0; s--)
        tapi_sql_disconnect(conn[s]);
    tsapi_sql_drop(conn[0], tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn[0]);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
