/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Aggregation: counts, sums, minima, maxima and averages over a whole
 * table and per group, a HAVING filter, DISTINCT inside an aggregate,
 * conditional aggregation, a window function, an aggregate compared
 * with an aggregate of aggregates - and the two places aggregates
 * surprise: NULLs (skipped, never counted as zero) and empty input
 * (a count of 0, every other aggregate NULL, no groups at all). Each
 * result is checked against values worked out by hand.
 *
 * The engines disagree about the type of an aggregate - a sum of
 * integers is an integer here and a decimal there - so the values are
 * compared as numbers.
 */

#define TE_TEST_NAME    "sql/aggregates"

#include "te_config.h"

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define TABLE   "t_agg"
#define EMPTY   "t_agg_empty"

#define I(_v)   { .type = TAPI_SQL_INT, .i = (_v) }
#define F(_v)   { .type = TAPI_SQL_FLOAT, .f = (_v) }
#define T(_s)   { .type = TAPI_SQL_TEXT, .str = (_s), .len = sizeof(_s) - 1 }
#define N       { .type = TAPI_SQL_NULL }

/** A query and the rows it must return, in order. */
typedef struct agg_case {
    const char    *name;
    const char    *sql;
    unsigned int   ncols;
    unsigned int   nrows;
    tapi_sql_value cells[16];
} agg_case;

static agg_case cases[] = {
    { "whole table",
      "SELECT count(*), count(price), sum(qty), min(qty), max(qty) FROM "
      TABLE,
      5, 1, { I(7), I(6), I(46), I(0), I(20) } },
    { "average",
      "SELECT avg(qty) FROM " TABLE " WHERE id <= 4",
      1, 1, { F(9.25) } },
    { "group by",
      "SELECT region, count(*), sum(qty) FROM " TABLE
      " GROUP BY region ORDER BY region",
      3, 3, { T("east"), I(1), I(0), T("north"), I(3), I(35),
              T("south"), I(3), I(11) } },
    { "having",
      "SELECT region, sum(qty) FROM " TABLE
      " GROUP BY region HAVING sum(qty) > 10 ORDER BY region",
      2, 2, { T("north"), I(35), T("south"), I(11) } },
    { "sum of a product, a NULL factor skipped",
      "SELECT product, sum(qty * price) FROM " TABLE
      " GROUP BY product ORDER BY product",
      2, 3, { T("bolt"), F(7.5), T("gear"), F(36.0), T("nut"), F(5.0) } },
    { "distinct inside an aggregate",
      "SELECT count(DISTINCT product), count(DISTINCT region) FROM " TABLE,
      2, 1, { I(3), I(3) } },
    { "average skips NULL",
      "SELECT avg(price), count(*), count(price) FROM " TABLE
      " WHERE region = 'south'",
      3, 1, { F(12.0), I(3), I(2) } },
    { "sum of nothing but NULL is NULL",
      "SELECT sum(price), max(price) FROM " TABLE " WHERE id = 5",
      2, 1, { N, N } },
    { "empty input",
      "SELECT count(*), sum(qty), max(qty), avg(qty) FROM " EMPTY,
      4, 1, { I(0), N, N, N } },
    { "empty input has no groups",
      "SELECT region, count(*) FROM " EMPTY " GROUP BY region",
      2, 0, { N } },
    { "conditional aggregation",
      "SELECT sum(CASE WHEN qty >= 10 THEN 1 ELSE 0 END), "
      "sum(CASE WHEN price IS NULL THEN 1 ELSE 0 END) FROM " TABLE,
      2, 1, { I(2), I(1) } },
    { "window: running total per region",
      "SELECT id, sum(qty) OVER (PARTITION BY region ORDER BY id) FROM "
      TABLE " ORDER BY id",
      2, 7, { I(1), I(10), I(2), I(30), I(3), I(35), I(4), I(2),
              I(5), I(10), I(6), I(0), I(7), I(11) } },
    { "the group with the largest total",
      "SELECT product FROM " TABLE " GROUP BY product HAVING sum(qty) = "
      "(SELECT max(s) FROM (SELECT sum(qty) AS s FROM " TABLE
      " GROUP BY product) totals)",
      1, 1, { T("bolt") } },
};

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TABLE, EMPTY };

    tsapi_sql_sess    sess = { 0 };
    tapi_sql_conn    *conn = NULL;
    tapi_sql_cmp_opts opts = TAPI_SQL_CMP_OPTS_DEFAULT;
    tapi_sql_result   got = TAPI_SQL_RESULT_INIT;
    const char       *backend;
    double            rows[TE_ARRAY_LEN(cases)];
    unsigned int      i;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    opts.abs_tol = 1e-9;

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_agg"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("Seven sales, one with no price, and an empty table");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " TABLE " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "region VARCHAR(10) NOT NULL, "
                              "product VARCHAR(10) NOT NULL, "
                              "qty INTEGER NOT NULL, "
                              "price DECIMAL(10,2))", NULL));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " EMPTY " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "region VARCHAR(10), qty INTEGER)", NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE " VALUES "
                              "(1, 'north', 'bolt', 10, 0.50), "
                              "(2, 'north', 'nut', 20, 0.25), "
                              "(3, 'north', 'bolt', 5, 0.50), "
                              "(4, 'south', 'gear', 2, 12.00), "
                              "(5, 'south', 'bolt', 8, NULL), "
                              "(6, 'east', 'nut', 0, 0.25), "
                              "(7, 'south', 'gear', 1, 12.00)", NULL));

    TEST_STEP("Every aggregate gives the value worked out by hand");
    for (i = 0; i < TE_ARRAY_LEN(cases); i++)
    {
        tapi_sql_result expected = { .ncols = cases[i].ncols,
                                     .nrows = cases[i].nrows,
                                     .cells = cases[i].cells };

        CHECK_RC(tapi_sql_query(conn, cases[i].sql, &got));
        tapi_sql_result_log(&got, cases[i].name, 10);
        rows[i] = (double)got.nrows;
        if (!tapi_sql_result_equal(&got, &expected, &opts, NULL))
        {
            /* Run the comparison again as an assertion, for its log. */
            tapi_sql_assert_result(&got, &expected, &opts, cases[i].name);
            TEST_STOP;
        }
    }

    tsapi_sql_chart("Rows returned, per aggregate query", NULL, NULL,
                    "rows", rows, TE_ARRAY_LEN(cases));

    TEST_SUCCESS;

cleanup:
    tapi_sql_result_free(&got);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
