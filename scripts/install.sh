#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${CSUDB_BUILD_DIR:-${PROJECT_ROOT}/build_release}"

usage()
{
  echo "Usage: $0 --user | --system"
  echo "  --user    install below ~/.local"
  echo "  --system  install below /usr/local (normally run through sudo)"
  echo "CSUDB_BUILD_DIR and CSUDB_INSTALL_PREFIX may override the defaults."
}

case "${1:-}" in
  --user) PREFIX="${CSUDB_INSTALL_PREFIX:-${HOME}/.local}" ;;
  --system) PREFIX="${CSUDB_INSTALL_PREFIX:-/usr/local}" ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

if [[ ! -x "${BUILD_DIR}/bin/csudb" || ! -x "${BUILD_DIR}/bin/csudbd" ]]; then
  echo "CSUDB binaries were not found in ${BUILD_DIR}/bin." >&2
  echo "Build first, or set CSUDB_BUILD_DIR." >&2
  exit 1
fi

cmake --install "${BUILD_DIR}" --prefix "${PREFIX}"
echo "CSUDB installed below ${PREFIX}."
echo "Ensure ${PREFIX}/bin is present in PATH."
