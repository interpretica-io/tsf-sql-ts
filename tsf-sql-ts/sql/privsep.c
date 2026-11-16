/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Privilege separation and authentication, as an audit with controls.
 *
 * A restricted account is granted one table to read and nothing else.
 * It must be refused everything beyond that - reading another table,
 * writing to its own, dropping it - and each refusal must be a
 * permission error. The control is the administrator doing the same
 * things: there the audit must report that nothing was refused.
 *
 * Credentials that must not work - a wrong password, an empty one - are
 * tried and must be refused. The control is the right password hidden
 * in the same list: the audit must notice that one was accepted.
 *
 * Every attempted statement runs inside a transaction that is rolled
 * back; the tables are checked to be as they were.
 */

#define TE_TEST_NAME    "sql/privsep"

#include "te_config.h"

#include <string.h>

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tapi_sql_audit.h"
#include "tsapi_sql.h"

#define OPEN_T      "t_ps_public"
#define SECRET_T    "t_ps_salaries"

/** A thing the restricted account must not be able to do. */
typedef struct forbidden {
    const char *what;
    const char *sql;
} forbidden;

static const forbidden attempts[] = {
    { "read a table it was not granted", "SELECT * FROM " SECRET_T },
    { "insert into a table it may only read",
      "INSERT INTO " OPEN_T " VALUES (99, 'intruder')" },
    { "change a table it may only read",
      "UPDATE " OPEN_T " SET note = 'defaced'" },
    { "delete from a table it may only read", "DELETE FROM " OPEN_T },
    { "create a table", "CREATE TABLE t_ps_mine (id INTEGER)" },
};

int
main(int argc, char **argv)
{
    static const char *const tables[] = { OPEN_T, SECRET_T };
    static const char *const bad_pw[] = { "", "password", "tsf" };
    static const char *const with_good[] = { "", TSAPI_SQL_RO_PW };

    tsapi_sql_sess  sess = { 0 };
    tapi_sql_conn  *admin = NULL;
    tapi_sql_conn  *ro = NULL;
    tapi_sql_config cfg;
    tapi_sql_report report = TAPI_SQL_REPORT_INIT;
    tapi_sql_report control = TAPI_SQL_REPORT_INIT;
    const char     *backend;
    int64_t         n = 0;
    double          refused[TE_ARRAY_LEN(attempts)];
    unsigned int    i;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_privsep"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    if (sess.engine == TAPI_SQL_ENGINE_SQLITE)
        TEST_SKIP("The engine has no accounts to separate");

    TEST_STEP("The administrator sets up two tables and grants one");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_ADMIN, &admin));
    tsapi_sql_drop(admin, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(admin, "CREATE TABLE " OPEN_T
                              " (id INTEGER PRIMARY KEY, note VARCHAR(20))",
                              NULL));
    CHECK_RC(tapi_sql_execute(admin, "CREATE TABLE " SECRET_T
                              " (id INTEGER PRIMARY KEY, amount INTEGER)",
                              NULL));
    CHECK_RC(tapi_sql_execute(admin, "INSERT INTO " OPEN_T
                              " VALUES (1, 'hello')", NULL));
    CHECK_RC(tapi_sql_execute(admin, "INSERT INTO " SECRET_T
                              " VALUES (1, 100000)", NULL));
    CHECK_RC(tapi_sql_execute_fmt(admin, NULL, "GRANT SELECT ON " OPEN_T
                                  " TO %s", sess.engine ==
                                  TAPI_SQL_ENGINE_POSTGRES ? "tsf_ro" :
                                  "'tsf_ro'@'%'"));

    TEST_STEP("The restricted account can do what it was granted");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_RO, &ro));
    CHECK_RC(tapi_sql_query_int(ro, "SELECT count(*) FROM " OPEN_T, &n));
    if (n != 1)
        TEST_VERDICT("The restricted account cannot read its own table");

    TEST_STEP("...and is refused everything else");
    for (i = 0; i < TE_ARRAY_LEN(attempts); i++)
    {
        unsigned int before = report.n;

        CHECK_RC(tapi_sql_audit_denied(ro, attempts[i].sql, attempts[i].what,
                                       &report));
        refused[i] = report.n == before ? 1.0 : 0.0;
    }
    tapi_sql_report_log(&report);
    if (tapi_sql_report_has(&report, "sql.privsep.allowed"))
        TEST_VERDICT("The restricted account was allowed something it was "
                     "not granted");
    if (tapi_sql_report_has(&report, "sql.privsep.not-assessed"))
        TEST_VERDICT("An attempt failed for a reason other than "
                     "permission, so it proves nothing");

    TEST_STEP("The control: the administrator is refused nothing, and the "
              "audit says so");
    for (i = 0; i < TE_ARRAY_LEN(attempts); i++)
        CHECK_RC(tapi_sql_audit_denied(admin, attempts[i].sql,
                                       attempts[i].what, &control));
    if (tapi_sql_report_count(&control, TAPI_SQL_SEV_HIGH) !=
        TE_ARRAY_LEN(attempts))
        TEST_VERDICT("The audit did not report every statement the "
                     "administrator was allowed");

    TEST_STEP("None of the attempts left a trace");
    CHECK_RC(tapi_sql_assert_row_count(admin, OPEN_T, 1));
    CHECK_RC(tapi_sql_assert_row_count(admin, OPEN_T
                                       " WHERE note = 'hello'", 1));
    CHECK_RC(tapi_sql_assert_row_count(admin, SECRET_T, 1));
    tapi_sql_execute(admin, "DROP TABLE IF EXISTS t_ps_mine", NULL);

    TEST_STEP("Passwords that must not work are refused");
    tapi_sql_report_free(&report);
    tsapi_sql_sess_config(&sess, TSAPI_SQL_ROLE_RO, &cfg);
    CHECK_RC(tapi_sql_audit_auth(sess.pco, &cfg, bad_pw,
                                 TE_ARRAY_LEN(bad_pw), &report));
    tapi_sql_report_log(&report);
    if (tapi_sql_report_has(&report, "sql.auth.accepted"))
        TEST_VERDICT("A password that must not work was accepted");
    if (tapi_sql_report_has(&report, "sql.auth.not-assessed"))
        TEST_VERDICT("A password could not be tried, so it proves nothing");

    TEST_STEP("The control: the right password in the list is noticed");
    tapi_sql_report_free(&control);
    CHECK_RC(tapi_sql_audit_auth(sess.pco, &cfg, with_good,
                                 TE_ARRAY_LEN(with_good), &control));
    if (!tapi_sql_report_has(&control, "sql.auth.accepted"))
        TEST_VERDICT("The audit did not notice an accepted password");
    for (i = 0; i < control.n; i++)
    {
        if (strstr(control.items[i].detail, TSAPI_SQL_RO_PW) != NULL)
            TEST_VERDICT("A finding quotes the password that was accepted");
    }

    tsapi_sql_chart("Forbidden statements refused, per attempt", NULL, NULL,
                    "refused", refused, TE_ARRAY_LEN(attempts));

    TEST_SUCCESS;

cleanup:
    tapi_sql_report_free(&report);
    tapi_sql_report_free(&control);
    tapi_sql_disconnect(ro);
    tsapi_sql_drop(admin, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(admin);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
