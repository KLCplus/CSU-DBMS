#!/usr/bin/env bash
# 启动 SQL 补全模型 sidecar：Qwen2.5-Coder-1.5B (GGUF Q8_0) + llama.cpp /infill。
# 仅监听 127.0.0.1，避免把本地 SQL/schema 暴露到公网。
#
# 模型来源（二选一）：
#   1) 本地 GGUF：SQL_COMPLETION_MODEL_FILE=/path/to/qwen2.5-coder-1.5b-q8_0.gguf
#   2) HuggingFace：SQL_COMPLETION_MODEL=ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF:Q8_0
set -euo pipefail

LLAMA_SERVER="${LLAMA_SERVER:-$HOME/llama.cpp/build/bin/llama-server}"
HOST="${SQL_COMPLETION_HOST:-127.0.0.1}"
PORT="${SQL_COMPLETION_PORT:-8012}"
CTX="${SQL_COMPLETION_CTX:-4096}"
MODEL_REPO="${SQL_COMPLETION_MODEL:-ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF:Q8_0}"
MODEL_FILE="${SQL_COMPLETION_MODEL_FILE:-}"

if [[ ! -x "${LLAMA_SERVER}" ]]; then
  echo "未找到 llama-server: ${LLAMA_SERVER}" >&2
  echo "请先运行 scripts/build_llama_p40.sh，或用 LLAMA_SERVER 指定路径" >&2
  exit 1
fi

# 若使用自建 CUDA 12 运行库，补充搜索路径
if [[ -d /usr/local/cuda-12.4/lib64 ]]; then
  export LD_LIBRARY_PATH="/usr/local/cuda-12.4/lib64:${LD_LIBRARY_PATH:-}"
fi

echo "启动 llama-server:"
echo "  host=${HOST} port=${PORT} ctx=${CTX}"

if [[ -n "${MODEL_FILE}" && -f "${MODEL_FILE}" ]]; then
  echo "  model(file)=${MODEL_FILE}"
  exec "${LLAMA_SERVER}" \
    -m "${MODEL_FILE}" \
    --host "${HOST}" --port "${PORT}" \
    -ngl 99 -c "${CTX}" -np 1 --cache-prompt --cache-reuse 64 -fa off
else
  echo "  model(hf)=${MODEL_REPO}"
  exec "${LLAMA_SERVER}" \
    -hf "${MODEL_REPO}" \
    --host "${HOST}" --port "${PORT}" \
    -ngl 99 -c "${CTX}" -np 1 --cache-prompt --cache-reuse 64 -fa off
fi
