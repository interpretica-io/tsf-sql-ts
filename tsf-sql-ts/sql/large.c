/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * A large result is read in bounded batches and never held whole: fifty
 * thousand rows are fetched five hundred at a time under a byte bound,
 * every batch respects both bounds, the largest batch the agent ever
 * built is a small fraction of the result, and nothing is missing or
 * repeated at the end.
 */

#define TE_TEST_NAME    "sql/large"

#include "te_config.h"

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tapi_sql_perf.h"
#include "tsapi_sql.h"

#define DIGITS      "t_large_digits"
#define TABLE       "t_large"
#define TOTAL_ROWS  50000
#define BATCH_ROWS  500
#define BATCH_BYTES 16384
#define MAX_POINTS  200

int
main(int argc, char **argv)
{
    static const char *const tables[] = { DIGITS, TABLE };
    static const double      pct[] = { 50, 95, 99 };

    tsapi_sql_sess  sess = { 0 };
    tapi_sql_conn  *conn = NULL;
    tapi_sql_stmt  *stmt = NULL;
    tapi_sql_result res = TAPI_SQL_RESULT_INIT;
    tapi_sql_perf   perf;
    const char     *backend;
    int64_t         affected = 0;
    int64_t         sum = 0;
    int64_t         expected_next = 1;
    uint64_t        max_batch = 0;
    unsigned int    total = 0;
    unsigned int    batches = 0;
    unsigned int    row;
    double          sizes[MAX_POINTS];
    bool            eof = false;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    tapi_sql_perf_init(&perf, "read 50000 rows");

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_large"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("Fill a table with fifty thousand rows");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tsapi_sql_digits(conn, DIGITS));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " TABLE " ("
                              "n INTEGER PRIMARY KEY, pad VARCHAR(40))",
                              NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE " (n, pad) "
                              "SELECT a.d + 10 * b.d + 100 * c.d + "
                              "1000 * e.d + 10000 * f.d + 1, "
                              "'row-padding-row-padding' "
                              "FROM " DIGITS " a, " DIGITS " b, " DIGITS
                              " c, " DIGITS " e, " DIGITS " f "
                              "WHERE f.d < 5", &affected));
    if (affected != TOTAL_ROWS)
        TEST_VERDICT("The insert reported %lld rows, not %d",
                     (long long)affected, TOTAL_ROWS);

    TEST_STEP("Read it back in bounded batches");
    CHECK_RC(tapi_sql_prepare(conn, "SELECT n, pad FROM " TABLE
                              " ORDER BY n", &stmt));
    CHECK_RC(tapi_sql_stmt_execute(stmt, NULL));
    while (!eof)
    {
        CHECK_RC(tapi_sql_fetch_bounded(stmt, BATCH_ROWS, BATCH_BYTES, &res,
                                        &eof));
        if (res.nrows > BATCH_ROWS)
            TEST_VERDICT("A batch exceeded the row bound");
        if (res.nrows == 0 && !eof)
            TEST_VERDICT("An empty batch before the end of the result");

        for (row = 0; row < res.nrows; row++)
        {
            int64_t n = tapi_sql_result_cell(&res, row, 0)->i;

            if (n != expected_next)
                TEST_VERDICT("Rows were lost, repeated or reordered across "
                             "batches");
            expected_next++;
            sum += n;
        }
        if (res.nrows > 0 && batches < MAX_POINTS)
            sizes[batches] = (double)res.nrows;
        batches += res.nrows > 0 ? 1 : 0;
        total += res.nrows;
    }
    RING("%u rows in %u batches", total, batches);

    TEST_STEP("Everything arrived");
    if (total != TOTAL_ROWS)
        TEST_VERDICT("%u rows arrived, not %d", total, TOTAL_ROWS);
    if (sum != (int64_t)TOTAL_ROWS * (TOTAL_ROWS + 1) / 2)
        TEST_VERDICT("The rows do not add up");
    if (batches < TOTAL_ROWS / BATCH_ROWS)
        TEST_VERDICT("The result came in fewer batches than the row bound "
                     "allows");

    TEST_STEP("The agent never built more than one bounded batch");
    CHECK_RC(tapi_sql_stats(sess.pco, NULL, NULL, &max_batch));
    RING("Largest batch on the agent: %llu bytes (bound %d)",
         (unsigned long long)max_batch, BATCH_BYTES);
    if (max_batch > BATCH_BYTES)
        TEST_VERDICT("The agent built a batch larger than the byte bound");
    if (max_batch == 0)
        TEST_VERDICT("The agent reports no batch at all");

    TEST_STEP("An abandoned result costs nothing: stop after one batch");
    CHECK_RC(tapi_sql_stmt_execute(stmt, NULL));
    CHECK_RC(tapi_sql_fetch(stmt, 10, &res, &eof));
    if (res.nrows != 10 || eof)
        TEST_VERDICT("The first ten rows did not come as ten rows");
    CHECK_RC(tapi_sql_statement_free(stmt));
    stmt = NULL;
    CHECK_RC(tapi_sql_assert_row_count(conn, TABLE, TOTAL_ROWS));

    TEST_STEP("Measure rows per second over three full reads");
    CHECK_RC(tapi_sql_perf_run_query(conn, "SELECT n, pad FROM " TABLE, 3,
                                     BATCH_ROWS, &perf));
    if (perf.count != 3 || perf.rows != 3 * TOTAL_ROWS)
        TEST_VERDICT("The measured reads did not read every row");
    tapi_sql_perf_log(&perf, pct, TE_ARRAY_LEN(pct));

    tsapi_sql_chart("Rows per batch", NULL, NULL, "rows", sizes,
                    batches < MAX_POINTS ? batches : MAX_POINTS);

    TEST_SUCCESS;

cleanup:
    tapi_sql_perf_free(&perf);
    tapi_sql_result_free(&res);
    tapi_sql_statement_free(stmt);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
