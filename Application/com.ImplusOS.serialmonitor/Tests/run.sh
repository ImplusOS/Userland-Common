#!/bin/sh
# Builds and runs the LogRing host harness with the host compiler.
set -e
cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -g -O1 \
   -o /tmp/logring_test logring_test.c ../LogRing.c
exec /tmp/logring_test
