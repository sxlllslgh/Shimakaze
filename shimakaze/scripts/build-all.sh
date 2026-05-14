#!/usr/bin/env sh
set -eu

workflow="${1:-Shimakaze Build}"
gh workflow run "$workflow"

if [ "${2:-}" = "--watch" ]; then
    sleep 3
    gh run watch
fi
