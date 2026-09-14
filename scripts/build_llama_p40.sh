#!/usr/bin/env bash
# 在 NVIDIA Tesla P40 (Pascal / compute capability 6.1) 上构建 llama.cpp。
# 关键点：必须使用 CUDA 12.x（不要用 CUDA 13.x），显式 sm_61，并开启 GGML_CUDA_FORCE_MMQ。
set -euo pipefail

LLAMA_DIR="${LLAMA_DIR:-$HOME/llama.cpp}"
CUDA_ARCH="${CUDA_ARCH:-61}"

echo "== 环境检查 =="
if ! command -v nvidia-smi >/dev/null 2>&1; then
  echo "未找到 nvidia-smi，请先安装 NVIDIA 驱动" >&2
  exit 1
fi
nvidia-smi --query-gpu=name,compute_cap,memory.total --format=csv || true

if ! command -v nvcc >/dev/null 2>&1; then
  echo "未找到 nvcc，请安装 CUDA 12.x toolkit" >&2
  exit 1
fi
nvcc --version | tail -n 2
CUDA_VER="$(nvcc --version | sed -n 's/.*release \([0-9][0-9]*\)\..*/\1/p' | head -n1)"
if [[ -n "${CUDA_VER}" && "${CUDA_VER}" -ge 13 ]]; then
  echo "检测到 CUDA ${CUDA_VER}.x，P40 构建请改用 CUDA 12.x toolkit" >&2
  exit 1
fi

if [[ ! -d "${LLAMA_DIR}" ]]; then
  echo "== 克隆 llama.cpp 到 ${LLAMA_DIR} =="
  git clone https://github.com/ggml-org/llama.cpp.git "${LLAMA_DIR}"
fi

cd "${LLAMA_DIR}"

echo "== 配置 (CUDA_ARCHITECTURES=${CUDA_ARCH}, GGML_CUDA_FORCE_MMQ=ON) =="
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES="${CUDA_ARCH}" \
  -DGGML_CUDA_FORCE_MMQ=ON

echo "== 编译 =="
cmake --build build -j"$(nproc)"

echo "== 验收 =="
./build/bin/llama-server --version || true
./build/bin/llama-server --list-devices || true

echo "完成。可运行 scripts/run_sql_completion_model.sh 启动补全模型。"
