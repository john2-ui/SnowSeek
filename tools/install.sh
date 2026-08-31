#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly BUILD_DIR="${SNOWSEEK_BUILD_DIR:-${PROJECT_ROOT}/build}"
readonly INSTALL_DIR="${SNOWSEEK_INSTALL_DIR:-/usr/local/bin}"
readonly SOURCE="${BUILD_DIR}/snowseek"
readonly TARGET="${INSTALL_DIR}/snowseek"

if [[ ! -x "${SOURCE}" ]]; then
        printf 'error: executable not found: %s\n' "${SOURCE}" >&2
        printf 'build SnowSeek first or set SNOWSEEK_BUILD_DIR\n' >&2
        exit 1
fi

run_install() {
        if [[ ${EUID} -eq 0 || ( -d "${INSTALL_DIR}" && -w "${INSTALL_DIR}" ) ]]; then
                "$@"
        elif command -v sudo >/dev/null 2>&1; then
                sudo "$@"
        else
                printf 'error: %s is not writable and sudo is unavailable\n' \
                        "${INSTALL_DIR}" >&2
                exit 1
        fi
}

run_install install -d "${INSTALL_DIR}"
run_install install -m 0755 "${SOURCE}" "${TARGET}"
printf 'installed: %s\n' "${TARGET}"

case ":${PATH}:" in
*:"${INSTALL_DIR}":*) ;;
*) printf 'warning: add %s to PATH to run snowseek directly\n' \
          "${INSTALL_DIR}" >&2 ;;
esac
