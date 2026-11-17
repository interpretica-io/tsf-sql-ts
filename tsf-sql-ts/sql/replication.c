/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * A primary and its replica. What is committed on the primary arrives
 * on the replica - schema and rows, within a deadline, and the same to
 * the last value; what is rolled back or still uncommitted never does.
 * A transaction arrives whole: a transfer between two accounts is
 * never seen half done on the replica. The replica takes reads only: a
 * write there is refused as "read only", and changes nothing on either
 * side.
 *
 * "Arrives" is measured: every row's way to the replica is timed and
 * the times are charted. Replication is asynchronous on both engines
 * as the suite sets them up, so the test waits for state on the replica
 * (tapi_sql_assert_eventually()) rather than expecting it at once.
 */

#define TE_TEST_NAME    "sql/replication"

#include "te_config.h"

#include <string.h>
#include <strings.h>

#include "tapi_test.h"
#include "te_sleep.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tapi_sql_audit.h"
#include "tsapi_sql.h"

#define ROWS        "t_repl_rows"
#define ACCOUNTS    "t_repl_accounts"
#define ARRIVE_MS   10000
#define ROUNDS      10
#define TRANSFERS   20
#define TOTAL       "SELECT sum(balance) FROM " ACCOUNTS

/* The value of a named column of a one-row result, or "". */
static const char *
column(const tapi_sql_result *res, const char *name)
{
    unsigned int i;

    for (i = 0; i < res->ncols && res->nrows > 0; i++)
    {
        const tapi_sql_value *v = tapi_sql_result_cell(res, 0, i);

        if (strcasecmp(res->names[i], name) == 0 && v->str != NULL)
            return v->str;
    }
    return "";
}

