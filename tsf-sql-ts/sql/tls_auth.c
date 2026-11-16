/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * TLS and authentication, and their failures. The right password over
 * an encrypted, verified connection works. A wrong password, an unknown
 * account, a certificate from a CA the client does not trust, a plain
 * connection for an account that is let in over TLS only, a port
 * nobody listens on: each is refused, and each refusal says which kind
 * it is - authentication, TLS, or no server - so a test can tell a
 * broken lab from a correct rejection.
 */

#define TE_TEST_NAME    "sql/tls_auth"

#include "te_config.h"

#include <string.h>

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define CA          TSAPI_SQL_WORKDIR "/ca.crt"
#define ROGUE_CA    TSAPI_SQL_WORKDIR "/rogue-ca.crt"

/* Try to connect; the class of the refusal, or NONE when let in. */
static tapi_sql_err_class
attempt(rcf_rpc_server *pco, const tapi_sql_config *cfg, const char *what,
        bool *encrypted)
{
    tapi_sql_conn *conn = NULL;
    tapi_sql_error err;
    tapi_sql_caps  caps;
    te_errno       rc;

    memset(&err, 0, sizeof(err));
    rc = tapi_sql_connect_err(pco, cfg, &conn, &err);
    if (rc != 0)
    {
        RING("%s: refused as '%s': %s", what,
             tapi_sql_err_class2str(err.cls), err.message);
        if (cfg->password != NULL && cfg->password[0] != '\0' &&
            strstr(err.message, cfg->password) != NULL)
            TEST_VERDICT("A refusal quotes the password that was tried");
        return err.cls;
    }

    if (encrypted != NULL && tapi_sql_capabilities(conn, &caps) == 0)
        *encrypted = caps.tls;
    RING("%s: connected", what);
    tapi_sql_disconnect(conn);
    return TAPI_SQL_ERR_NONE;
}

int
main(int argc, char **argv)
{
    tsapi_sql_sess     sess = { 0 };
    tapi_sql_config    cfg;
    tapi_sql_err_class cls;
    const char        *backend;
    bool               encrypted = false;
    double             outcome[8];
    unsigned int       n = 0;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_tls"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    if (sess.engine == TAPI_SQL_ENGINE_SQLITE)
        TEST_SKIP("The engine has neither accounts nor a network");

    TEST_STEP("The right password, no TLS: let in, not encrypted");
    tsapi_sql_sess_config(&sess, TSAPI_SQL_ROLE_APP, &cfg);
    encrypted = true;
    cls = attempt(sess.pco, &cfg, "plain", &encrypted);
    if (cls != TAPI_SQL_ERR_NONE)
        TEST_VERDICT("The right credentials were refused");
    if (encrypted)
        TEST_VERDICT("A connection with TLS disabled is encrypted");
    outcome[n++] = (double)cls;

    TEST_STEP("TLS required: let in, encrypted");
    cfg.tls_mode = TAPI_SQL_TLS_REQUIRE;
    encrypted = false;
    cls = attempt(sess.pco, &cfg, "tls=require", &encrypted);
    if (cls != TAPI_SQL_ERR_NONE)
        TEST_VERDICT("The server refused an encrypted connection");
    if (!encrypted)
        TEST_VERDICT("A connection that requires TLS is not encrypted");
    outcome[n++] = (double)cls;

    TEST_STEP("TLS with the server's CA and host name verified: let in");
    cfg.tls_mode = TAPI_SQL_TLS_VERIFY_FULL;
    cfg.tls_ca = CA;
    encrypted = false;
    cls = attempt(sess.pco, &cfg, "tls=verify-full", &encrypted);
    if (cls != TAPI_SQL_ERR_NONE || !encrypted)
        TEST_VERDICT("A verified TLS connection was not established");
    outcome[n++] = (double)cls;

    TEST_STEP("TLS verified against an unrelated CA: refused, as TLS");
    cfg.tls_mode = TAPI_SQL_TLS_VERIFY_CA;
    cfg.tls_ca = ROGUE_CA;
    cls = attempt(sess.pco, &cfg, "tls=verify-ca, wrong CA", NULL);
    if (cls == TAPI_SQL_ERR_NONE)
        TEST_VERDICT("A server certificate from an untrusted CA was "
                     "accepted");
    if (cls != TAPI_SQL_ERR_TLS)
        TEST_VERDICT("An untrusted server certificate was refused as '%s'",
                     tapi_sql_err_class2str(cls));
    outcome[n++] = (double)cls;

    TEST_STEP("A wrong password: refused, as authentication");
    tsapi_sql_sess_config(&sess, TSAPI_SQL_ROLE_APP, &cfg);
    cfg.password = "not-the-password-51Xw";
    cls = attempt(sess.pco, &cfg, "wrong password", NULL);
    if (cls == TAPI_SQL_ERR_NONE)
        TEST_VERDICT("A wrong password was accepted");
    if (cls != TAPI_SQL_ERR_AUTH)
        TEST_VERDICT("A wrong password was refused as '%s'",
                     tapi_sql_err_class2str(cls));
    outcome[n++] = (double)cls;

    TEST_STEP("An account that does not exist: refused, as "
              "authentication");
    tsapi_sql_sess_config(&sess, TSAPI_SQL_ROLE_APP, &cfg);
    cfg.user = "tsf_nobody";
    cls = attempt(sess.pco, &cfg, "unknown account", NULL);
    if (cls != TAPI_SQL_ERR_AUTH)
        TEST_VERDICT("An unknown account was %s",
                     cls == TAPI_SQL_ERR_NONE ? "let in" :
                     "refused as something other than authentication");
    outcome[n++] = (double)cls;

    TEST_STEP("The TLS-only account: let in encrypted, refused in the "
              "clear");
    tsapi_sql_sess_config(&sess, TSAPI_SQL_ROLE_TLS, &cfg);
    encrypted = false;
    cls = attempt(sess.pco, &cfg, "TLS-only account over TLS", &encrypted);
    if (cls != TAPI_SQL_ERR_NONE || !encrypted)
        TEST_VERDICT("The TLS-only account could not connect over TLS");
    cfg.tls_mode = TAPI_SQL_TLS_DISABLE;
    cls = attempt(sess.pco, &cfg, "TLS-only account in the clear", NULL);
    if (cls == TAPI_SQL_ERR_NONE)
        TEST_VERDICT("The TLS-only account was let in without TLS");
    if (cls != TAPI_SQL_ERR_AUTH && cls != TAPI_SQL_ERR_TLS)
        TEST_VERDICT("The TLS-only account without TLS was refused as '%s'",
                     tapi_sql_err_class2str(cls));
    outcome[n++] = (double)cls;

    TEST_STEP("A port nobody listens on: refused, as no server");
    tsapi_sql_sess_config(&sess, TSAPI_SQL_ROLE_APP, &cfg);
    cfg.port = 1;
    cfg.timeout_ms = 3000;
    cls = attempt(sess.pco, &cfg, "closed port", NULL);
    if (cls != TAPI_SQL_ERR_CONNECT)
        TEST_VERDICT("A closed port was reported as '%s'",
                     tapi_sql_err_class2str(cls));
    outcome[n++] = (double)cls;

    tsapi_sql_chart("Outcome class per attempt (0 = connected)", NULL, NULL,
                    "class", outcome, n);

    TEST_SUCCESS;

cleanup:
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
