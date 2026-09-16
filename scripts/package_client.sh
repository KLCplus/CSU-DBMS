#!/usr/bin/env bash
# ------------------------------------------------------------------------------------------------
# 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
# ------------------------------------------------------------------------------------------------
#  package_client.sh:28       TOPDIR
#  package_client.sh:32       NAME
#  package_client.sh:33       BUILD_DIR
#  package_client.sh:35       配置 Release 构建（无 ASan，静态标准库）
#  package_client.sh:43       编译 csudb 客户端
#  package_client.sh:53       组装发布目录 dist/${NAME}
#  package_client.sh:61       打包
# ------------------------------------------------------------------------------------------------
# 构建并打包 CSUDB Linux 命令行客户端（Release，静态 libstdc++/libgcc，仅依赖 libc/libm）。
# 产物：dist/csudb-client-linux-x64.tar.gz 及其 .sha256
#
# 输入：
#   - 仓库根目录的 CMake 工程与 csudb 客户端源码
#   - scripts/client_readme.md（随包发布的说明文档）
#   - 可选的 NOTICE 文件
# 输出：
#   - dist/csudb-client-linux-x64/       解压后的发布目录
#   - dist/csudb-client-linux-x64.tar.gz 最终分发包
#   - dist/csudb-client-linux-x64.tar.gz.sha256 校验值
# 依赖：cmake、支持 -j 的构建工具（make/ninja）以及 nproc。
set -euo pipefail

# 切换到仓库根目录：以脚本自身位置为基准解析，使相对路径稳定。
TOPDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$TOPDIR"

# 发布包名与独立构建目录（与开发用构建目录隔离，避免缓存干扰 Release 产物）。
NAME="csudb-client-linux-x64"
BUILD_DIR="build_release"

echo "== 配置 Release 构建（无 ASan，静态标准库）=="
# 配置 CMake：Release 优化、关闭 ASan、静态链接标准库、不编译单元测试。
cmake -S . -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_ASAN=OFF \
  -DSTATIC_STDLIB=ON \
  -DWITH_UNIT_TESTS=OFF

echo "== 编译 csudb 客户端 =="
# 仅构建 csudb 目标，-j 使用全部 CPU 核心并行加速。
cmake --build "${BUILD_DIR}" --target csudb -j"$(nproc)"

# 校验可执行文件确实生成且具备执行权限，否则终止打包。
if [ ! -x "${BUILD_DIR}/bin/csudb" ]; then
  echo "构建失败：未找到 ${BUILD_DIR}/bin/csudb" >&2
  exit 1
fi

echo "== 组装发布目录 dist/${NAME} =="
# 重建干净的发布目录，复制二进制与说明文档；NOTICE 存在才复制。
rm -rf "dist/${NAME}"
mkdir -p "dist/${NAME}"
cp "${BUILD_DIR}/bin/csudb" "dist/${NAME}/csudb"
cp "scripts/client_readme.md" "dist/${NAME}/README.md"
[ -f NOTICE ] && cp NOTICE "dist/${NAME}/NOTICE" || true

echo "== 打包 =="
# 以 dist 为根打包，解压后顶层目录即 ${NAME}；随后生成 SHA256 校验文件。
tar -czf "dist/${NAME}.tar.gz" -C dist "${NAME}"
( cd dist && sha256sum "${NAME}.tar.gz" > "${NAME}.tar.gz.sha256" )

echo
echo "完成："
# 列出产物大小，便于确认打包结果。
ls -lh "dist/${NAME}.tar.gz" "dist/${NAME}.tar.gz.sha256"
# 面向使用者的分发与运行提示。
echo "分发给他人：下载 tar.gz → 解压 → chmod +x csudb → ./csudb --url <host:port> -u root -p"
