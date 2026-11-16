/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Parameterised statements: every value type is bound, stored and read
 * back unchanged - a text with quotes, tabs and newlines in it, binary
 * with every byte value including NUL, a 64-bit integer a double cannot
 * hold, NULLs, empty values. A statement is prepared once and executed
 * with different bindings.
 */

#define TE_TEST_NAME    "sql/params"

#include "te_config.h"

#include <string.h>

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define TABLE   "t_params"
#define UTABLE  "t_params_u"
#define NCOLS   8
#define NROWS   3

/* The awkward text: what breaks a statement built by pasting. */
#define AWKWARD "o'brien said \"hi\"; -- \\ \t tab \n newline \xc3\xa9"

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TABLE, UTABLE };

    tsapi_sql_sess  sess = { 0 };
    tapi_sql_conn  *conn = NULL;
    tapi_sql_stmt  *ins = NULL;
    tapi_sql_stmt  *sel = NULL;
    tapi_sql_result got = TAPI_SQL_RESULT_INIT;
    tapi_sql_value  rows[NROWS][NCOLS];
    unsigned char   bytes[256];
    const char     *backend;
    double          equal[NROWS];
    unsigned int    i;
    unsigned int    j;
    bool            eof = false;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    for (i = 0; i < sizeof(bytes); i++)
        bytes[i] = (unsigned char)i;

    /* Row 1: one of everything. */
    rows[0][0] = tapi_sql_int(1);
    rows[0][1] = tapi_sql_text(AWKWARD);
    rows[0][2] = tapi_sql_int(9007199254740993LL);
    rows[0][3] = tapi_sql_float(0.1);
    rows[0][4] = tapi_sql_decimal("12345.678900");
    rows[0][5] = tapi_sql_binary(bytes, sizeof(bytes));
    rows[0][6] = tapi_sql_datetime("2024-02-29");
    rows[0][7] = tapi_sql_bool(true);
    /* Row 2: NULL wherever it is allowed. */
    rows[1][0] = tapi_sql_int(2);
    for (j = 1; j < NCOLS; j++)
        rows[1][j] = tapi_sql_null();
    /* Row 3: empty and extreme values. */
    rows[2][0] = tapi_sql_int(3);
    rows[2][1] = tapi_sql_text("");
    rows[2][2] = tapi_sql_int(-9223372036854775807LL);
    rows[2][3] = tapi_sql_float(-1.5e300);
    rows[2][4] = tapi_sql_decimal("-0.000001");
    rows[2][5] = tapi_sql_binary("", 0);
    rows[2][6] = tapi_sql_datetime("1999-12-31");
    rows[2][7] = tapi_sql_bool(false);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_params"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("Create a table with a column of every type");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute_fmt(conn, NULL, "CREATE TABLE " TABLE " ("
                                  "id INTEGER PRIMARY KEY, t VARCHAR(200), "
                                  "i BIGINT, f DOUBLE PRECISION, "
                                  "d DECIMAL(20,6), b %s, m DATE, "
                                  "flag BOOLEAN)",
                                  tsapi_sql_blob_type(sess.engine)));

    TEST_STEP("Insert three rows through one prepared statement");
    CHECK_RC(tapi_sql_prepare(conn, "INSERT INTO " TABLE
                              " VALUES (?, ?, ?, ?, ?, ?, ?, ?)", &ins));
    if (tapi_sql_stmt_nparams(ins) != NCOLS)
        TEST_VERDICT("A statement with %d parameters reports %u", NCOLS,
                     tapi_sql_stmt_nparams(ins));
    for (i = 0; i < NROWS; i++)
    {
        int64_t affected = 0;

        for (j = 0; j < NCOLS; j++)
            CHECK_RC(tapi_sql_bind(ins, j + 1, rows[i][j]));
        CHECK_RC(tapi_sql_stmt_execute(ins, &affected));
        if (affected != 1)
            TEST_VERDICT("A one-row insert reported %lld",
                         (long long)affected);
    }

    TEST_STEP("Read each row back by a bound key and compare the values");
    CHECK_RC(tapi_sql_prepare(conn, "SELECT id, t, i, f, d, b, m, flag FROM "
                              TABLE " WHERE id = ?", &sel));
    for (i = 0; i < NROWS; i++)
    {
        unsigned int same = 0;

        CHECK_RC(tapi_sql_bind(sel, 1, rows[i][0]));
        CHECK_RC(tapi_sql_stmt_execute(sel, NULL));
        CHECK_RC(tapi_sql_fetch(sel, 10, &got, &eof));
        if (got.nrows != 1 || got.ncols != NCOLS)
            TEST_VERDICT("The lookup of row %u returned %u row(s) of %u "
                         "column(s)", i + 1, got.nrows, got.ncols);
        for (j = 0; j < NCOLS; j++)
        {
            const tapi_sql_value *v = tapi_sql_result_cell(&got, 0, j);

            if (tapi_sql_value_equal(v, &rows[i][j], NULL))
            {
                same++;
                continue;
            }
            ERROR("Row %u column %s: type %c, length %u", i + 1,
                  got.names[j], (char)v->type, (unsigned int)v->len);
            TEST_VERDICT("Column %s did not come back as it was bound",
                         got.names[j]);
        }
        equal[i] = (double)same;
    }

    TEST_STEP("The 64-bit integer came back exactly, not through a double");
    CHECK_RC(tapi_sql_bind(sel, 1, tapi_sql_int(1)));
    CHECK_RC(tapi_sql_stmt_execute(sel, NULL));
    CHECK_RC(tapi_sql_fetch(sel, 10, &got, &eof));
    if (tapi_sql_result_cell(&got, 0, 2)->i != 9007199254740993LL)
        TEST_VERDICT("A 64-bit integer lost precision");

    TEST_STEP("The binary value kept its length and its NUL bytes");
    {
        const tapi_sql_value *b = tapi_sql_result_cell(&got, 0, 5);

        if (b->type != TAPI_SQL_BINARY || b->len != sizeof(bytes) ||
            memcmp(b->str, bytes, sizeof(bytes)) != 0)
            TEST_VERDICT("Binary data was altered on the way");
    }

    TEST_STEP("A parameter that does not exist is refused, not ignored");
    rc = tapi_sql_bind(sel, 7, tapi_sql_int(1));
    if (rc == 0)
    {
        rc = tapi_sql_stmt_execute(sel, NULL);
        if (rc == 0)
            TEST_VERDICT("A binding beyond the parameter count was "
                         "accepted");
    }

    if (tapi_sql_has_cap(conn, TAPI_SQL_CAP_UINT64))
    {
        tapi_sql_value big = tapi_sql_uint(18446744073709551615ULL);

        TEST_STEP("The full unsigned 64-bit range, where the engine has "
                  "it");
        CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " UTABLE
                                  " (u BIGINT UNSIGNED)", NULL));
        CHECK_RC(tapi_sql_query_params(conn, "INSERT INTO " UTABLE
                                       " VALUES (?)", &big, 1, NULL, NULL));
        CHECK_RC(tapi_sql_query(conn, "SELECT u FROM " UTABLE, &got));
        if (got.nrows != 1 ||
            tapi_sql_result_cell(&got, 0, 0)->u != 18446744073709551615ULL)
            TEST_VERDICT("The largest unsigned 64-bit integer did not come "
                         "back");
    }

    tsapi_sql_chart("Values that came back as bound, per row", NULL, NULL,
                    "values", equal, NROWS);

    TEST_SUCCESS;

cleanup:
    tapi_sql_result_free(&got);
    tapi_sql_statement_free(ins);
    tapi_sql_statement_free(sel);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
