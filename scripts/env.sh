#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica Unipessoal Lda
if [ ! -d "${TS_TOPDIR}" ] ; then
    echo "TS_TOPDIR is not set" 1>&2
    exit 1
fi

if [ -d "${TS_TOPDIR}/../test-environment" ] ; then
    export TE_BASE="${TS_TOPDIR}/../test-environment"
fi
