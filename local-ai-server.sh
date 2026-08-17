#!/bin/bash
# Build and run the llama.cpp server this fork talks to.
#
# The client speaks the OpenAI-compatible subset (/v1/models and
# /v1/chat/completions with stream: true), so any llama.cpp build recent enough
# to have `llama-server` works.
#
# Usage:
#   ./local-ai-server.sh setup            # clone + build llama.cpp
#   ./local-ai-server.sh model <hf-repo>  # download a GGUF from Hugging Face
#   ./local-ai-server.sh serve [gguf...]  # run the server
#
# Environment:
#   LLAMA_DIR     where llama.cpp lives            (default ~/llama.cpp)
#   MODELS_DIR    where GGUF files live            (default ~/llama-models)
#   MODELS_MAX    models kept resident in router mode (default 2)
#   BACKEND       cuda | metal | vulkan | cpu      (default: autodetected)
#   HOST          bind address                     (default 127.0.0.1)
#   PORT          listen port                      (default 8080)
#   NGL           layers to offload to the GPU     (default 999 = all)
#   CTX           context size per slot            (default 8192)
#   PARALLEL      concurrent slots                 (default 1)
#   API_KEY       require this bearer token        (default: none)
#
# Renting a GPU box: run `serve` there with HOST=0.0.0.0 behind an SSH tunnel
#   ssh -N -L 8080:127.0.0.1:8080 user@gpu-box
# and leave the app pointed at http://127.0.0.1:8080. Exposing the port to the
# open internet without a tunnel means anyone can use (and read) your model.
set -euo pipefail

LLAMA_DIR="${LLAMA_DIR:-$HOME/llama.cpp}"
MODELS_DIR="${MODELS_DIR:-$HOME/llama-models}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8080}"
NGL="${NGL:-999}"
CTX="${CTX:-8192}"
PARALLEL="${PARALLEL:-1}"
API_KEY="${API_KEY:-}"

detect_backend() {
	if [ -n "${BACKEND:-}" ]; then
		echo "$BACKEND"
	elif command -v nvcc >/dev/null 2>&1 || [ -d /usr/local/cuda ]; then
		echo cuda
	elif [ "$(uname -s)" = "Darwin" ]; then
		echo metal
	elif command -v vulkaninfo >/dev/null 2>&1; then
		echo vulkan
	else
		echo cpu
	fi
}

cmake_flags() {
	case "$(detect_backend)" in
	cuda)   echo "-DGGML_CUDA=ON" ;;
	metal)  echo "-DGGML_METAL=ON" ;;
	vulkan) echo "-DGGML_VULKAN=ON" ;;
	*)      echo "" ;;
	esac
}

cmd_setup() {
	local backend
	backend="$(detect_backend)"
	echo "=== backend: $backend ==="
	if [ ! -d "$LLAMA_DIR/.git" ]; then
		git clone --depth 1 https://github.com/ggml-org/llama.cpp "$LLAMA_DIR"
	else
		git -C "$LLAMA_DIR" pull --ff-only
	fi
	# shellcheck disable=SC2046
	cmake -S "$LLAMA_DIR" -B "$LLAMA_DIR/build" \
		-DCMAKE_BUILD_TYPE=Release \
		-DLLAMA_BUILD_SERVER=ON \
		$(cmake_flags)
	cmake --build "$LLAMA_DIR/build" --config Release -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" --target llama-server
	echo "=== built $(server_bin) ==="
}

server_bin() {
	for candidate in \
		"$LLAMA_DIR/build/bin/llama-server" \
		"$LLAMA_DIR/build/llama-server" \
		"$(command -v llama-server 2>/dev/null || true)"
	do
		if [ -n "$candidate" ] && [ -x "$candidate" ]; then
			echo "$candidate"
			return 0
		fi
	done
	return 1
}

cmd_model() {
	local repo="${1:-}"
	if [ -z "$repo" ]; then
		echo "usage: $0 model <hf-user/hf-repo> [filename-substring]" >&2
		exit 2
	fi
	mkdir -p "$MODELS_DIR"
	if ! command -v huggingface-cli >/dev/null 2>&1; then
		echo "error: huggingface-cli not found (pip install -U 'huggingface_hub[cli]')" >&2
		exit 1
	fi
	local pattern="${2:-*.gguf}"
	huggingface-cli download "$repo" --include "$pattern" --local-dir "$MODELS_DIR"
	echo "=== models in $MODELS_DIR ==="
	find "$MODELS_DIR" -name '*.gguf' -printf '%p\n' 2>/dev/null || find "$MODELS_DIR" -name '*.gguf'
}

cmd_serve() {
	local bin
	if ! bin="$(server_bin)"; then
		echo "error: llama-server not found -- run '$0 setup' first" >&2
		exit 1
	fi
	local -a args=(
		--host "$HOST" --port "$PORT"
		--n-gpu-layers "$NGL"
		--ctx-size "$((CTX * PARALLEL))"
		--parallel "$PARALLEL"
		--flash-attn auto
		--jinja                 # honour each model's own chat template
		--metrics
	)
	if [ -n "$API_KEY" ]; then
		args+=(--api-key "$API_KEY")
	fi

	if [ "$#" -gt 0 ]; then
		# Explicit files: classic single-model mode, one chat in the app.
		for file in "$@"; do
			echo "    model: $file"
			args+=(--model "$file")
		done
	else
		# Router mode: started without --model, llama-server lists every GGUF
		# under --models-dir in /v1/models and loads them on demand. That list
		# is exactly what the app turns into chats.
		if ! find "$MODELS_DIR" -name '*.gguf' 2>/dev/null | grep -q .; then
			echo "error: no GGUF files in $MODELS_DIR -- run '$0 model <repo>'" >&2
			exit 1
		fi
		args+=(--models-dir "$MODELS_DIR" --models-max "${MODELS_MAX:-2}")
		echo "    router over $MODELS_DIR:"
		find "$MODELS_DIR" -name '*.gguf' | sort | sed 's/^/      /'
	fi

	echo "=== $bin ==="
	echo "=== http://$HOST:$PORT (ngl=$NGL ctx=$CTX x$PARALLEL) ==="
	exec "$bin" "${args[@]}"
}

case "${1:-serve}" in
setup) shift; cmd_setup "$@" ;;
model) shift; cmd_model "$@" ;;
serve) shift; cmd_serve "$@" ;;
*) echo "usage: $0 {setup|model|serve} ..." >&2; exit 2 ;;
esac
