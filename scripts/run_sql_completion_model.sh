#!/usr/bin/env bash
# 启动 SQL 补全模型 sidecar：Qwen2.5-Coder-1.5B (GGUF Q8_0) + llama.cpp /infill。
# 仅监听 127.0.0.1，避免把本地 SQL/schema 暴露到公网。
#
# 模型来源（二选一）：
#   1) 本地 GGUF：SQL_COMPLETION_MODEL_FILE=/path/to/qwen2.5-coder-1.5b-q8_0.gguf
#   2) HuggingFace：SQL_COMPLETION_MODEL=ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF:Q8_0
#
# 可用环境变量：
#   LLAMA_SERVER             llama-server 可执行文件路径（默认 $HOME/llama.cpp/build/bin/llama-server）
#   SQL_COMPLETION_HOST      监听地址，默认 127.0.0.1（仅本机可访问）
#   SQL_COMPLETION_PORT      监听端口，默认 8012
#   SQL_COMPLETION_CTX       上下文长度，默认 4096
#   SQL_COMPLETION_MODEL     HuggingFace 模型仓库，默认 ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF:Q8_0
#   SQL_COMPLETION_MODEL_FILE 本地 GGUF 文件路径，设置且存在时优先使用
# 说明：本脚本通过 exec 让 llama-server 接管进程，便于被 systemd 等管理器托管。
set -euo pipefail

# 各运行参数：均可由环境变量覆盖，未设置时使用默认值。
LLAMA_SERVER="${LLAMA_SERVER:-$HOME/llama.cpp/build/bin/llama-server}"
HOST="${SQL_COMPLETION_HOST:-127.0.0.1}"
PORT="${SQL_COMPLETION_PORT:-8012}"
CTX="${SQL_COMPLETION_CTX:-4096}"
MODEL_REPO="${SQL_COMPLETION_MODEL:-ggml-org/Qwen2.5-Coder-1.5B-Q8_0-GGUF:Q8_0}"
MODEL_FILE="${SQL_COMPLETION_MODEL_FILE:-}"

# 校验 llama-server 存在且可执行，否则提示先构建并退出。
if [[ ! -x "${LLAMA_SERVER}" ]]; then
  echo "未找到 llama-server: ${LLAMA_SERVER}" >&2
  echo "请先运行 scripts/build_llama_p40.sh，或用 LLAMA_SERVER 指定路径" >&2
  exit 1
fi

# 若使用自建 CUDA 12 运行库，补充搜索路径
# （把 CUDA 运行库目录前置到 LD_LIBRARY_PATH，使 llama-server 能找到 CUDA 动态库）。
if [[ -d /usr/local/cuda-12.4/lib64 ]]; then
  export LD_LIBRARY_PATH="/usr/local/cuda-12.4/lib64:${LD_LIBRARY_PATH:-}"
fi

echo "启动 llama-server:"
echo "  host=${HOST} port=${PORT} ctx=${CTX}"

# 优先使用本地 GGUF（存在即有效）；否则回退到从 HuggingFace 拉取。
# -ngl 99：尽可能多地把层放到 GPU；-c：上下文长度；-np 1：单并发；
# --cache-prompt/--cache-reuse：复用提示缓存加速补全；-fa off：关闭 FlashAttention（P40 兼容）。
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
