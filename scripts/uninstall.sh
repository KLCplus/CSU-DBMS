#!/usr/bin/env bash

set -euo pipefail

usage()
{
  echo "Usage: $0 --user | --system"
  echo "This removes installed CSUDB programs, product docs, examples, and completion only."
  echo "Database data directories are never removed."
}

case "${1:-}" in
  --user) PREFIX="${CSUDB_INSTALL_PREFIX:-${HOME}/.local}" ;;
  --system) PREFIX="${CSUDB_INSTALL_PREFIX:-/usr/local}" ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

rm -f "${PREFIX}/bin/csudb" "${PREFIX}/bin/csudbd"
rm -f "${PREFIX}/share/bash-completion/completions/csudb"
rm -rf "${PREFIX}/share/csudb/docs/product" "${PREFIX}/share/csudb/examples"
rmdir "${PREFIX}/share/csudb/docs" "${PREFIX}/share/csudb" 2>/dev/null || true

echo "CSUDB programs and support files were removed from ${PREFIX}."
echo "No database data was removed."
