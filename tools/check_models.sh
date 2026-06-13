#!/usr/bin/env bash
# 检查 default.yaml 中引用的模型/词表文件是否存在且非空。
# 用法:
#   ./tools/check_models.sh
#   ./tools/check_models.sh --cwd /path/to/rknn_eai_rk3588
#   ./tools/check_models.sh --config runtime/config/default.yaml --cwd .
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CWD="${REPO_ROOT}"
CONFIG="${REPO_ROOT}/runtime/config/default.yaml"

usage() {
    echo "用法: $0 [--cwd DIR] [--config PATH]" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cwd)
            [[ $# -ge 2 ]] || usage
            CWD="$(cd "$2" && pwd)"
            shift 2
            ;;
        --config)
            [[ $# -ge 2 ]] || usage
            CONFIG="$2"
            if [[ "$CONFIG" != /* ]]; then
                CONFIG="$(cd "$(dirname "$CONFIG")" && pwd)/$(basename "$CONFIG")"
            fi
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "未知参数: $1" >&2
            usage
            ;;
    esac
done

if [[ ! -f "$CONFIG" ]]; then
    echo "FAIL 配置文件不存在: $CONFIG" >&2
    exit 1
fi

if [[ ! -d "$CWD" ]]; then
    echo "FAIL 工作目录不存在: $CWD" >&2
    exit 1
fi

# 从 yaml 提取 path 类字段及 llm/tts 开关，再按 cwd 解析相对路径。
mapfile -t PATH_LINES < <(python3 - "$CONFIG" <<'PY'
import sys
import yaml

cfg_path = sys.argv[1]
with open(cfg_path, encoding="utf-8") as f:
    cfg = yaml.safe_load(f) or {}

def get(d, *keys, default=None):
    cur = d
    for k in keys:
        if not isinstance(cur, dict) or k not in cur:
            return default
        cur = cur[k]
    return cur

paths = []
model = cfg.get("model") or {}

paths.append(("model.yolo.path", get(model, "yolo", "path")))
paths.append(("model.scrfd.path", get(model, "scrfd", "path")))

llm_enabled = bool(get(model, "llm", "enabled", default=False))
tts_enabled = bool(get(model, "tts", "enabled", default=False))

if llm_enabled:
    paths.append(("model.llm.path", get(model, "llm", "path")))

if llm_enabled and tts_enabled:
    tts = model.get("tts") or {}
    for key in (
        "encoder_path",
        "decoder_path",
        "lexicon_path",
        "tokens_path",
    ):
        paths.append((f"model.tts.{key}", tts.get(key)))

for name, p in paths:
    if p is None or str(p).strip() == "":
        print(f"MISSING_KEY\t{name}\t")
    else:
        print(f"PATH\t{name}\t{p}")
PY
)

fail=0
warn=0

human_size() {
    local bytes="$1"
    if [[ "$bytes" -lt 1024 ]]; then
        echo "${bytes} B"
    elif [[ "$bytes" -lt 1048576 ]]; then
        echo "$(( bytes / 1024 )) KB"
    else
        echo "$(( bytes / 1048576 )) MB"
    fi
}

echo "check_models: cwd=${CWD} config=${CONFIG}"

for line in "${PATH_LINES[@]}"; do
    kind="${line%%$'\t'*}"
    rest="${line#*$'\t'}"
    name="${rest%%$'\t'*}"
    raw_path="${rest#*$'\t'}"

    if [[ "$kind" == "MISSING_KEY" ]]; then
        echo "FAIL  ${name} 未配置路径"
        fail=1
        continue
    fi

    if [[ "$raw_path" == /* ]]; then
        resolved="$raw_path"
    else
        resolved="${CWD}/${raw_path#./}"
    fi

    if [[ ! -e "$resolved" ]]; then
        if [[ "$name" == "model.llm.path" ]]; then
            echo "WARN  ${name}  ${raw_path}  -> 不存在 (${resolved})，板端将仅视觉模式"
            warn=1
        else
            echo "FAIL  ${name}  ${raw_path}  -> 不存在 (${resolved})"
            fail=1
        fi
    elif [[ ! -s "$resolved" ]]; then
        echo "FAIL  ${name}  ${raw_path}  -> 空文件 (${resolved})"
        fail=1
    else
        size="$(stat -c '%s' "$resolved" 2>/dev/null || stat -f '%z' "$resolved")"
        echo "OK    ${name}  ${raw_path}  ($(human_size "$size"))"
    fi
done

# YOLO 标签文件由 CMake 安装，运行时也需要。
for extra in "./model/coco_80_labels_list.txt"; do
    resolved="${CWD}/${extra#./}"
    name="model.coco_80_labels_list.txt"
    if [[ ! -f "$resolved" ]]; then
        echo "WARN  ${name}  ${extra}  -> 不存在 (${resolved})"
        warn=1
    elif [[ ! -s "$resolved" ]]; then
        echo "FAIL  ${name}  ${extra}  -> 空文件"
        fail=1
    else
        size="$(stat -c '%s' "$resolved" 2>/dev/null || stat -f '%z' "$resolved")"
        echo "OK    ${name}  ${extra}  ($(human_size "$size"))"
    fi
done

if [[ "$fail" -ne 0 ]]; then
    echo "check_models: FAILED"
    exit 1
fi

if [[ "$warn" -ne 0 ]]; then
    echo "check_models: OK (有 WARN，请确认 install/model/ 是否已同步)"
    exit 0
fi

echo "check_models: OK"
exit 0
