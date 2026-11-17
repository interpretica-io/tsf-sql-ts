/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Test Suite library: what every tsf-sql test needs
 */

#define TE_LGR_USER     "TSAPI SQL"

#include "te_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_defs.h"
#include "te_string.h"
#include "te_mi_log.h"
#include "tapi_rpc_stdio.h"

#include "tsapi_sql.h"

/** How long starting two database containers may take. */
#define SERVERS_TIMEOUT_MS  600000

/* A setting from the environment of the run, or its default. */
static const char *
setting(const char *name, const char *def)
{
    const char *value = getenv(name);

    return (value != NULL && *value != '\0') ? value : def;
}

/* Run a shell command on the agent; its exit status, or -1. */
static int
agent_sh(rcf_rpc_server *pco, unsigned int timeout_ms, const char *cmd)
{
    rpc_wait_status st;

    pco->timeout = timeout_ms;
    RPC_AWAIT_IUT_ERROR(pco);
    st = rpc_system(pco, cmd);
    if (st.flag != RPC_WAIT_STATUS_EXITED)
        return -1;
    return (int)st.value;
}

/* See description in tsapi_sql.h */
te_errno
tsapi_sql_sess_init(tsapi_sql_sess *sess, const char *name)
{
    te_errno rc;

    memset(sess, 0, sizeof(*sess));
    sess->ta = TSAPI_SQL_TA;
    snprintf(sess->file, sizeof(sess->file), "/tmp/tsf-sql-ts-%s.sqlite",
             name);

    rc = rcf_rpc_server_create(sess->ta, name, &sess->pco);
    if (rc != 0)
    {
        ERROR("Cannot create the RPC server '%s' on %s: %r", name, sess->ta,
              rc);
        return rc;
    }
    return 0;
}

/* See description in tsapi_sql.h */
bool
tsapi_sql_sess_select(tsapi_sql_sess *sess, const char *backend)
{
    te_string cmd = TE_STRING_INIT;
    int       status;

    if (tapi_sql_engine_from_str(backend, &sess->engine) != 0)
    {
        ERROR("Unknown backend '%s'", backend);
        return false;
    }
    if (!tapi_sql_supports(sess->pco, sess->engine))
    {
        WARN("The agent has no %s driver", backend);
        return false;
    }

    if (sess->engine == TAPI_SQL_ENGINE_SQLITE)
    {
        /* A fresh database for every test. */
        te_string_append(&cmd, "rm -f %s %s-journal %s-wal %s-shm",
                         sess->file, sess->file, sess->file, sess->file);
        agent_sh(sess->pco, 10000, cmd.ptr);
        te_string_free(&cmd);
        return true;
    }

    /* The prologue leaves the CA certificate behind when it has
     * started the servers; external servers are taken on trust. */
    if (strcmp(setting("TSF_SQL_SERVERS", ""), "external") == 0)
        return true;
    te_string_append(&cmd, "test -f %s/ca.crt", TSAPI_SQL_WORKDIR);
    status = agent_sh(sess->pco, 10000, cmd.ptr);
    te_string_free(&cmd);
    if (status != 0)
        WARN("The suite has no %s server (the prologue could not start "
             "one)", backend);
    return status == 0;
}

/* See description in tsapi_sql.h */
void
tsapi_sql_sess_config(const tsapi_sql_sess *sess, tsapi_sql_role role,
                      tapi_sql_config *cfg)
{
    bool pg = sess->engine == TAPI_SQL_ENGINE_POSTGRES;

    memset(cfg, 0, sizeof(*cfg));
    cfg->engine = sess->engine;
    cfg->timeout_ms = 8000;

    if (sess->engine == TAPI_SQL_ENGINE_SQLITE)
    {
        cfg->dbname = sess->file;
        return;
    }

    cfg->host = setting(pg ? "TSF_SQL_PG_HOST" : "TSF_SQL_MY_HOST",
                        "127.0.0.1");
    cfg->port = atoi(setting(pg ? "TSF_SQL_PG_PORT" : "TSF_SQL_MY_PORT",
                             pg ? "55433" : "53307"));
    cfg->dbname = "tsf";
    cfg->tls_mode = TAPI_SQL_TLS_DISABLE;

    switch (role)
    {
        case TSAPI_SQL_ROLE_ADMIN:
            cfg->user = pg ? "tsf_admin" : "root";
            cfg->password = TSAPI_SQL_ADMIN_PW;
            break;
        case TSAPI_SQL_ROLE_RO:
            cfg->user = "tsf_ro";
            cfg->password = TSAPI_SQL_RO_PW;
            break;
        case TSAPI_SQL_ROLE_TLS:
            cfg->user = "tsf_tls";
            cfg->password = TSAPI_SQL_TLS_PW;
            cfg->tls_mode = TAPI_SQL_TLS_REQUIRE;
            break;
        default:
            cfg->user = "tsf";
            cfg->password = TSAPI_SQL_APP_PW;
            break;
    }
}

