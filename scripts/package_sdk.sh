#!/usr/bin/env bash
# 打包 CSUDB SDK（Python 驱动 + Java JDBC 驱动）供外部项目使用。
# 产物：dist/csudb-python-sdk-<ver>.tar.gz、dist/csudb-jdbc-<ver>.jar、dist/SDK-README.md、dist/SDK-sha256.txt
set -euo pipefail

TOPDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$TOPDIR"

VER="2026.1.0"
PY_NAME="csudb-python-sdk-${VER}"
JDBC_JAR="csudb-jdbc-${VER}.jar"

echo "== Python SDK =="
rm -rf "dist/sdk/${PY_NAME}"
mkdir -p "dist/sdk/${PY_NAME}"
cp sdk/python/csudb.py sdk/python/__init__.py sdk/python/README.md "dist/sdk/${PY_NAME}/"
tar -czf "dist/${PY_NAME}.tar.gz" -C dist/sdk "${PY_NAME}"

echo "== Java JDBC =="
if command -v javac >/dev/null 2>&1; then
  bash sdk/java/build.sh
  cp "sdk/java/build/${JDBC_JAR}" "dist/${JDBC_JAR}"
else
  echo "警告：未找到 javac（需 JDK 17+），跳过 Java JDBC 构建" >&2
fi

echo "== 校验 =="
cp scripts/sdk_readme.md dist/SDK-README.md
: > dist/SDK-sha256.txt
( cd dist && sha256sum "${PY_NAME}.tar.gz" >> SDK-sha256.txt )
[ -f "dist/${JDBC_JAR}" ] && ( cd dist && sha256sum "${JDBC_JAR}" >> SDK-sha256.txt ) || true

echo
echo "完成："
ls -lh dist/*.tar.gz dist/*.jar dist/SDK-README.md dist/SDK-sha256.txt 2>/dev/null
