#!/usr/bin/env bash
# ------------------------------------------------------------------------------------------------
# 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
# ------------------------------------------------------------------------------------------------
#  package_sdk.sh:31          TOPDIR
#  package_sdk.sh:35          VER
#  package_sdk.sh:36          PY_NAME
#  package_sdk.sh:37          JDBC_JAR
#  package_sdk.sh:39          Python SDK
#  package_sdk.sh:48          Java JDBC
#  package_sdk.sh:57          校验
# ------------------------------------------------------------------------------------------------
# 打包 CSUDB SDK（Python 驱动 + Java JDBC 驱动）供外部项目使用。
# 产物：dist/csudb-python-sdk-<ver>.tar.gz、dist/csudb-jdbc-<ver>.jar、dist/SDK-README.md、dist/SDK-sha256.txt
#
# 输入：
#   - sdk/python/ 下的 Python 驱动源码与说明文档
#   - sdk/java/ 下的 JDBC 驱动源码（由 sdk/java/build.sh 编译）
#   - scripts/sdk_readme.md（对外 SDK 说明）
#   - 可选的 javac（JDK 17+），缺失时跳过 Java 部分
# 输出（全部位于 dist/ 目录）：
#   - csudb-python-sdk-<ver>.tar.gz    Python SDK 压缩包
#   - csudb-jdbc-<ver>.jar             JDBC 驱动（若成功构建）
#   - SDK-README.md                    对外说明文档
#   - SDK-sha256.txt                   上述产物的 SHA256 校验值
# 说明：脚本自身负责切换到仓库根目录，可在任意工作目录下调用。
set -euo pipefail

# 切换到仓库根目录：以脚本自身位置为基准解析，保证相对路径稳定可比。
# （$(dirname "${BASH_SOURCE[0]}") 为 scripts/，其上一级即仓库根目录）
TOPDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$TOPDIR"

# 版本号与产物文件名（集中定义，便于统一修改）。
VER="2026.1.0"
PY_NAME="csudb-python-sdk-${VER}"
JDBC_JAR="csudb-jdbc-${VER}.jar"

echo "== Python SDK =="
# 清理旧的暂存目录，确保不残留上一次打包的文件。
rm -rf "dist/sdk/${PY_NAME}"
mkdir -p "dist/sdk/${PY_NAME}"
# 复制 Python 驱动源码、包初始化文件和说明文档到暂存目录。
cp sdk/python/csudb.py sdk/python/__init__.py sdk/python/README.md "dist/sdk/${PY_NAME}/"
# 以暂存目录为根打包，保证解压后顶层目录名为 ${PY_NAME}。
tar -czf "dist/${PY_NAME}.tar.gz" -C dist/sdk "${PY_NAME}"

echo "== Java JDBC =="
# 仅当系统存在 javac 时才构建 JDBC 驱动，否则给出告警并跳过（不中断整体打包）。
if command -v javac >/dev/null 2>&1; then
  bash sdk/java/build.sh
  cp "sdk/java/build/${JDBC_JAR}" "dist/${JDBC_JAR}"
else
  echo "警告：未找到 javac（需 JDK 17+），跳过 Java JDBC 构建" >&2
fi

echo "== 校验 =="
# 复制对外 README；随后重建校验文件，避免追加到旧内容上。
cp scripts/sdk_readme.md dist/SDK-README.md
: > dist/SDK-sha256.txt
# 计算 Python SDK 压缩包的 SHA256 并写入校验文件（在 dist 内计算以得到相对路径）。
( cd dist && sha256sum "${PY_NAME}.tar.gz" >> SDK-sha256.txt )
# 若 JDBC jar 存在则一并计算校验值；不存在时用 || true 忽略失败，保证 set -e 下不报错。
[ -f "dist/${JDBC_JAR}" ] && ( cd dist && sha256sum "${JDBC_JAR}" >> SDK-sha256.txt ) || true

echo
echo "完成："
# 列出最终产物（2>/dev/null 屏蔽缺失文件时的报错噪声）。
ls -lh dist/*.tar.gz dist/*.jar dist/SDK-README.md dist/SDK-sha256.txt 2>/dev/null
