/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Schema introspection: tables, columns, indexes, constraints and
 * foreign keys of the same schema come back in the same structures on
 * every engine - same names, same normalised types, same key and
 * nullability flags - whatever catalogue the engine keeps them in.
 */

#define TE_TEST_NAME    "sql/schema"

#include "te_config.h"

#include <string.h>
#include <strings.h>

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define PARENT  "t_schema_dept"
#define CHILD   "t_schema_emp"
#define VIEW    "v_schema_emp"

int
main(int argc, char **argv)
{
    static const char *const tables[] = { PARENT, CHILD };
    static const tapi_sql_column_spec emp_cols[] = {
        { "id", TAPI_SQL_INT, TAPI_SQL_NO, TAPI_SQL_YES },
        { "dept_id", TAPI_SQL_INT, TAPI_SQL_NO, TAPI_SQL_NO },
        { "email", TAPI_SQL_TEXT, TAPI_SQL_NO, TAPI_SQL_NO },
        { "salary", TAPI_SQL_DECIMAL, TAPI_SQL_YES, TAPI_SQL_NO },
        { "rating", TAPI_SQL_FLOAT, TAPI_SQL_YES, TAPI_SQL_NO },
        { "hired", TAPI_SQL_DATETIME, TAPI_SQL_YES, TAPI_SQL_NO },
        { "active", TAPI_SQL_BOOL, TAPI_SQL_YES, TAPI_SQL_NO },
        { "photo", TAPI_SQL_BINARY, TAPI_SQL_YES, TAPI_SQL_NO },
    };

    tsapi_sql_sess        sess = { 0 };
    tapi_sql_conn        *conn = NULL;
    tapi_sql_table       *tabs = NULL;
    tapi_sql_column      *cols = NULL;
    tapi_sql_index       *idx = NULL;
    tapi_sql_constraint  *cons = NULL;
    tapi_sql_foreign_key *fks = NULL;
    unsigned int          ntabs = 0;
    unsigned int          ncols = 0;
    unsigned int          nidx = 0;
    unsigned int          ncons = 0;
    unsigned int          nfks = 0;
    const char           *backend;
    double                counts[5];
    unsigned int          i;
    bool                  found_parent = false;
    bool                  found_child = false;
    bool                  found_view = false;
    bool                  found_index = false;
    bool                  found_unique = false;
    bool                  found_pk = false;
    bool                  found_fk = false;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_schema"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("Create two related tables, an index and a view");
    tapi_sql_execute(conn, "DROP VIEW IF EXISTS " VIEW, NULL);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " PARENT " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "name VARCHAR(40) NOT NULL)", NULL));
    CHECK_RC(tapi_sql_execute_fmt(conn, NULL, "CREATE TABLE " CHILD " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "dept_id INTEGER NOT NULL, "
                              "email VARCHAR(80) NOT NULL, "
                              "salary DECIMAL(10,2), "
                              "rating DOUBLE PRECISION, "
                              "hired DATE, "
                              "active BOOLEAN DEFAULT TRUE, "
                              "photo %s, "
                              "CONSTRAINT emp_email_uq UNIQUE (email), "
                              "CONSTRAINT emp_dept_fk FOREIGN KEY (dept_id) "
                              "REFERENCES " PARENT " (id) "
                              "ON DELETE CASCADE)",
                              tsapi_sql_blob_type(sess.engine)));
    CHECK_RC(tapi_sql_execute(conn, "CREATE INDEX emp_hired_ix ON " CHILD
                              " (hired, dept_id)", NULL));
    CHECK_RC(tapi_sql_execute(conn, "CREATE VIEW " VIEW " AS SELECT id, "
                              "email FROM " CHILD, NULL));

    TEST_STEP("Tables and views are listed, and told apart");
    CHECK_RC(tapi_sql_tables(conn, &tabs, &ntabs));
    for (i = 0; i < ntabs; i++)
    {
        if (strcasecmp(tabs[i].name, PARENT) == 0)
            found_parent = !tabs[i].is_view;
        else if (strcasecmp(tabs[i].name, CHILD) == 0)
            found_child = !tabs[i].is_view;
        else if (strcasecmp(tabs[i].name, VIEW) == 0)
            found_view = tabs[i].is_view;
    }
    if (!found_parent || !found_child)
        TEST_VERDICT("A created table is missing from the table list");
    if (!found_view)
        TEST_VERDICT("The view is missing or not marked as a view");
    counts[0] = (double)ntabs;

    TEST_STEP("Columns come back in order with normalised types");
    CHECK_RC(tapi_sql_columns(conn, CHILD, &cols, &ncols));
    counts[1] = (double)ncols;
    if (ncols != TE_ARRAY_LEN(emp_cols))
        TEST_VERDICT("The table has %u columns in introspection, not %u",
                     ncols, (unsigned int)TE_ARRAY_LEN(emp_cols));
    for (i = 0; i < ncols; i++)
    {
        RING("column %u: %s type %c (%s) nullable=%d pk=%d default=%s",
             cols[i].position, cols[i].name, (char)cols[i].type,
             cols[i].native_type, cols[i].nullable, cols[i].primary_key,
             cols[i].default_value != NULL ? cols[i].default_value : "-");
        if (cols[i].position != i + 1 ||
            strcasecmp(cols[i].name, emp_cols[i].name) != 0)
            TEST_VERDICT("Columns are not reported in table order");
    }
    if (cols[6].default_value == NULL)
        TEST_VERDICT("A column default is not reported");
    CHECK_RC(tapi_sql_assert_schema(conn, CHILD, emp_cols,
                                    TE_ARRAY_LEN(emp_cols), true));

    TEST_STEP("Indexes: the one created, with its columns in order");
    CHECK_RC(tapi_sql_indexes(conn, CHILD, &idx, &nidx));
    counts[2] = (double)nidx;
    for (i = 0; i < nidx; i++)
    {
        RING("index %s unique=%d primary=%d (%s)", idx[i].name,
             idx[i].unique, idx[i].primary, idx[i].columns);
        if (strcasecmp(idx[i].name, "emp_hired_ix") == 0)
            found_index = !idx[i].unique && !idx[i].primary &&
                          strcasecmp(idx[i].columns, "hired,dept_id") == 0;
    }
    if (!found_index)
        TEST_VERDICT("The created index is missing or misreported");

    TEST_STEP("Constraints: primary key, unique, foreign key");
    CHECK_RC(tapi_sql_constraints(conn, CHILD, &cons, &ncons));
    counts[3] = (double)ncons;
    for (i = 0; i < ncons; i++)
    {
        RING("constraint %s kind=%d (%s)", cons[i].name, cons[i].kind,
             cons[i].columns);
        if (cons[i].kind == TAPI_SQL_CONSTRAINT_PRIMARY)
            found_pk = strcasecmp(cons[i].columns, "id") == 0;
        else if (cons[i].kind == TAPI_SQL_CONSTRAINT_UNIQUE &&
                 strcasecmp(cons[i].columns, "email") == 0)
            found_unique = true;
    }
    if (!found_pk)
        TEST_VERDICT("The primary key constraint is missing or misreported");
    if (!found_unique)
        TEST_VERDICT("The unique constraint is missing or misreported");

    TEST_STEP("Foreign keys: columns, referenced table, delete rule");
    CHECK_RC(tapi_sql_foreign_keys(conn, CHILD, &fks, &nfks));
    counts[4] = (double)nfks;
    for (i = 0; i < nfks; i++)
    {
        RING("foreign key %s (%s) -> %s (%s) on delete %s on update %s",
             fks[i].name, fks[i].columns, fks[i].ref_table,
             fks[i].ref_columns, fks[i].on_delete, fks[i].on_update);
        if (strcasecmp(fks[i].columns, "dept_id") == 0 &&
            strcasecmp(fks[i].ref_table, PARENT) == 0 &&
            strcasecmp(fks[i].ref_columns, "id") == 0 &&
            strcmp(fks[i].on_delete, "cascade") == 0)
            found_fk = true;
    }
    if (nfks != 1 || !found_fk)
        TEST_VERDICT("The foreign key is missing or misreported");

    TEST_STEP("A table that does not exist has no columns");
    tapi_sql_columns_free(cols, ncols);
    cols = NULL;
    CHECK_RC(tapi_sql_columns(conn, "t_schema_absent", &cols, &ncols));
    if (ncols != 0)
        TEST_VERDICT("A missing table reported columns");

    tsapi_sql_chart("Objects found: tables, columns, indexes, constraints, "
                    "foreign keys", NULL, NULL, "objects", counts, 5);

    TEST_SUCCESS;

cleanup:
    tapi_sql_tables_free(tabs, ntabs);
    tapi_sql_columns_free(cols, ncols);
    tapi_sql_indexes_free(idx, nidx);
    tapi_sql_constraints_free(cons, ncons);
    tapi_sql_foreign_keys_free(fks, nfks);
    if (conn != NULL)
        tapi_sql_execute(conn, "DROP VIEW IF EXISTS " VIEW, NULL);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
