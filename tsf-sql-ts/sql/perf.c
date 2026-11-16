/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Performance measurements: query latency, rows processed per second,
 * transaction throughput and connection establishment latency are
 * measured and reported with count, minimum, maximum, mean and
 * percentiles; error rate and timeout frequency are measured on
 * statements made to fail and to time out, where the right answer is
 * known. The numbers themselves are not judged - this is a functional
 * suite on a laptop or a CI runner - only that they are consistent.
 */

#define TE_TEST_NAME    "sql/perf"

#include "te_config.h"

#include "tapi_test.h"
#include "te_string.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tapi_sql_perf.h"
#include "tsapi_sql.h"

#define DIGITS  "t_perf_digits"
#define TABLE   "t_perf"
#define QUERIES 100
#define TXS     30
#define CONNS   10
#define READS   5
#define FAILS   10
#define SLOWS   3

/* A series must be self-consistent before its numbers mean anything. */
static void
check_series(const tapi_sql_perf *perf, unsigned int count,
             unsigned int errors)
{
    tapi_sql_perf_stats st;
    double              p50 = tapi_sql_perf_percentile(perf, 50);
    double              p99 = tapi_sql_perf_percentile(perf, 99);

    tapi_sql_perf_stats_get(perf, &st);
    if (st.count != count || st.errors != errors)
        TEST_VERDICT("Series '%s' counted %u successes and %u errors, not "
                     "%u and %u", perf->name, st.count, st.errors, count,
                     errors);
    if (count == 0)
        return;
    if (!(st.min_us > 0.0 && st.min_us <= st.mean_us &&
          st.mean_us <= st.max_us))
        TEST_VERDICT("Series '%s': minimum, mean and maximum are out of "
                     "order", perf->name);
    if (!(st.min_us <= p50 && p50 <= p99 && p99 <= st.max_us))
        TEST_VERDICT("Series '%s': the percentiles are out of order",
                     perf->name);
    if (st.ops_per_sec <= 0.0)
        TEST_VERDICT("Series '%s' has no throughput", perf->name);
}

