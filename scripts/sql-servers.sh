#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica Unipessoal Lda
#
# The database servers the suite tests against: a PostgreSQL and a
# MariaDB container, each with a streaming replica of its own, started
# for one run and removed after it.
#
# Usage: scripts/sql-servers.sh up|down <work directory>
#
# The suite prologue runs "up" on the agent and the epilogue "down";
# the agent needs a docker client and the Docker socket (the suite's
# build container has both). With TSF_SQL_SERVERS=external nothing is
# started: the suite then expects servers set up the same way at
# TSF_SQL_PG_HOST:TSF_SQL_PG_PORT and TSF_SQL_MY_HOST:TSF_SQL_MY_PORT,
# and their replicas, if any, at TSF_SQL_PG_REPLICA_PORT and
# TSF_SQL_MY_REPLICA_PORT (see the README for what "the same way" is).
#
# Each server gets:
#  - TLS, with a certificate for 127.0.0.1 / localhost issued by a CA
#    made here (ca.crt in the work directory), and a second, unrelated
#    CA (rogue-ca.crt) for the test that must refuse the server;
#  - an administrator, an ordinary account that owns the test database,
#    a read-only account, and an account that is let in over TLS only;
#  - a replica: a second container that follows the first (PostgreSQL
#    streaming replication from a base backup; MariaDB binary-log
#    replication by GTID) and takes reads only. The accounts reach it
#    by replication. The four containers share a Docker network of
#    their own, which is how a replica finds its primary.
#
# The accounts and passwords are fixtures: they exist in containers
# that live for one run and listen on loopback only, and they are the
# same constants lib/tsapi_sql.h has. They protect nothing. They are
# still kept off every command line here (docker --env-file, an init
# script copied in), because the suite checks that the module under
# test keeps secrets off command lines and it would be odd to do worse.

set -euo pipefail

cmd="${1:-}"
work="${2:-}"
if [ -z "${cmd}" ] || [ -z "${work}" ] ; then
    echo "usage: $0 up|down <work directory>" >&2
    exit 2
fi

pg_name=tsf-sql-ts-pg
my_name=tsf-sql-ts-my
pg_replica=tsf-sql-ts-pg-replica
my_replica=tsf-sql-ts-my-replica
net_name=tsf-sql-ts-net
pg_port="${TSF_SQL_PG_PORT:-55433}"
my_port="${TSF_SQL_MY_PORT:-53307}"
pg_replica_port="${TSF_SQL_PG_REPLICA_PORT:-55434}"
my_replica_port="${TSF_SQL_MY_REPLICA_PORT:-53308}"
pg_image="${TSF_SQL_PG_IMAGE:-postgres:17}"
my_image="${TSF_SQL_MY_IMAGE:-mariadb:11.4}"

# Fixture credentials; keep in step with lib/tsapi_sql.h.
admin_pw='tsf-fixture-admin-7Qx'
app_pw='tsf-fixture-app-4Zr'
ro_pw='tsf-fixture-ro-9Lm'
tls_pw='tsf-fixture-tls-2Kd'
repl_pw='tsf-fixture-repl-6Hn'

down() {
    docker rm -f "${pg_replica}" "${my_replica}" "${pg_name}" "${my_name}" \
        >/dev/null 2>&1 || true
    docker network rm "${net_name}" >/dev/null 2>&1 || true
    rm -rf "${work}"
}

# Copy a directory tree into a container, owned by the server's user
# (uid 999 in both images): PostgreSQL refuses a private key that is
# not its own.
copy_in() {
    tar -C "$1" --owner=999 --group=999 -cf - . | docker cp -a - "$2:/"
}

make_pki() {
    local tls="${work}/pki"

    mkdir -p "${tls}"
    openssl req -x509 -newkey rsa:2048 -nodes -days 30 \
        -keyout "${tls}/ca.key" -out "${work}/ca.crt" \
        -subj "/CN=tsf-sql-ts test CA" >/dev/null 2>&1
    openssl req -x509 -newkey rsa:2048 -nodes -days 30 \
        -keyout "${tls}/rogue.key" -out "${work}/rogue-ca.crt" \
        -subj "/CN=tsf-sql-ts unrelated CA" >/dev/null 2>&1
    openssl req -newkey rsa:2048 -nodes \
        -keyout "${tls}/server.key" -out "${tls}/server.csr" \
        -subj "/CN=localhost" >/dev/null 2>&1
    printf 'subjectAltName=IP:127.0.0.1,DNS:localhost\n' > "${tls}/ext"
    openssl x509 -req -in "${tls}/server.csr" -CA "${work}/ca.crt" \
        -CAkey "${tls}/ca.key" -CAcreateserial -days 30 \
        -extfile "${tls}/ext" -out "${tls}/server.crt" >/dev/null 2>&1
    chmod 644 "${work}/ca.crt" "${work}/rogue-ca.crt"
}

