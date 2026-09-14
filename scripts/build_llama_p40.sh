#!/usr/bin/env bash
# 在 NVIDIA Tesla P40 (Pascal / compute capability 6.1) 上构建 llama.cpp。
# 关键点：P40 是 sm_61，CUDA 13.x 已不再支持 Pascal，必须使用 CUDA 12.x。
# 默认开启 CUDA MMQ（P40 无 Tensor Core，MMQ 是主要路径）。
set -euo pipefail

LLAMA_DIR="${LLAMA_DIR:-$HOME/llama.cpp}"
CUDA_ARCH="${CUDA_ARCH:-61}"

echo "== 环境检查 =="
if ! command -v nvidia-smi >/dev/null 2>&1; then
  echo "未找到 nvidia-smi，请先安装 NVIDIA 驱动" >&2
  exit 1
fi
nvidia-smi --query-gpu=name,compute_cap,memory.total --format=csv || true

# 选择 CUDA 12.x 工具链（优先 /usr/local/cuda-12.*，其次 PATH 中的 nvcc）
CUDA_HOME_CANDIDATE=""
for d in /usr/local/cuda-12 /usr/local/cuda-12.*; do
  if [ -x "$d/bin/nvcc" ]; then CUDA_HOME_CANDIDATE="$d"; fi
done
if [ -z "$CUDA_HOME_CANDIDATE" ] && command -v nvcc >/dev/null 2>&1; then
  CUDA_HOME_CANDIDATE="$(dirname "$(dirname "$(readlink -f "$(command -v nvcc)")")")"
fi
if [ -z "$CUDA_HOME_CANDIDATE" ] || [ ! -x "$CUDA_HOME_CANDIDATE/bin/nvcc" ]; then
  echo "未找到 CUDA 12.x nvcc。P40(sm_61) 不能用 CUDA 13.x 构建。" >&2
  echo "请安装 CUDA 12.x toolkit（例如 cuda-toolkit-12-4）后重试。" >&2
  exit 1
fi

NVCC="$CUDA_HOME_CANDIDATE/bin/nvcc"
CUDA_VER="$("$NVCC" --version | sed -n 's/.*release \([0-9][0-9]*\)\..*/\1/p' | head -n1)"
echo "使用 CUDA: $CUDA_HOME_CANDIDATE (major=$CUDA_VER)"
if [ -n "$CUDA_VER" ] && [ "$CUDA_VER" -ge 13 ]; then
  echo "检测到 CUDA ${CUDA_VER}.x，P40 构建必须使用 CUDA 12.x" >&2
  exit 1
fi
if ! "$NVCC" --list-gpu-arch 2>/dev/null | grep -q "compute_${CUDA_ARCH}"; then
  echo "CUDA 工具链不支持 compute_${CUDA_ARCH}（P40 需要）" >&2
  exit 1
fi

if [ ! -d "${LLAMA_DIR}" ]; then
  echo "== 克隆 llama.cpp 到 ${LLAMA_DIR} =="
  git clone --depth 1 https://github.com/ggml-org/llama.cpp.git "${LLAMA_DIR}"
fi

cd "${LLAMA_DIR}"

echo "== 配置 (CUDA_ARCHITECTURES=${CUDA_ARCH}, GGML_CUDA_FORCE_MMQ=ON) =="
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES="${CUDA_ARCH}" \
  -DGGML_CUDA_FORCE_MMQ=ON \
  -DLLAMA_CURL=OFF \
  -DCUDAToolkit_ROOT="${CUDA_HOME_CANDIDATE}" \
  -DCMAKE_CUDA_COMPILER="${NVCC}"

echo "== 编译 llama-server =="
cmake --build build --target llama-server -j"$(nproc)"

echo "== 验收 =="
./build/bin/llama-server --version || true
./build/bin/llama-server --list-devices || true

echo "完成。可运行 scripts/run_sql_completion_model.sh 启动补全模型。"
