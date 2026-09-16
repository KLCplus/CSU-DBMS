#!/usr/bin/env bash
# 在 NVIDIA Tesla P40 (Pascal / compute capability 6.1) 上构建 llama.cpp。
# 关键点：P40 是 sm_61，CUDA 13.x 已不再支持 Pascal，必须使用 CUDA 12.x。
# 默认开启 CUDA MMQ（P40 无 Tensor Core，MMQ 是主要路径）。
#
# 输入（均可用环境变量覆盖）：
#   - LLAMA_DIR   llama.cpp 源码目录，默认 $HOME/llama.cpp（不存在则自动浅克隆）
#   - CUDA_ARCH   CUDA 计算能力，默认 61（对应 P40 的 sm_61）
# 前置条件：
#   - NVIDIA 驱动（nvidia-smi 可用）
#   - CUDA 12.x 工具链（/usr/local/cuda-12* 或 PATH 中的 nvcc）
# 产物：
#   - ${LLAMA_DIR}/build/ 下编译出的 llama-server 可执行文件
set -euo pipefail

# 允许通过环境变量覆盖源码目录与目标 CUDA 架构，默认面向 P40(sm_61)。
LLAMA_DIR="${LLAMA_DIR:-$HOME/llama.cpp}"
CUDA_ARCH="${CUDA_ARCH:-61}"

echo "== 环境检查 =="
# 必须先有 NVIDIA 驱动；缺失时直接报错退出。
if ! command -v nvidia-smi >/dev/null 2>&1; then
  echo "未找到 nvidia-smi，请先安装 NVIDIA 驱动" >&2
  exit 1
fi
# 打印 GPU 型号/计算能力/显存，便于确认目标确为 P40（|| true 避免查询失败中断）。
nvidia-smi --query-gpu=name,compute_cap,memory.total --format=csv || true

# 选择 CUDA 12.x 工具链（优先 /usr/local/cuda-12.*，其次 PATH 中的 nvcc）
# 依次探测常见安装路径；循环结束后保留最后一个匹配项（即版本最高的 cuda-12.*）。
CUDA_HOME_CANDIDATE=""
for d in /usr/local/cuda-12 /usr/local/cuda-12.*; do
  if [ -x "$d/bin/nvcc" ]; then CUDA_HOME_CANDIDATE="$d"; fi
done
# 若未在固定路径找到，则回退到 PATH 中的 nvcc，并向上回溯两级目录得到 CUDA_HOME。
if [ -z "$CUDA_HOME_CANDIDATE" ] && command -v nvcc >/dev/null 2>&1; then
  CUDA_HOME_CANDIDATE="$(dirname "$(dirname "$(readlink -f "$(command -v nvcc)")")")"
fi
# 仍未找到可用 nvcc 则报错退出，并提示安装 CUDA 12.x。
if [ -z "$CUDA_HOME_CANDIDATE" ] || [ ! -x "$CUDA_HOME_CANDIDATE/bin/nvcc" ]; then
  echo "未找到 CUDA 12.x nvcc。P40(sm_61) 不能用 CUDA 13.x 构建。" >&2
  echo "请安装 CUDA 12.x toolkit（例如 cuda-toolkit-12-4）后重试。" >&2
  exit 1
fi

# 解析 nvcc 主版本号（如 "release 12.4" -> 12），用于后续校验。
NVCC="$CUDA_HOME_CANDIDATE/bin/nvcc"
CUDA_VER="$("$NVCC" --version | sed -n 's/.*release \([0-9][0-9]*\)\..*/\1/p' | head -n1)"
echo "使用 CUDA: $CUDA_HOME_CANDIDATE (major=$CUDA_VER)"
# CUDA 13+ 已放弃 Pascal，检测到即拒绝，避免编译出无法在 P40 上运行的代码。
if [ -n "$CUDA_VER" ] && [ "$CUDA_VER" -ge 13 ]; then
  echo "检测到 CUDA ${CUDA_VER}.x，P40 构建必须使用 CUDA 12.x" >&2
  exit 1
fi
# 确认该工具链支持目标架构 compute_61，否则同样无法为 P40 构建。
if ! "$NVCC" --list-gpu-arch 2>/dev/null | grep -q "compute_${CUDA_ARCH}"; then
  echo "CUDA 工具链不支持 compute_${CUDA_ARCH}（P40 需要）" >&2
  exit 1
fi

# 源码目录不存在时浅克隆 llama.cpp（--depth 1 节省时间与空间）。
if [ ! -d "${LLAMA_DIR}" ]; then
  echo "== 克隆 llama.cpp 到 ${LLAMA_DIR} =="
  git clone --depth 1 https://github.com/ggml-org/llama.cpp.git "${LLAMA_DIR}"
fi

# 进入源码目录并就地构建。
cd "${LLAMA_DIR}"

echo "== 配置 (CUDA_ARCHITECTURES=${CUDA_ARCH}, GGML_CUDA_FORCE_MMQ=ON) =="
# 配置构建：Release、启用 CUDA、限定 sm_61、强制 MMQ、关闭 curl，
# 并显式指定 CUDA 根目录与 nvcc，避免误用其他版本工具链。
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES="${CUDA_ARCH}" \
  -DGGML_CUDA_FORCE_MMQ=ON \
  -DLLAMA_CURL=OFF \
  -DCUDAToolkit_ROOT="${CUDA_HOME_CANDIDATE}" \
  -DCMAKE_CUDA_COMPILER="${NVCC}"

echo "== 编译 llama-server =="
# 仅构建 llama-server 目标，-j 全核心并行编译。
cmake --build build --target llama-server -j"$(nproc)"

echo "== 验收 =="
# 打印版本与可用设备信息以确认构建成功（|| true 容忍运行时无 GPU 等情况）。
./build/bin/llama-server --version || true
./build/bin/llama-server --list-devices || true

echo "完成。可运行 scripts/run_sql_completion_model.sh 启动补全模型。"