# The trees copied into the containers: /tls and the init scripts the
# images run on first start.
stage() {
    local root="$1"

    mkdir -p "${root}/tls" "${root}/docker-entrypoint-initdb.d"
    cp "${work}/pki/server.crt" "${work}/pki/server.key" "${root}/tls/"
    cp "${work}/ca.crt" "${root}/tls/ca.crt"
    chmod 600 "${root}/tls/server.key"
}

up_postgres() {
    local root="${work}/pg-root"

    stage "${root}"
    cat > "${root}/docker-entrypoint-initdb.d/10-accounts.sql" <<SQL
CREATE ROLE tsf LOGIN PASSWORD '${app_pw}';
CREATE ROLE tsf_ro LOGIN PASSWORD '${ro_pw}';
CREATE ROLE tsf_tls LOGIN PASSWORD '${tls_pw}';
GRANT ALL ON DATABASE tsf TO tsf;
GRANT ALL ON SCHEMA public TO tsf;
GRANT ALL ON SCHEMA public TO tsf_tls;
CREATE ROLE tsf_repl REPLICATION LOGIN PASSWORD '${repl_pw}';
SQL
    # tsf_tls is refused unless the connection is encrypted; tsf_repl
    # may fetch the write-ahead log.
    cat > "${root}/docker-entrypoint-initdb.d/20-hba.sh" <<'SH'
#!/bin/sh
sed -i '1i hostnossl all tsf_tls all reject' "${PGDATA}/pg_hba.conf"
echo 'host replication tsf_repl all scram-sha-256' >> "${PGDATA}/pg_hba.conf"
SH
    chmod 755 "${root}/docker-entrypoint-initdb.d/20-hba.sh"

    ( umask 077
      printf 'POSTGRES_USER=tsf_admin\nPOSTGRES_PASSWORD=%s\nPOSTGRES_DB=tsf\n' \
          "${admin_pw}" > "${work}/pg.env" )
    docker create --name "${pg_name}" --network "${net_name}" \
        -p "127.0.0.1:${pg_port}:5432" \
        --env-file "${work}/pg.env" "${pg_image}" \
        -c ssl=on -c ssl_cert_file=/tls/server.crt \
        -c ssl_key_file=/tls/server.key >/dev/null
    copy_in "${root}" "${pg_name}"
    docker start "${pg_name}" >/dev/null
}

up_mariadb() {
    local root="${work}/my-root"

    stage "${root}"
    cat > "${root}/docker-entrypoint-initdb.d/10-accounts.sql" <<SQL
CREATE USER 'tsf_ro'@'%' IDENTIFIED BY '${ro_pw}';
CREATE USER 'tsf_tls'@'%' IDENTIFIED BY '${tls_pw}' REQUIRE SSL;
GRANT ALL ON tsf.* TO 'tsf_tls'@'%';
SQL

    ( umask 077
      printf 'MARIADB_ROOT_PASSWORD=%s\nMARIADB_DATABASE=tsf\n' \
          "${admin_pw}" > "${work}/my.env"
      printf 'MARIADB_USER=tsf\nMARIADB_PASSWORD=%s\n' \
          "${app_pw}" >> "${work}/my.env"
      printf 'MARIADB_REPLICATION_USER=tsf_repl\n' >> "${work}/my.env"
      printf 'MARIADB_REPLICATION_PASSWORD=%s\n' \
          "${repl_pw}" >> "${work}/my.env" )
    docker create --name "${my_name}" --network "${net_name}" \
        -p "127.0.0.1:${my_port}:3306" \
        --env-file "${work}/my.env" "${my_image}" \
        --server-id=1 --log-bin --binlog-format=ROW \
        --ssl-cert=/tls/server.crt --ssl-key=/tls/server.key \
        --ssl-ca=/tls/ca.crt >/dev/null
    copy_in "${root}" "${my_name}"
    docker start "${my_name}" >/dev/null
}

