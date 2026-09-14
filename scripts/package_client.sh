#!/usr/bin/env bash
# 构建并打包 CSUDB Linux 命令行客户端（Release，静态 libstdc++/libgcc，仅依赖 libc/libm）。
# 产物：dist/csudb-client-linux-x64.tar.gz 及其 .sha256
set -euo pipefail

TOPDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$TOPDIR"

NAME="csudb-client-linux-x64"
BUILD_DIR="build_release"

echo "== 配置 Release 构建（无 ASan，静态标准库）=="
cmake -S . -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_ASAN=OFF \
  -DSTATIC_STDLIB=ON \
  -DWITH_UNIT_TESTS=OFF

echo "== 编译 csudb 客户端 =="
cmake --build "${BUILD_DIR}" --target csudb -j"$(nproc)"

if [ ! -x "${BUILD_DIR}/bin/csudb" ]; then
  echo "构建失败：未找到 ${BUILD_DIR}/bin/csudb" >&2
  exit 1
fi

echo "== 组装发布目录 dist/${NAME} =="
rm -rf "dist/${NAME}"
mkdir -p "dist/${NAME}"
cp "${BUILD_DIR}/bin/csudb" "dist/${NAME}/csudb"
cp "scripts/client_readme.md" "dist/${NAME}/README.md"
[ -f NOTICE ] && cp NOTICE "dist/${NAME}/NOTICE" || true

echo "== 打包 =="
tar -czf "dist/${NAME}.tar.gz" -C dist "${NAME}"
( cd dist && sha256sum "${NAME}.tar.gz" > "${NAME}.tar.gz.sha256" )

echo
echo "完成："
ls -lh "dist/${NAME}.tar.gz" "dist/${NAME}.tar.gz.sha256"
echo "分发给他人：下载 tar.gz → 解压 → chmod +x csudb → ./csudb --url <host:port> -u root -p"