int
main(int argc, char **argv)
{
    static const char *const tables[] = { DIGITS, TABLE };
    static const double      pct[] = { 50, 90, 95, 99 };
    static const char *const transfer[] = {
        "UPDATE " TABLE " SET v = v - 1 WHERE id = 1",
        "UPDATE " TABLE " SET v = v + 1 WHERE id = 2",
    };

    tsapi_sql_sess      sess = { 0 };
    tapi_sql_conn      *conn = NULL;
    tapi_sql_config     cfg;
    tapi_sql_perf       query;
    tapi_sql_perf       reading;
    tapi_sql_perf       tx;
    tapi_sql_perf       connecting;
    tapi_sql_perf       failing;
    tapi_sql_perf       slow;
    tapi_sql_perf_stats st;
    te_string           slow_sql = TE_STRING_INIT;
    const char         *backend;
    double              means[4];

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    tapi_sql_perf_init(&query, "point query");
    tapi_sql_perf_init(&reading, "read 1000 rows");
    tapi_sql_perf_init(&tx, "transfer transaction");
    tapi_sql_perf_init(&connecting, "connect");
    tapi_sql_perf_init(&failing, "failing statement");
    tapi_sql_perf_init(&slow, "statement past its deadline");

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_perf"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("A thousand rows to work on");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tsapi_sql_digits(conn, DIGITS));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " TABLE
                              " (id INTEGER PRIMARY KEY, v INTEGER)", NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE " (id, v) "
                              "SELECT a.d + 10 * b.d + 100 * c.d + 1, 1000 "
                              "FROM " DIGITS " a, " DIGITS " b, " DIGITS
                              " c", NULL));

    TEST_STEP("Query latency");
    CHECK_RC(tapi_sql_perf_run(conn, "SELECT v FROM " TABLE
                               " WHERE id = 500", QUERIES, &query));
    check_series(&query, QUERIES, 0);
    tapi_sql_perf_log(&query, pct, TE_ARRAY_LEN(pct));

    TEST_STEP("Rows processed per second");
    CHECK_RC(tapi_sql_perf_run_query(conn, "SELECT id, v FROM " TABLE, READS,
                                     250, &reading));
    check_series(&reading, READS, 0);
    tapi_sql_perf_stats_get(&reading, &st);
    if (reading.rows != READS * 1000 || st.rows_per_sec <= 0.0)
        TEST_VERDICT("The row rate does not account for every row read");
    tapi_sql_perf_log(&reading, pct, TE_ARRAY_LEN(pct));

    TEST_STEP("Transaction throughput");
    CHECK_RC(tapi_sql_perf_run_tx(conn, transfer, TE_ARRAY_LEN(transfer),
                                  TXS, &tx));
    check_series(&tx, TXS, 0);
    tapi_sql_perf_log(&tx, pct, TE_ARRAY_LEN(pct));
    CHECK_RC(tapi_sql_assert_number(conn, "SELECT v FROM " TABLE
                                    " WHERE id = 2", 1000 + TXS, 0.0, 0.0));

    TEST_STEP("Connection establishment latency");
    tsapi_sql_sess_config(&sess, TSAPI_SQL_ROLE_APP, &cfg);
    CHECK_RC(tapi_sql_perf_run_connect(sess.pco, &cfg, CONNS, &connecting));
    check_series(&connecting, CONNS, 0);
    tapi_sql_perf_log(&connecting, pct, TE_ARRAY_LEN(pct));

    TEST_STEP("Error rate, on a statement that always fails");
    CHECK_RC(tapi_sql_perf_run(conn, "INSERT INTO " TABLE " VALUES (1, 0)",
                               FAILS, &failing));
    check_series(&failing, 0, FAILS);
    tapi_sql_perf_stats_get(&failing, &st);
    if (st.error_rate != 1.0 || st.timeouts != 0)
        TEST_VERDICT("Ten failures out of ten are not an error rate of 1");
    tapi_sql_perf_log(&failing, pct, TE_ARRAY_LEN(pct));

    TEST_STEP("Timeout frequency, on a statement that always times out");
    tsapi_sql_slow_query(DIGITS, &slow_sql);
    CHECK_RC(tapi_sql_set_timeout(conn, TAPI_SQL_TIMEOUT_STATEMENT, 200));
    CHECK_RC(tapi_sql_perf_run(conn, slow_sql.ptr, SLOWS, &slow));
    CHECK_RC(tapi_sql_set_timeout(conn, TAPI_SQL_TIMEOUT_STATEMENT, 0));
    tapi_sql_perf_stats_get(&slow, &st);
    if (st.errors != SLOWS || st.timeouts != SLOWS ||
        st.timeout_rate != 1.0)
        TEST_VERDICT("Statements past their deadline were not all counted "
                     "as timeouts");
    tapi_sql_perf_log(&slow, pct, TE_ARRAY_LEN(pct));

    tapi_sql_perf_stats_get(&query, &st);
    means[0] = st.mean_us;
    tapi_sql_perf_stats_get(&reading, &st);
    means[1] = st.mean_us;
    tapi_sql_perf_stats_get(&tx, &st);
    means[2] = st.mean_us;
    tapi_sql_perf_stats_get(&connecting, &st);
    means[3] = st.mean_us;
    tsapi_sql_chart("Mean latency, us: point query, 1000-row read, "
                    "transaction, connect", NULL, NULL, "us", means, 4);

    TEST_SUCCESS;

cleanup:
    te_string_free(&slow_sql);
    tapi_sql_perf_free(&query);
    tapi_sql_perf_free(&reading);
    tapi_sql_perf_free(&tx);
    tapi_sql_perf_free(&connecting);
    tapi_sql_perf_free(&failing);
    tapi_sql_perf_free(&slow);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