# The PostgreSQL replica: a base backup of the primary, taken over the
# replication protocol, then a server in recovery that follows it.
up_postgres_replica() {
    ( umask 077
      printf 'PGPASSWORD=%s\n' "${repl_pw}" > "${work}/pg-replica.env" )
    docker run -d --name "${pg_replica}" --network "${net_name}" \
        -p "127.0.0.1:${pg_replica_port}:5432" \
        --env-file "${work}/pg-replica.env" \
        --user postgres --entrypoint bash "${pg_image}" -c \
        "until pg_basebackup -h ${pg_name} -U tsf_repl -D \"\${PGDATA}\" \
             -R -X stream ; do sleep 1 ; done ; \
         chmod 700 \"\${PGDATA}\" ; exec postgres" >/dev/null
}

# The MariaDB replica: the image points it at the primary by itself
# (MARIADB_MASTER_HOST) and everything else arrives by replication.
up_mariadb_replica() {
    ( umask 077
      printf 'MARIADB_MASTER_HOST=%s\n' "${my_name}" > "${work}/my-replica.env"
      printf 'MARIADB_REPLICATION_USER=tsf_repl\n' >> "${work}/my-replica.env"
      printf 'MARIADB_REPLICATION_PASSWORD=%s\n' \
          "${repl_pw}" >> "${work}/my-replica.env"
      printf 'MARIADB_ROOT_PASSWORD=%s\n' \
          "${admin_pw}" >> "${work}/my-replica.env" )
    docker run -d --name "${my_replica}" --network "${net_name}" \
        -p "127.0.0.1:${my_replica_port}:3306" \
        --env-file "${work}/my-replica.env" "${my_image}" \
        --server-id=2 --log-bin --read-only=1 >/dev/null
}

# Both images run their first-start set-up on a temporary server that
# does not listen on TCP, so an answer over TCP means the real one.
wait_tcp() {
    local what="$1"
    shift

    for _ in $(seq 1 90) ; do
        if "$@" >/dev/null 2>&1 ; then
            return 0
        fi
        sleep 2
    done
    echo "sql-servers: ${what} did not come up" >&2
    return 1
}

pg_ready() {
    docker exec "$1" pg_isready -q -h 127.0.0.1
}

my_ready() {
    docker exec "$1" mariadb-admin --protocol=tcp -h 127.0.0.1 ping
}

wait_ready() {
    if wait_tcp "PostgreSQL" pg_ready "${pg_name}" &&
       wait_tcp "MariaDB" my_ready "${my_name}" ; then
        return 0
    fi
    docker logs --tail 30 "${pg_name}" >&2 || true
    docker logs --tail 30 "${my_name}" >&2 || true
    return 1
}

# The replicas are an extra: without them the replication tests skip
# and everything else runs.
wait_replicas() {
    if wait_tcp "the PostgreSQL replica" pg_ready "${pg_replica}" &&
       wait_tcp "the MariaDB replica" my_ready "${my_replica}" ; then
        touch "${work}/replicas"
        return 0
    fi
    docker logs --tail 30 "${pg_replica}" >&2 || true
    docker logs --tail 30 "${my_replica}" >&2 || true
    return 1
}

case "${cmd}" in
    up)
        if [ "${TSF_SQL_SERVERS:-}" = "external" ] ; then
            echo "sql-servers: using external servers"
            exit 0
        fi
        if ! docker version >/dev/null 2>&1 ; then
            echo "sql-servers: no usable docker here" >&2
            exit 3
        fi
        down
        mkdir -p "${work}"
        docker network create "${net_name}" >/dev/null
        make_pki
        up_postgres
        up_mariadb
        wait_ready
        up_postgres_replica
        up_mariadb_replica
        if ! wait_replicas ; then
            echo "sql-servers: no replicas; the replication tests" \
                 "will skip" >&2
        fi
        # The staged trees hold the server key and the init scripts
        # with the fixture passwords; they have done their job.
        rm -rf "${work}/pg-root" "${work}/my-root" "${work}/pki" \
               "${work}/ca.srl" \
               "${work}/pg.env" "${work}/my.env" \
               "${work}/pg-replica.env" "${work}/my-replica.env"
        echo "sql-servers: PostgreSQL on 127.0.0.1:${pg_port}" \
             "(replica ${pg_replica_port})," \
             "MariaDB on 127.0.0.1:${my_port} (replica ${my_replica_port})"
        ;;
    down)
        if [ "${TSF_SQL_SERVERS:-}" != "external" ] ; then
            down
        fi
        ;;
    *)
        echo "usage: $0 up|down <work directory>" >&2
        exit 2
        ;;
esac