/* See description in tsapi_sql.h */
bool
tsapi_sql_sess_has_replica(const tsapi_sql_sess *sess)
{
    bool pg = sess->engine == TAPI_SQL_ENGINE_POSTGRES;

    if (sess->engine == TAPI_SQL_ENGINE_SQLITE)
        return false;

    /* External servers: a replica is there when its port is named. */
    if (strcmp(setting("TSF_SQL_SERVERS", ""), "external") == 0)
        return *setting(pg ? "TSF_SQL_PG_REPLICA_PORT" :
                             "TSF_SQL_MY_REPLICA_PORT", "") != '\0';

    return agent_sh(sess->pco, 10000,
                    "test -f " TSAPI_SQL_WORKDIR "/replicas") == 0;
}

/* See description in tsapi_sql.h */
void
tsapi_sql_sess_replica_config(const tsapi_sql_sess *sess,
                              tsapi_sql_role role, tapi_sql_config *cfg)
{
    bool pg = sess->engine == TAPI_SQL_ENGINE_POSTGRES;

    tsapi_sql_sess_config(sess, role, cfg);
    cfg->port = atoi(setting(pg ? "TSF_SQL_PG_REPLICA_PORT" :
                                  "TSF_SQL_MY_REPLICA_PORT",
                             pg ? "55434" : "53308"));
    cfg->tls_mode = TAPI_SQL_TLS_DISABLE;
}

/* See description in tsapi_sql.h */
te_errno
tsapi_sql_sess_connect(const tsapi_sql_sess *sess, tsapi_sql_role role,
                       tapi_sql_conn **conn)
{
    tapi_sql_config cfg;

    tsapi_sql_sess_config(sess, role, &cfg);
    return tapi_sql_connect(sess->pco, &cfg, conn);
}

/* See description in tsapi_sql.h */
void
tsapi_sql_sess_fini(tsapi_sql_sess *sess)
{
    te_string cmd = TE_STRING_INIT;

    if (sess->pco == NULL)
        return;

    if (sess->engine == TAPI_SQL_ENGINE_SQLITE)
    {
        te_string_append(&cmd, "rm -f %s %s-journal %s-wal %s-shm",
                         sess->file, sess->file, sess->file, sess->file);
        agent_sh(sess->pco, 10000, cmd.ptr);
        te_string_free(&cmd);
    }
    rcf_rpc_server_destroy(sess->pco);
    sess->pco = NULL;
}

/* See description in tsapi_sql.h */
const char *
tsapi_sql_blob_type(tapi_sql_engine engine)
{
    return engine == TAPI_SQL_ENGINE_POSTGRES ? "BYTEA" : "BLOB";
}

/* See description in tsapi_sql.h */
tapi_sql_isolation
tsapi_sql_level(const char *name)
{
    if (strcmp(name, "read_uncommitted") == 0)
        return TAPI_SQL_ISO_READ_UNCOMMITTED;
    if (strcmp(name, "read_committed") == 0)
        return TAPI_SQL_ISO_READ_COMMITTED;
    if (strcmp(name, "repeatable_read") == 0)
        return TAPI_SQL_ISO_REPEATABLE_READ;
    if (strcmp(name, "serializable") == 0)
        return TAPI_SQL_ISO_SERIALIZABLE;
    return TAPI_SQL_ISO_DEFAULT;
}

/* See description in tsapi_sql.h */
void
tsapi_sql_drop(tapi_sql_conn *conn, const char *const *tables,
               unsigned int n)
{
    unsigned int i;

    if (conn == NULL)
        return;
    for (i = n; i > 0; i--)
        tapi_sql_execute_fmt(conn, NULL, "DROP TABLE IF EXISTS %s",
                             tables[i - 1]);
}

/* See description in tsapi_sql.h */
te_errno
tsapi_sql_digits(tapi_sql_conn *conn, const char *table)
{
    te_errno rc;

    rc = tapi_sql_execute_fmt(conn, NULL, "DROP TABLE IF EXISTS %s", table);
    if (rc == 0)
        rc = tapi_sql_execute_fmt(conn, NULL,
                                  "CREATE TABLE %s (d INTEGER PRIMARY KEY)",
                                  table);
    if (rc == 0)
        rc = tapi_sql_execute_fmt(conn, NULL,
                                  "INSERT INTO %s VALUES (0), (1), (2), "
                                  "(3), (4), (5), (6), (7), (8), (9)",
                                  table);
    return rc;
}

