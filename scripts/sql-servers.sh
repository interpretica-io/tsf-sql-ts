#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica Unipessoal Lda
#
# The database servers the suite tests against: a PostgreSQL and a
# MariaDB container, started for one run and removed after it.
#
# Usage: scripts/sql-servers.sh up|down <work directory>
#
# The suite prologue runs "up" on the agent and the epilogue "down";
# the agent needs a docker client and the Docker socket (the suite's
# build container has both). With TSF_SQL_SERVERS=external nothing is
# started: the suite then expects servers set up the same way at
# TSF_SQL_PG_HOST:TSF_SQL_PG_PORT and TSF_SQL_MY_HOST:TSF_SQL_MY_PORT
# (see the README for what "the same way" is).
#
# Each server gets:
#  - TLS, with a certificate for 127.0.0.1 / localhost issued by a CA
#    made here (ca.crt in the work directory), and a second, unrelated
#    CA (rogue-ca.crt) for the test that must refuse the server;
#  - an administrator, an ordinary account that owns the test database,
#    a read-only account, and an account that is let in over TLS only.
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
pg_port="${TSF_SQL_PG_PORT:-55433}"
my_port="${TSF_SQL_MY_PORT:-53307}"
pg_image="${TSF_SQL_PG_IMAGE:-postgres:17}"
my_image="${TSF_SQL_MY_IMAGE:-mariadb:11.4}"

# Fixture credentials; keep in step with lib/tsapi_sql.h.
admin_pw='tsf-fixture-admin-7Qx'
app_pw='tsf-fixture-app-4Zr'
ro_pw='tsf-fixture-ro-9Lm'
tls_pw='tsf-fixture-tls-2Kd'

down() {
    docker rm -f "${pg_name}" "${my_name}" >/dev/null 2>&1 || true
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
SQL
    # tsf_tls is refused unless the connection is encrypted.
    cat > "${root}/docker-entrypoint-initdb.d/20-hba.sh" <<'SH'
#!/bin/sh
sed -i '1i hostnossl all tsf_tls all reject' "${PGDATA}/pg_hba.conf"
SH
    chmod 755 "${root}/docker-entrypoint-initdb.d/20-hba.sh"

    ( umask 077
      printf 'POSTGRES_USER=tsf_admin\nPOSTGRES_PASSWORD=%s\nPOSTGRES_DB=tsf\n' \
          "${admin_pw}" > "${work}/pg.env" )
    docker create --name "${pg_name}" -p "127.0.0.1:${pg_port}:5432" \
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
          "${app_pw}" >> "${work}/my.env" )
    docker create --name "${my_name}" -p "127.0.0.1:${my_port}:3306" \
        --env-file "${work}/my.env" "${my_image}" \
        --ssl-cert=/tls/server.crt --ssl-key=/tls/server.key \
        --ssl-ca=/tls/ca.crt >/dev/null
    copy_in "${root}" "${my_name}"
    docker start "${my_name}" >/dev/null
}

# Both images run their first-start set-up on a temporary server that
# does not listen on TCP, so an answer over TCP means the real one.
wait_ready() {
    for _ in $(seq 1 90) ; do
        if docker exec "${pg_name}" pg_isready -q -h 127.0.0.1 \
                >/dev/null 2>&1 &&
           docker exec "${my_name}" mariadb-admin --protocol=tcp \
                -h 127.0.0.1 ping >/dev/null 2>&1 ; then
            return 0
        fi
        sleep 2
    done
    echo "sql-servers: the servers did not come up" >&2
    docker logs --tail 30 "${pg_name}" >&2 || true
    docker logs --tail 30 "${my_name}" >&2 || true
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
        make_pki
        up_postgres
        up_mariadb
        wait_ready
        # The staged trees hold the server key and the init scripts
        # with the fixture passwords; they have done their job.
        rm -rf "${work}/pg-root" "${work}/my-root" "${work}/pki" \
               "${work}/ca.srl" \
               "${work}/pg.env" "${work}/my.env"
        echo "sql-servers: PostgreSQL on 127.0.0.1:${pg_port}," \
             "MariaDB on 127.0.0.1:${my_port}"
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