int
main(int argc, char **argv)
{
    static const char *const tables[] = { ROWS, ACCOUNTS };

    tsapi_sql_sess    sess = { 0 };
    tapi_sql_conn    *primary = NULL;
    tapi_sql_conn    *replica = NULL;
    tapi_sql_conn    *admin = NULL;
    tapi_sql_config   cfg;
    tapi_sql_cmp_opts strict = TAPI_SQL_CMP_OPTS_DEFAULT;
    tapi_sql_result   res = TAPI_SQL_RESULT_INIT;
    tapi_sql_report   report = TAPI_SQL_REPORT_INIT;
    const char       *backend;
    int64_t           n = 0;
    unsigned int      waited = 0;
    double            lag_ms[ROUNDS];
    int               i;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    strict.strict_types = true;

    TEST_STEP("Open a session; skip when there is no replica");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_repl"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    if (!tsapi_sql_sess_has_replica(&sess))
        TEST_SKIP("The suite has no replica of this server");

    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &primary));
    tsapi_sql_sess_replica_config(&sess, TSAPI_SQL_ROLE_APP, &cfg);
    CHECK_RC(tapi_sql_connect(sess.pco, &cfg, &replica));

    TEST_STEP("The replica knows it is one, and replication is running");
    if (sess.engine == TAPI_SQL_ENGINE_POSTGRES)
    {
        CHECK_RC(tapi_sql_query_int(replica, "SELECT pg_is_in_recovery()",
                                    &n));
        if (n != 1)
            TEST_VERDICT("The replica is not in recovery");
        CHECK_RC(tapi_sql_query_int(primary, "SELECT pg_is_in_recovery()",
                                    &n));
        if (n != 0)
            TEST_VERDICT("The primary is in recovery");

        /* The primary's administrator sees who is streaming from it. */
        CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_ADMIN,
                                        &admin));
        CHECK_RC(tapi_sql_query_int(admin, "SELECT count(*) FROM "
                                    "pg_stat_replication "
                                    "WHERE state = 'streaming'", &n));
        if (n < 1)
            TEST_VERDICT("Nothing is streaming from the primary");
    }
    else
    {
        CHECK_RC(tapi_sql_query_int(replica, "SELECT @@read_only", &n));
        if (n != 1)
            TEST_VERDICT("The replica is not read-only");

        /* The replica's administrator sees its two threads. */
        tsapi_sql_sess_replica_config(&sess, TSAPI_SQL_ROLE_ADMIN, &cfg);
        CHECK_RC(tapi_sql_connect(sess.pco, &cfg, &admin));
        CHECK_RC(tapi_sql_query(admin, "SHOW REPLICA STATUS", &res));
        RING("Replica threads: IO %s, SQL %s; last error '%s'",
             column(&res, "Slave_IO_Running"),
             column(&res, "Slave_SQL_Running"), column(&res, "Last_Error"));
        if (strcmp(column(&res, "Slave_IO_Running"), "Yes") != 0 ||
            strcmp(column(&res, "Slave_SQL_Running"), "Yes") != 0)
            TEST_VERDICT("The replica's replication threads are not "
                         "running");
    }

    TEST_STEP("A table created on the primary appears on the replica");
    tsapi_sql_drop(primary, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(primary, "CREATE TABLE " ROWS " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "note VARCHAR(40), amount DECIMAL(10,2))",
                              NULL));
    CHECK_RC(tapi_sql_assert_eventually(replica, "SELECT count(*) FROM "
                                        ROWS, 0, ARRIVE_MS, &waited));
    RING("The table reached the replica in %u ms", waited);

    TEST_STEP("Rows committed on the primary arrive, each timed");
    for (i = 0; i < ROUNDS; i++)
    {
        CHECK_RC(tapi_sql_execute_fmt(primary, NULL, "INSERT INTO " ROWS
                                      " VALUES (%d, 'row %d', %d.25)",
                                      i + 1, i + 1, i));
        CHECK_RC(tapi_sql_assert_eventually(replica, "SELECT count(*) FROM "
                                            ROWS, i + 1, ARRIVE_MS,
                                            &waited));
        lag_ms[i] = (double)waited;
    }

    TEST_STEP("...and are the same rows, to the last value and type");
    CHECK_RC(tapi_sql_assert_query_equal(
                 replica, "SELECT id, note, amount FROM " ROWS
                          " ORDER BY id",
                 primary, "SELECT id, note, amount FROM " ROWS
                          " ORDER BY id", &strict));

    TEST_STEP("The replica refuses a write, as read-only");
    rc = tapi_sql_execute(replica, "INSERT INTO " ROWS
                          " VALUES (500, 'written on the replica', 0)",
                          NULL);
    RING("Write on the replica: rc=%r class=%s: %s", rc,
         tapi_sql_err_class2str(tapi_sql_last_error(replica)->cls),
         tapi_sql_last_error(replica)->message);
    if (rc == 0)
        TEST_VERDICT("The replica accepted a write");
    if (tapi_sql_last_error(replica)->cls != TAPI_SQL_ERR_READ_ONLY)
        TEST_VERDICT("A write on the replica was refused as '%s'",
                     tapi_sql_err_class2str(
                         tapi_sql_last_error(replica)->cls));
    if (tapi_sql_execute(replica, "DELETE FROM " ROWS, NULL) == 0 ||
        tapi_sql_execute(replica, "CREATE TABLE t_repl_stray (id INTEGER)",
                         NULL) == 0)
        TEST_VERDICT("The replica accepted a delete or a new table");
    CHECK_RC(tapi_sql_ping(replica));
    CHECK_RC(tapi_sql_assert_row_count(replica, ROWS, ROUNDS));
    CHECK_RC(tapi_sql_assert_row_count(primary, ROWS, ROUNDS));

    TEST_STEP("A rolled back transaction never arrives");
    CHECK_RC(tapi_sql_begin(primary));
    CHECK_RC(tapi_sql_execute(primary, "INSERT INTO " ROWS
                              " VALUES (999, 'rolled back', 0)", NULL));
    CHECK_RC(tapi_sql_rollback(primary));
    /* A committed row after it marks the point the replica must pass. */
    CHECK_RC(tapi_sql_execute(primary, "INSERT INTO " ROWS
                              " VALUES (1000, 'marker', 0)", NULL));
    CHECK_RC(tapi_sql_assert_eventually(replica, "SELECT count(*) FROM "
                                        ROWS " WHERE id = 1000", 1,
                                        ARRIVE_MS, NULL));
    CHECK_RC(tapi_sql_assert_row_count(replica, ROWS " WHERE id = 999", 0));

    TEST_STEP("A transaction arrives whole: transfers never seen half "
              "done");
    CHECK_RC(tapi_sql_execute(primary, "CREATE TABLE " ACCOUNTS " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "balance INTEGER NOT NULL)", NULL));
    CHECK_RC(tapi_sql_execute(primary, "INSERT INTO " ACCOUNTS
                              " VALUES (1, 500), (2, 500)", NULL));
    CHECK_RC(tapi_sql_assert_eventually(replica, TOTAL, 1000, ARRIVE_MS,
                                        NULL));
    for (i = 0; i < TRANSFERS; i++)
    {
        CHECK_RC(tapi_sql_begin(primary));
        CHECK_RC(tapi_sql_execute(primary, "UPDATE " ACCOUNTS
                                  " SET balance = balance - 5 WHERE id = 1",
                                  NULL));
        /* The replica is read between the two halves and after. */
        CHECK_RC(tapi_sql_query_int(replica, TOTAL, &n));
        if (n != 1000)
            TEST_VERDICT("The replica showed half of a transaction");
        CHECK_RC(tapi_sql_execute(primary, "UPDATE " ACCOUNTS
                                  " SET balance = balance + 5 WHERE id = 2",
                                  NULL));
        CHECK_RC(tapi_sql_commit(primary));
        CHECK_RC(tapi_sql_query_int(replica, TOTAL, &n));
        if (n != 1000)
            TEST_VERDICT("The replica showed half of a transaction");
    }
    CHECK_RC(tapi_sql_assert_eventually(replica, "SELECT balance FROM "
                                        ACCOUNTS " WHERE id = 1",
                                        500 - 5 * TRANSFERS, ARRIVE_MS,
                                        NULL));
    CHECK_RC(tapi_sql_assert_number(replica, TOTAL, 1000.0, 0.0, 0.0));

    TEST_STEP("Uncommitted work on the primary is not on the replica");
    CHECK_RC(tapi_sql_begin(primary));
    CHECK_RC(tapi_sql_execute(primary, "UPDATE " ACCOUNTS
                              " SET balance = balance + 1000000", NULL));
    te_motivated_msleep(500, "give the change time to leak, if it can");
    CHECK_RC(tapi_sql_assert_number(replica, TOTAL, 1000.0, 0.0, 0.0));
    CHECK_RC(tapi_sql_rollback(primary));

    TEST_STEP("The posture audit reads a replica too, and changes "
              "nothing");
    CHECK_RC(tapi_sql_audit(replica, &report));
    tapi_sql_report_log(&report);
    if (!tapi_sql_report_has(&report, "sql.tls.off"))
        TEST_VERDICT("The audit missed the replica's unencrypted session");
    CHECK_RC(tapi_sql_assert_row_count(replica, ROWS, ROUNDS + 1));

    TEST_STEP("Dropping the tables on the primary removes them from the "
              "replica");
    tsapi_sql_drop(primary, tables, TE_ARRAY_LEN(tables));
    for (i = 0; i < ARRIVE_MS / 100; i++)
    {
        if (tapi_sql_query_int(replica, "SELECT count(*) FROM " ROWS,
                               &n) != 0)
            break;
        te_motivated_msleep(100, "wait for the drop to arrive");
    }
    if (i == ARRIVE_MS / 100)
        TEST_VERDICT("A dropped table is still on the replica");

    tsapi_sql_chart("Time for a committed row to reach the replica, ms",
                    NULL, NULL, "ms", lag_ms, ROUNDS);

    TEST_SUCCESS;

cleanup:
    tapi_sql_result_free(&res);
    tapi_sql_report_free(&report);
    if (primary != NULL)
        tapi_sql_rollback(primary);
    tapi_sql_disconnect(admin);
    tapi_sql_disconnect(replica);
    tsapi_sql_drop(primary, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(primary);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
