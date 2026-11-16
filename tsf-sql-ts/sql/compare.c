/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Comparing two results: in order and as bags of rows, with NULL equal
 * to NULL or to nothing, with numbers compared by value or by type,
 * exactly or within an absolute or a relative tolerance. Every case is
 * decided both ways - a pair that must be equal and a pair that must
 * not - so a comparison that always says "equal" fails here.
 */

#define TE_TEST_NAME    "sql/compare"

#include "te_config.h"

#include "tapi_test.h"
#include "te_string.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define TA  "t_cmp_a"
#define TB  "t_cmp_b"

/** One comparison and the answer it must give. */
typedef struct cmp_case {
    const char       *name;
    const char       *sql_got;
    const char       *sql_exp;
    tapi_sql_cmp_opts opts;
    bool              equal;
} cmp_case;

#define ROWS_A      "SELECT grp, val, note FROM " TA " ORDER BY id"
#define ROWS_B_REV  "SELECT grp, val, note FROM " TB " ORDER BY id DESC"
#define BAG_XXY     "SELECT 'x' AS g UNION ALL SELECT 'x' UNION ALL SELECT 'y'"
#define BAG_XYY     "SELECT 'x' AS g UNION ALL SELECT 'y' UNION ALL SELECT 'y'"
#define BAG_YXX     "SELECT 'y' AS g UNION ALL SELECT 'x' UNION ALL SELECT 'x'"

static const cmp_case cases[] = {
    { "ordered, same rows", ROWS_A, ROWS_A,
      { .ordered = true, .null_equals_null = true }, true },
    { "ordered, other order", ROWS_A, ROWS_B_REV,
      { .ordered = true, .null_equals_null = true, .abs_tol = 1e-3 },
      false },
    { "unordered, other order, within tolerance", ROWS_A, ROWS_B_REV,
      { .ordered = false, .null_equals_null = true, .abs_tol = 1e-3 },
      true },
    { "unordered, other order, no tolerance", ROWS_A, ROWS_B_REV,
      { .ordered = false, .null_equals_null = true }, false },
    { "NULL equals nothing", ROWS_A, ROWS_A,
      { .ordered = true, .null_equals_null = false }, false },
    { "bag, same rows other order", BAG_XXY, BAG_YXX,
      { .ordered = false, .null_equals_null = true }, true },
    { "bag, same values other counts", BAG_XXY, BAG_XYY,
      { .ordered = false, .null_equals_null = true }, false },
    { "numbers by value", "SELECT 2", "SELECT 2.0",
      { .ordered = true, .null_equals_null = true }, true },
    { "numbers by type", "SELECT 2", "SELECT 2.0",
      { .ordered = true, .null_equals_null = true, .strict_types = true },
      false },
    { "relative tolerance, inside", "SELECT 1000000", "SELECT 1000001",
      { .ordered = true, .null_equals_null = true, .rel_tol = 1e-5 },
      true },
    { "relative tolerance, outside", "SELECT 1000000", "SELECT 1000001",
      { .ordered = true, .null_equals_null = true, .rel_tol = 1e-7 },
      false },
    { "other shape", ROWS_A, BAG_XXY "  UNION ALL SELECT 'z'",
      { .ordered = false, .null_equals_null = true }, false },
};

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TA, TB };

    tsapi_sql_sess  sess = { 0 };
    tapi_sql_conn  *conn = NULL;
    tapi_sql_result got = TAPI_SQL_RESULT_INIT;
    tapi_sql_result exp = TAPI_SQL_RESULT_INIT;
    te_string       diag = TE_STRING_INIT;
    const char     *backend;
    double          right[TE_ARRAY_LEN(cases)];
    unsigned int    i;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_compare"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("Two tables with the same rows, one value a hair off");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " TA " (id INTEGER "
                              "PRIMARY KEY, grp VARCHAR(8), "
                              "val DOUBLE PRECISION, note VARCHAR(8))",
                              NULL));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " TB " (id INTEGER "
                              "PRIMARY KEY, grp VARCHAR(8), "
                              "val DOUBLE PRECISION, note VARCHAR(8))",
                              NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TA " VALUES "
                              "(1, 'x', 1.0, 'n'), (2, 'x', 2.0, NULL), "
                              "(3, 'y', 3.0, 'm')", NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TB " VALUES "
                              "(1, 'x', 1.0, 'n'), (2, 'x', 2.0, NULL), "
                              "(3, 'y', 3.0000001, 'm')", NULL));

    TEST_STEP("Decide every case and check the answer");
    for (i = 0; i < TE_ARRAY_LEN(cases); i++)
    {
        bool equal;

        CHECK_RC(tapi_sql_query(conn, cases[i].sql_got, &got));
        CHECK_RC(tapi_sql_query(conn, cases[i].sql_exp, &exp));
        te_string_reset(&diag);
        equal = tapi_sql_result_equal(&got, &exp, &cases[i].opts, &diag);
        RING("case '%s': %s%s%s", cases[i].name,
             equal ? "equal" : "different", diag.len > 0 ? "\n" : "",
             diag.len > 0 ? diag.ptr : "");

        if (equal != cases[i].equal)
            TEST_VERDICT("Comparison '%s' gave the wrong answer",
                         cases[i].name);
        if (!equal && diag.len == 0)
            TEST_VERDICT("Comparison '%s' found a difference and described "
                         "none", cases[i].name);
        right[i] = 1.0;
    }

    tsapi_sql_chart("Comparison cases decided correctly", NULL, NULL,
                    "correct", right, TE_ARRAY_LEN(cases));

    TEST_SUCCESS;

cleanup:
    te_string_free(&diag);
    tapi_sql_result_free(&got);
    tapi_sql_result_free(&exp);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
