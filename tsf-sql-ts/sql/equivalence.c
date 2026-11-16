/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Two engines, one answer. The same schema and the same rows are loaded
 * into two different engines through the same parameterised statements,
 * and the same queries - a filter, a join, an aggregate, an average, a
 * NULL-aware count - are run on both. The results must be equivalent:
 * the same rows in any order, numbers compared by value (one engine's
 * decimal is another's float) and within a small tolerance.
 *
 * What a migration test or a replica check does, in miniature.
 */

#define TE_TEST_NAME    "sql/equivalence"

#include "te_config.h"

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define ITEMS   "t_eq_item"
#define SALES   "t_eq_sale"

static const char *const queries[] = {
    "SELECT id, name, price FROM " ITEMS " WHERE price > 1.5",
    "SELECT i.name, s.qty FROM " SALES " s JOIN " ITEMS
    " i ON i.id = s.item_id",
    "SELECT i.name, sum(s.qty), sum(s.qty * i.price) FROM " SALES
    " s JOIN " ITEMS " i ON i.id = s.item_id GROUP BY i.name",
    "SELECT avg(price) FROM " ITEMS,
    "SELECT count(*), count(note) FROM " SALES,
    "SELECT item_id, max(qty) FROM " SALES " GROUP BY item_id "
    "HAVING max(qty) >= 5",
};

/* Create and fill the same tables on one engine. */
static void
load(tapi_sql_conn *conn)
{
    static const char *const tables[] = { ITEMS, SALES };
    static const struct { int id; const char *name; const char *price; }
    items[] = {
        { 1, "bolt", "0.10" }, { 2, "nut", "0.20" }, { 3, "gear", "12.75" },
        { 4, "shaft", "99.99" },
    };
    static const struct { int id; int item; int qty; const char *note; }
    sales[] = {
        { 1, 1, 100, "bulk" }, { 2, 1, 3, NULL }, { 3, 2, 50, NULL },
        { 4, 3, 5, "urgent" }, { 5, 3, 1, NULL }, { 6, 4, 2, "o'neil" },
    };
    tapi_sql_stmt *stmt = NULL;
    unsigned int   i;

    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " ITEMS " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "name VARCHAR(20) NOT NULL, "
                              "price DECIMAL(10,2) NOT NULL)", NULL));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " SALES " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "item_id INTEGER NOT NULL, "
                              "qty INTEGER NOT NULL, note VARCHAR(20), "
                              "FOREIGN KEY (item_id) REFERENCES " ITEMS
                              " (id))", NULL));

    CHECK_RC(tapi_sql_prepare(conn, "INSERT INTO " ITEMS " VALUES (?, ?, ?)",
                              &stmt));
    for (i = 0; i < TE_ARRAY_LEN(items); i++)
    {
        CHECK_RC(tapi_sql_bind(stmt, 1, tapi_sql_int(items[i].id)));
        CHECK_RC(tapi_sql_bind(stmt, 2, tapi_sql_text(items[i].name)));
        CHECK_RC(tapi_sql_bind(stmt, 3, tapi_sql_decimal(items[i].price)));
        CHECK_RC(tapi_sql_stmt_execute(stmt, NULL));
    }
    CHECK_RC(tapi_sql_statement_free(stmt));

    CHECK_RC(tapi_sql_prepare(conn, "INSERT INTO " SALES
                              " VALUES (?, ?, ?, ?)", &stmt));
    for (i = 0; i < TE_ARRAY_LEN(sales); i++)
    {
        CHECK_RC(tapi_sql_bind(stmt, 1, tapi_sql_int(sales[i].id)));
        CHECK_RC(tapi_sql_bind(stmt, 2, tapi_sql_int(sales[i].item)));
        CHECK_RC(tapi_sql_bind(stmt, 3, tapi_sql_int(sales[i].qty)));
        CHECK_RC(tapi_sql_bind(stmt, 4, sales[i].note != NULL ?
                               tapi_sql_text(sales[i].note) :
                               tapi_sql_null()));
        CHECK_RC(tapi_sql_stmt_execute(stmt, NULL));
    }
    CHECK_RC(tapi_sql_statement_free(stmt));
}

int
main(int argc, char **argv)
{
    static const char *const tables[] = { ITEMS, SALES };

    tsapi_sql_sess    sess = { 0 };
    tsapi_sql_sess    ref_sess = { 0 };
    tapi_sql_conn    *conn = NULL;
    tapi_sql_conn    *ref = NULL;
    tapi_sql_cmp_opts opts = TAPI_SQL_CMP_OPTS_DEFAULT;
    tapi_sql_result   res = TAPI_SQL_RESULT_INIT;
    const char       *backend;
    const char       *reference;
    double            rows[TE_ARRAY_LEN(queries)];
    unsigned int      i;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    TEST_GET_STRING_PARAM(reference);

    /* Bags of rows, numbers by value, a hair of tolerance. */
    opts.ordered = false;
    opts.abs_tol = 1e-9;
    opts.rel_tol = 1e-9;

    TEST_STEP("Open a session on each engine; skip when one is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_eq"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_init(&ref_sess, "pco_sql_eq_ref"));
    if (!tsapi_sql_sess_select(&ref_sess, reference))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));
    CHECK_RC(tsapi_sql_sess_connect(&ref_sess, TSAPI_SQL_ROLE_APP, &ref));

    TEST_STEP("Load the same data into both");
    load(conn);
    load(ref);

    TEST_STEP("The same queries give equivalent results");
    for (i = 0; i < TE_ARRAY_LEN(queries); i++)
    {
        CHECK_RC(tapi_sql_query(conn, queries[i], &res));
        tapi_sql_result_log(&res, queries[i], 10);
        rows[i] = (double)res.nrows;
        if (res.nrows == 0)
            TEST_VERDICT("A query of the comparison returned nothing");
        CHECK_RC(tapi_sql_assert_query_equal(conn, queries[i], ref,
                                             queries[i], &opts));
    }

    TEST_STEP("A change on one side only is noticed");
    CHECK_RC(tapi_sql_execute(conn, "UPDATE " SALES
                              " SET qty = qty + 1 WHERE id = 5", NULL));
    CHECK_RC(tapi_sql_query(conn, queries[2], &res));
    {
        tapi_sql_result other = TAPI_SQL_RESULT_INIT;
        bool            equal;

        CHECK_RC(tapi_sql_query(ref, queries[2], &other));
        equal = tapi_sql_result_equal(&res, &other, &opts, NULL);
        tapi_sql_result_free(&other);
        if (equal)
            TEST_VERDICT("Two different databases compared as equivalent");
    }

    tsapi_sql_chart("Rows returned, per compared query", NULL, NULL, "rows",
                    rows, TE_ARRAY_LEN(queries));

    TEST_SUCCESS;

cleanup:
    tapi_sql_result_free(&res);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tsapi_sql_drop(ref, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tapi_sql_disconnect(ref);
    tsapi_sql_sess_fini(&ref_sess);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
