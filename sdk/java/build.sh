#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="${SCRIPT_DIR}/build"
CLASSES_DIR="${BUILD_DIR}/classes"
JAR_FILE="${BUILD_DIR}/csudb-jdbc-2026.1.0.jar"

rm -rf "${CLASSES_DIR}"
mkdir -p "${CLASSES_DIR}"

mapfile -t SOURCES < <(find "${SCRIPT_DIR}/src/main/java" -name '*.java' -print | sort)
javac --release 17 -encoding UTF-8 -d "${CLASSES_DIR}" "${SOURCES[@]}"
cp -R "${SCRIPT_DIR}/src/main/resources/." "${CLASSES_DIR}/"
jar --create --file "${JAR_FILE}" -C "${CLASSES_DIR}" .

echo "Built ${JAR_FILE}"
