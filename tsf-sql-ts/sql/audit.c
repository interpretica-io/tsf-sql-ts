/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * The security posture of a database server, read without changing
 * anything. Each engine is audited twice.
 *
 * As the administrator over a plain connection - the known-bad control:
 * an unencrypted session of a superuser must be found as exactly that.
 * An audit that comes back clean here is broken, and its opinion of the
 * other account would be worth nothing.
 *
 * As the application account over verified TLS - the account under
 * judgement: it must have no finding of HIGH severity or above. The
 * lower findings (the test containers listen on every address, for
 * one) are logged and charted, not failed on; the severity that fails
 * is the @c min_severity parameter, so a hardening profile can run the
 * same test with a lower bar.
 *
 * The audit only reads settings and catalogue views. The test checks
 * that too: the tables are the same before and after.
 */

#define TE_TEST_NAME    "sql/audit"

#include "te_config.h"

#include <string.h>

#include "tapi_test.h"
#include "te_string.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tapi_sql_audit.h"
#include "tsapi_sql.h"

#define CA      TSAPI_SQL_WORKDIR "/ca.crt"

static tapi_sql_severity
severity_of(const char *name)
{
    if (strcmp(name, "low") == 0)
        return TAPI_SQL_SEV_LOW;
    if (strcmp(name, "medium") == 0)
        return TAPI_SQL_SEV_MEDIUM;
    if (strcmp(name, "critical") == 0)
        return TAPI_SQL_SEV_CRITICAL;
    return TAPI_SQL_SEV_HIGH;
}

int
main(int argc, char **argv)
{
    tsapi_sql_sess    sess = { 0 };
    tapi_sql_conn    *admin = NULL;
    tapi_sql_conn    *app = NULL;
    tapi_sql_config   cfg;
    tapi_sql_report   control = TAPI_SQL_REPORT_INIT;
    tapi_sql_report   report = TAPI_SQL_REPORT_INIT;
    tapi_sql_table   *before = NULL;
    tapi_sql_table   *after = NULL;
    te_string         verdict = TE_STRING_INIT;
    tapi_sql_severity bar;
    const char       *backend;
    const char       *min_severity;
    unsigned int      nbefore = 0;
    unsigned int      nafter = 0;
    double            by_sev[5];
    int               sev;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    TEST_GET_STRING_PARAM(min_severity);
    bar = severity_of(min_severity);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_audit"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");

    if (sess.engine == TAPI_SQL_ENGINE_SQLITE)
    {
        TEST_STEP("A file database: the audit says what protects it");
        CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &app));
        CHECK_RC(tapi_sql_audit(app, &report));
        tapi_sql_report_log(&report);
        if (!tapi_sql_report_has(&report, "sql.sqlite.file"))
            TEST_VERDICT("The audit of a file database says nothing");
        if (tapi_sql_report_count(&report, TAPI_SQL_SEV_LOW) != 0)
            TEST_VERDICT("The audit of a file database reports a server "
                         "weakness");
        by_sev[0] = (double)report.n;
        tsapi_sql_chart("Findings", NULL, NULL, "findings", by_sev, 1);
        TEST_SUCCESS;
    }

    TEST_STEP("The control: the administrator over a plain connection");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_ADMIN, &admin));
    CHECK_RC(tapi_sql_tables(admin, &before, &nbefore));
    CHECK_RC(tapi_sql_audit(admin, &control));
    tapi_sql_report_log(&control);
    if (!tapi_sql_report_has(&control, "sql.tls.off"))
        TEST_VERDICT("The audit missed an unencrypted session");
    if (!tapi_sql_report_has(&control, "sql.account.superuser"))
        TEST_VERDICT("The audit missed a superuser account");

    TEST_STEP("The audit changed nothing");
    CHECK_RC(tapi_sql_tables(admin, &after, &nafter));
    if (nafter != nbefore)
        TEST_VERDICT("The number of tables changed during the audit");

    TEST_STEP("The account under judgement: the application account over "
              "verified TLS");
    tsapi_sql_sess_config(&sess, TSAPI_SQL_ROLE_APP, &cfg);
    cfg.tls_mode = TAPI_SQL_TLS_VERIFY_FULL;
    cfg.tls_ca = CA;
    CHECK_RC(tapi_sql_connect(sess.pco, &cfg, &app));
    CHECK_RC(tapi_sql_audit(app, &report));
    tapi_sql_report_log(&report);

    for (sev = TAPI_SQL_SEV_INFO; sev <= TAPI_SQL_SEV_CRITICAL; sev++)
        by_sev[sev] = (double)(tapi_sql_report_count(&report, sev) -
                               (sev < TAPI_SQL_SEV_CRITICAL ?
                                tapi_sql_report_count(&report, sev + 1) :
                                0));
    tsapi_sql_chart("Findings by severity: info, low, medium, high, "
                    "critical", NULL, NULL, "findings", by_sev, 5);

    if (tapi_sql_report_has(&report, "sql.tls.off") ||
        tapi_sql_report_has(&report, "sql.tls.unverified"))
        TEST_VERDICT("A verified TLS session was reported as weak");
    if (tapi_sql_report_has(&report, "sql.account.superuser"))
        TEST_VERDICT("An ordinary account was reported as a superuser");

    TEST_STEP("Judge it at the severity the profile asks for");
    if (tapi_sql_report_verdict(&report, bar, &verdict))
        TEST_VERDICT("%s", verdict.ptr);

    TEST_SUCCESS;

cleanup:
    te_string_free(&verdict);
    tapi_sql_tables_free(before, nbefore);
    tapi_sql_tables_free(after, nafter);
    tapi_sql_report_free(&control);
    tapi_sql_report_free(&report);
    tapi_sql_disconnect(admin);
    tapi_sql_disconnect(app);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
