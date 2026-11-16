/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Driver errors: a statement that does not parse, a table that does not
 * exist, a duplicate key, a dangling reference, a NULL where none is
 * allowed. Each engine words and numbers these its own way; the test
 * sees the same error class for each, with the engine's message and
 * code kept, and the connection is fit for use afterwards.
 */

#define TE_TEST_NAME    "sql/errors"

#include "te_config.h"

#include <string.h>

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define PARENT  "t_err_parent"
#define CHILD   "t_err_child"

/** A statement and the class of failure it must produce. */
typedef struct err_case {
    const char        *what;
    const char        *sql;
    tapi_sql_err_class cls;
} err_case;

static const err_case cases[] = {
    { "a syntax error", "SELEKT 1", TAPI_SQL_ERR_SYNTAX },
    { "a missing table", "SELECT * FROM t_err_absent",
      TAPI_SQL_ERR_NOT_FOUND },
    { "a missing column", "SELECT nope FROM " PARENT,
      TAPI_SQL_ERR_NOT_FOUND },
    { "a duplicate key", "INSERT INTO " PARENT " VALUES (1, 'again')",
      TAPI_SQL_ERR_CONSTRAINT },
    { "a duplicate unique value",
      "INSERT INTO " PARENT " VALUES (5, 'one')", TAPI_SQL_ERR_CONSTRAINT },
    { "a dangling reference", "INSERT INTO " CHILD " VALUES (1, 999)",
      TAPI_SQL_ERR_CONSTRAINT },
    { "a NULL in a NOT NULL column",
      "INSERT INTO " PARENT " VALUES (6, NULL)", TAPI_SQL_ERR_CONSTRAINT },
    { "deleting a referenced row", "DELETE FROM " PARENT " WHERE id = 2",
      TAPI_SQL_ERR_CONSTRAINT },
};

int
main(int argc, char **argv)
{
    static const char *const tables[] = { PARENT, CHILD };

    tsapi_sql_sess sess = { 0 };
    tapi_sql_conn *conn = NULL;
    tapi_sql_stmt *stmt = NULL;
    const char    *backend;
    double         classes[TE_ARRAY_LEN(cases)];
    unsigned int   stmts = 0;
    unsigned int   i;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_errors"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));

    TEST_STEP("Two related tables with constraints");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " PARENT " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "name VARCHAR(20) NOT NULL UNIQUE)", NULL));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " CHILD " ("
                              "id INTEGER NOT NULL PRIMARY KEY, "
                              "parent_id INTEGER NOT NULL, "
                              "FOREIGN KEY (parent_id) REFERENCES " PARENT
                              " (id))", NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " PARENT
                              " VALUES (1, 'one'), (2, 'two')", NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " CHILD " VALUES (10, 2)",
                              NULL));

    TEST_STEP("Each bad statement fails with its class");
    for (i = 0; i < TE_ARRAY_LEN(cases); i++)
    {
        const tapi_sql_error *err;

        rc = tapi_sql_execute(conn, cases[i].sql, NULL);
        err = tapi_sql_last_error(conn);
        RING("%s: rc=%r class=%s sqlstate=%s native=%d: %s", cases[i].what,
             rc, tapi_sql_err_class2str(err->cls), err->sqlstate,
             err->native, err->message);
        classes[i] = (double)err->cls;

        if (rc == 0)
            TEST_VERDICT("The statement with %s succeeded", cases[i].what);
        if (err->cls != cases[i].cls)
            TEST_VERDICT("The statement with %s failed as '%s'",
                         cases[i].what, tapi_sql_err_class2str(err->cls));
        if (err->message[0] == '\0')
            TEST_VERDICT("The failure with %s has no message",
                         cases[i].what);

        /* An error must not cost the connection. */
        CHECK_RC(tapi_sql_ping(conn));
    }

    TEST_STEP("The same through a prepared statement");
    rc = tapi_sql_prepare(conn, "SELECT * FROM t_err_absent WHERE id = ?",
                          &stmt);
    if (rc == 0)
    {
        /* SQLite and MariaDB refuse at prepare, PostgreSQL too; an
         * engine that defers must refuse at execute. */
        CHECK_RC(tapi_sql_bind(stmt, 1, tapi_sql_int(1)));
        rc = tapi_sql_stmt_execute(stmt, NULL);
    }
    if (rc == 0 || tapi_sql_last_error(conn)->cls != TAPI_SQL_ERR_NOT_FOUND)
        TEST_VERDICT("A prepared statement on a missing table did not fail "
                     "as 'not found'");
    CHECK_RC(tapi_sql_statement_free(stmt));
    stmt = NULL;

    CHECK_RC(tapi_sql_prepare(conn, "INSERT INTO " PARENT " VALUES (?, ?)",
                              &stmt));
    CHECK_RC(tapi_sql_bind(stmt, 1, tapi_sql_int(2)));
    CHECK_RC(tapi_sql_bind(stmt, 2, tapi_sql_text("clash")));
    rc = tapi_sql_stmt_execute(stmt, NULL);
    if (rc == 0 || tapi_sql_last_error(conn)->cls != TAPI_SQL_ERR_CONSTRAINT)
        TEST_VERDICT("A prepared insert of a duplicate key did not fail as "
                     "a constraint violation");

    TEST_STEP("A statement that failed can run again and succeed");
    CHECK_RC(tapi_sql_bind(stmt, 1, tapi_sql_int(3)));
    CHECK_RC(tapi_sql_stmt_execute(stmt, NULL));
    CHECK_RC(tapi_sql_statement_free(stmt));
    stmt = NULL;

    TEST_STEP("Nothing the failures touched was changed, nothing leaked");
    CHECK_RC(tapi_sql_assert_row_count(conn, PARENT, 3));
    CHECK_RC(tapi_sql_assert_row_count(conn, CHILD, 1));
    CHECK_RC(tapi_sql_stats(sess.pco, NULL, &stmts, NULL));
    if (stmts != 0)
        TEST_VERDICT("%u statement(s) leaked after the failures", stmts);

    tsapi_sql_chart("Error class reported, per bad statement", NULL, NULL,
                    "class", classes, TE_ARRAY_LEN(cases));

    TEST_SUCCESS;

cleanup:
    tapi_sql_statement_free(stmt);
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
