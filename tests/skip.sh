#!/bin/sh
# usage: tests/skip.sh <test> <what is missing>
#
# Stands in for a test that cannot run on this machine: says what it lacks
# and exits 77, which meson reports as SKIP.
echo "SKIP $1: $2"
exit 77
