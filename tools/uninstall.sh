#!/usr/bin/env bash

set -Eeuo pipefail

readonly INSTALL_DIR="${SNOWSEEK_INSTALL_DIR:-/usr/local/bin}"
readonly TARGET="${INSTALL_DIR}/snowseek"

if [[ ! -e "${TARGET}" ]]; then
        printf 'not installed: %s\n' "${TARGET}"
        exit 0
fi
if [[ ! -f "${TARGET}" ]]; then
        printf 'error: refusing to remove non-file target: %s\n' "${TARGET}" >&2
        exit 1
fi

if [[ ${EUID} -eq 0 || -w "${INSTALL_DIR}" ]]; then
        rm -f -- "${TARGET}"
elif command -v sudo >/dev/null 2>&1; then
        sudo rm -f -- "${TARGET}"
else
        printf 'error: %s is not writable and sudo is unavailable\n' \
                "${INSTALL_DIR}" >&2
        exit 1
fi

printf 'removed: %s\n' "${TARGET}"
