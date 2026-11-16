/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * The lost update: two sessions read the same balance, each adds its
 * own amount to what it read and writes the sum back. If both writes
 * are accepted, one addition is lost. Whether the engine lets that
 * happen depends on the isolation level:
 *
 *   SQLite                         prevented (the second writer is
 *                                  refused: one writer at a time)
 *   PostgreSQL read committed      lost
 *   PostgreSQL repeatable read,    prevented (the second writer gets
 *              serializable         a serialization failure)
 *   MariaDB read committed         lost
 *   MariaDB repeatable read        lost - unless the server runs with
 *                                  innodb_snapshot_isolation, which
 *                                  refuses the second writer
 *   MariaDB serializable           prevented (the reads lock, a writer
 *                                  gives up or deadlocks)
 *
 * The second write is started with tapi_sql_execute_start(), checked to
 * be waiting, released by the first session's commit and collected:
 * the synchronisation point of the scenario.
 */

#define TE_TEST_NAME    "sql/lost_update"

#include "te_config.h"

#include <string.h>

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define TABLE       "t_lu"
#define LOCK_MS     3000
#define READ        "SELECT v FROM " TABLE " WHERE id = 1"

/* Did the statement give way to the other session, rather than fail? */
static bool
gave_way(tapi_sql_conn *conn, te_errno rc)
{
    tapi_sql_err_class cls;

    if (rc == 0)
        return false;
    cls = tapi_sql_last_error(conn)->cls;
    if (cls != TAPI_SQL_ERR_LOCK_TIMEOUT && cls != TAPI_SQL_ERR_DEADLOCK &&
        cls != TAPI_SQL_ERR_SERIALIZATION)
        TEST_FAIL("A statement failed for an unexpected reason: %s (%s)",
                  tapi_sql_last_error(conn)->message,
                  tapi_sql_err_class2str(cls));
    RING("A statement gave way: %s (%s)",
         tapi_sql_last_error(conn)->message, tapi_sql_err_class2str(cls));
    return true;
}

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TABLE };

    tsapi_sql_sess     sess = { 0 };
    tapi_sql_conn     *a = NULL;
    tapi_sql_conn     *b = NULL;
    tapi_sql_conn     *probe = NULL;
    tapi_sql_caps      caps;
    tapi_sql_result    res = TAPI_SQL_RESULT_INIT;
    tapi_sql_isolation iso;
    const char        *backend;
    const char        *level;
    char               sql[128];
    int64_t            ra = 0;
    int64_t            rb = 0;
    int64_t            final = 0;
    bool               done = false;
    bool               a_open = false;
    bool               b_open = false;
    bool               lost;
    bool               exp_lost = false;
    double             points[2];

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    TEST_GET_STRING_PARAM(level);
    iso = tsapi_sql_level(level);

    TEST_STEP("Open the sessions; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_lu"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &a));
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &b));
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &probe));

    CHECK_RC(tapi_sql_capabilities(a, &caps));
    if ((caps.isolation & (unsigned int)iso) == 0)
        TEST_SKIP("The engine does not have this isolation level");

    /* What the engine is documented to do. */
    if (sess.engine == TAPI_SQL_ENGINE_POSTGRES)
    {
        exp_lost = iso <= TAPI_SQL_ISO_READ_COMMITTED;
    }
    else if (sess.engine == TAPI_SQL_ENGINE_MYSQL)
    {
        exp_lost = iso <= TAPI_SQL_ISO_READ_COMMITTED;
        if (iso == TAPI_SQL_ISO_REPEATABLE_READ)
        {
            /* Absent on servers that do not have the setting. */
            exp_lost = true;
            if (tapi_sql_query(probe, "SELECT @@innodb_snapshot_isolation",
                               &res) == 0 && res.nrows == 1 &&
                tapi_sql_result_cell(&res, 0, 0)->i != 0)
                exp_lost = false;
            RING("innodb_snapshot_isolation is %s",
                 exp_lost ? "off or absent" : "on");
        }
    }

    TEST_STEP("One row with a balance of 100");
    tsapi_sql_drop(a, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(a, "CREATE TABLE " TABLE
                              " (id INTEGER PRIMARY KEY, v INTEGER)", NULL));
    CHECK_RC(tapi_sql_execute(a, "INSERT INTO " TABLE " VALUES (1, 100)",
                              NULL));
    CHECK_RC(tapi_sql_set_timeout(a, TAPI_SQL_TIMEOUT_LOCK, LOCK_MS));
    CHECK_RC(tapi_sql_set_timeout(b, TAPI_SQL_TIMEOUT_LOCK, LOCK_MS));

    TEST_STEP("Both sessions read the balance");
    CHECK_RC(tapi_sql_begin_iso(a, iso));
    a_open = true;
    CHECK_RC(tapi_sql_begin_iso(b, iso));
    b_open = true;
    CHECK_RC(tapi_sql_query_int(a, READ, &ra));
    CHECK_RC(tapi_sql_query_int(b, READ, &rb));

    TEST_STEP("A writes its sum; B starts writing its own");
    snprintf(sql, sizeof(sql), "UPDATE " TABLE " SET v = %lld WHERE id = 1",
             (long long)(ra + 10));
    rc = tapi_sql_execute(a, sql, NULL);
    if (gave_way(a, rc))
    {
        /* A was refused: nothing can be lost any more. */
        CHECK_RC(tapi_sql_rollback(a));
        a_open = false;
        CHECK_RC(tapi_sql_rollback(b));
        b_open = false;
    }
    else
    {
        snprintf(sql, sizeof(sql),
                 "UPDATE " TABLE " SET v = %lld WHERE id = 1",
                 (long long)(rb + 20));
        CHECK_RC(tapi_sql_execute_start(b, sql));
        rc = tapi_sql_execute_wait(b, 700, &done, NULL);

        if (!done)
        {
            TEST_SUBSTEP("B waits for A's row lock; A commits and "
                         "releases it");
            CHECK_RC(tapi_sql_commit(a));
            a_open = false;
            rc = tapi_sql_execute_wait(b, 8000, &done, NULL);
            if (!done)
                TEST_VERDICT("The waiting writer never returned");
        }

        if (gave_way(b, rc))
        {
            CHECK_RC(tapi_sql_rollback(b));
            b_open = false;
        }
        if (a_open)
        {
            CHECK_RC(tapi_sql_commit(a));
            a_open = false;
        }
        if (b_open)
        {
            rc = tapi_sql_commit(b);
            b_open = false;
            if (rc != 0)
                gave_way(b, rc);
        }
    }

    TEST_STEP("See which additions survived");
    CHECK_RC(tapi_sql_query_int(probe, READ, &final));
    RING("Read 100 twice, wrote 110 and 120: the balance is %lld",
         (long long)final);
    if (final != 100 && final != 110 && final != 120)
        TEST_VERDICT("The balance is none of the values written");
    lost = final == 120;

    points[0] = (double)final;
    points[1] = lost ? 1.0 : 0.0;
    tsapi_sql_chart("Final balance, and whether an update was lost", NULL,
                    NULL, "value", points, 2);

    if (lost != exp_lost)
        TEST_VERDICT("The update was %s at this level",
                     lost ? "lost" : "not lost");

    TEST_SUCCESS;

cleanup:
    tapi_sql_result_free(&res);
    if (a != NULL)
        tapi_sql_rollback(a);
    tapi_sql_disconnect(b);
    tapi_sql_disconnect(probe);
    tsapi_sql_drop(a, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(a);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