/* See description in tsapi_sql.h */
void
tsapi_sql_slow_query(const char *table, te_string *sql)
{
    char alias;

    te_string_append(sql, "SELECT count(*) FROM ");
    for (alias = 'a'; alias <= 'i'; alias++)
        te_string_append(sql, "%s%s %c", alias == 'a' ? "" : ", ", table,
                         alias);
}

/* The command that runs scripts/sql-servers.sh on the agent. */
static te_errno
servers_cmd(const char *action, te_string *cmd)
{
    static const char *const vars[] = {
        "TSF_SQL_SERVERS", "TSF_SQL_PG_PORT", "TSF_SQL_MY_PORT",
        "TSF_SQL_PG_REPLICA_PORT", "TSF_SQL_MY_REPLICA_PORT",
        "TSF_SQL_PG_IMAGE", "TSF_SQL_MY_IMAGE",
    };
    const char              *top = getenv("TS_TOPDIR");
    size_t                   i;

    if (top == NULL || *top == '\0')
    {
        ERROR("TS_TOPDIR is not set: cannot find scripts/sql-servers.sh");
        return TE_RC(TE_TAPI, TE_ENOENT);
    }

    /* The agent does not inherit the run's environment; the settings
     * that matter (none of them a secret) are handed over. */
    for (i = 0; i < TE_ARRAY_LEN(vars); i++)
    {
        const char *value = getenv(vars[i]);

        if (value != NULL && *value != '\0')
            te_string_append(cmd, "%s='%s' ", vars[i], value);
    }
    te_string_append(cmd, "bash %s/scripts/sql-servers.sh %s %s", top, action,
                     TSAPI_SQL_WORKDIR);
    return 0;
}

/* See description in tsapi_sql.h */
te_errno
tsapi_sql_servers_up(rcf_rpc_server *pco)
{
    te_string cmd = TE_STRING_INIT;
    te_errno  rc;
    int       status;

    rc = servers_cmd("up", &cmd);
    if (rc != 0)
        return rc;

    status = agent_sh(pco, SERVERS_TIMEOUT_MS, cmd.ptr);
    te_string_free(&cmd);
    if (status == 0)
        return 0;
    if (status == 3)
    {
        WARN("The agent has no usable docker: the PostgreSQL and MariaDB "
             "tests will skip");
        return TE_RC(TE_TAPI, TE_EOPNOTSUPP);
    }
    ERROR("Starting the database servers failed (status %d)", status);
    return TE_RC(TE_TAPI, TE_EFAIL);
}

/* See description in tsapi_sql.h */
te_errno
tsapi_sql_servers_down(rcf_rpc_server *pco)
{
    te_string cmd = TE_STRING_INIT;
    te_errno  rc;
    int       status;

    rc = servers_cmd("down", &cmd);
    if (rc != 0)
        return rc;

    status = agent_sh(pco, SERVERS_TIMEOUT_MS, cmd.ptr);
    te_string_free(&cmd);
    return status == 0 ? 0 : TE_RC(TE_TAPI, TE_EFAIL);
}

/* See description in tsapi_sql.h */
void
tsapi_sql_chart(const char *title, const char *xname, const double *xs,
                const char *yname, const double *ys, unsigned int n)
{
    te_mi_logger *logger = NULL;
    unsigned int  i;

    if (n == 0 || te_mi_logger_meas_create("tsf-sql", &logger) != 0)
        return;

    for (i = 0; i < n; i++)
    {
        if (xs != NULL)
            te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_UNITLESS_VALUE,
                                  xname, TE_MI_MEAS_AGGR_SINGLE, xs[i],
                                  TE_MI_MEAS_MULTIPLIER_PLAIN);
        te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_UNITLESS_VALUE, yname,
                              TE_MI_MEAS_AGGR_SINGLE, ys[i],
                              TE_MI_MEAS_MULTIPLIER_PLAIN);
    }

    te_mi_logger_add_meas_view(logger, NULL, TE_MI_MEAS_VIEW_LINE_GRAPH,
                               "graph", title);
    te_mi_logger_meas_graph_axis_add_name(logger, NULL,
                               TE_MI_MEAS_VIEW_LINE_GRAPH, "graph",
                               TE_MI_GRAPH_AXIS_X,
                               xs != NULL ? xname : TE_MI_GRAPH_AUTO_SEQNO);
    te_mi_logger_meas_graph_axis_add(logger, NULL,
                               TE_MI_MEAS_VIEW_LINE_GRAPH, "graph",
                               TE_MI_GRAPH_AXIS_Y,
                               TE_MI_MEAS_UNITLESS_VALUE, yname);
    te_mi_logger_destroy(logger);
}
