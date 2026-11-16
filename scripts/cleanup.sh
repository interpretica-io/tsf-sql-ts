#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica Unipessoal Lda
TS_TOPDIR="$(cd "$(dirname "$(which "$0")")"/.. && pwd -P)" || exit 1
export TS_TOPDIR
cd "$TS_TOPDIR" || exit 1
sudo killall ta ta_rpcs te_tester te_logger dispatcher.sh
rm -f ta.*

