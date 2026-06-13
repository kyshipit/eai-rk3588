#!/usr/bin/env python3
"""校验 runtime/config/default.yaml 结构与 main.cc 读取项一致。"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

try:
    import yaml
except ImportError:
    print("FAIL 需要 PyYAML: pip install pyyaml", file=sys.stderr)
    sys.exit(1)


# main.cc 读取且无默认值的 key；与 runtime/config/default.yaml 结构对齐。
REQUIRED_KEYS: list[tuple[str, str]] = [
    ("model.type", "str"),
    ("model.yolo.path", "str"),
    ("model.yolo.person_threshold_percent", "int"),
    ("model.scrfd.path", "str"),
    ("model.scrfd.conf_threshold_percent", "int"),
    ("model.scrfd.nms_threshold_percent", "int"),
    ("model.llm.enabled", "bool"),
    ("model.llm.path", "str"),
    ("model.llm.max_new_tokens", "int"),
    ("model.llm.max_context_len", "int"),
    ("model.llm.preload_on_startup", "bool"),
    ("model.llm.preload_on_scrfd", "bool"),
    ("model.llm.face_stable_frames", "int"),
    ("model.llm.face_absent_frames", "int"),
    ("model.llm.grace_timeout_ms", "int"),
    ("model.llm.idle_timeout_ms", "int"),
    ("model.llm.auto_greeting_text", "str"),
    ("model.llm.user_prompt_prefix", "str"),
    ("model.tts.enabled", "bool"),
    ("model.tts.skip_static_greeting", "bool"),
    ("model.tts.encoder_path", "str"),
    ("model.tts.decoder_path", "str"),
    ("model.tts.lexicon_path", "str"),
    ("model.tts.tokens_path", "str"),
    ("model.tts.language", "str"),
    ("model.tts.speak_id", "int"),
    ("model.tts.speed", "num"),
    ("model.tts.disable_bert", "bool"),
    ("model.tts.preload_on_startup", "bool"),
    ("model.tts.max_speak_chars", "int"),
    ("model.tts.split_min_chars", "int"),
    ("model.tts.single_shot_max_chars", "int"),
    ("model.tts.planner.zh_min_chars", "int"),
    ("model.tts.planner.zh_max_chars", "int"),
    ("model.tts.planner.en_min_words", "int"),
    ("model.tts.planner.en_max_words", "int"),
    ("model.tts.planner.fallback_timeout_ms", "int"),
    ("model.tts.planner.short_answer_max_chars", "int"),
    ("model.tts.qos.enable_visual_throttle", "bool"),
    ("model.tts.qos.low_watermark_chunks", "int"),
    ("model.tts.qos.high_watermark_chunks", "int"),
    ("model.tts.qos.min_start_pcm_chunks", "int"),
    ("system.log_level", "str"),
    ("system.infer_threads", "int"),
    ("system.npu_cores", "int_array"),
    ("system.slots.yolo_always_on", "bool"),
    ("system.slots.scene_dwell_frames", "int"),
    ("system.switch.present_threshold", "int"),
    ("system.switch.absent_threshold", "int"),
    ("system.switch.single_thread", "bool"),
    ("input.source", "str"),
    ("input.rotate", "str"),
    ("input.width", "int"),
    ("input.height", "int"),
    ("input.show_window", "bool"),
    ("input.display.screen_width", "int"),
    ("input.display.screen_height", "int"),
    ("input.display.max_screen_ratio_percent", "int"),
    ("input.display.fullscreen", "bool"),
    ("input.display.title_bar_reserve_px", "int"),
]

LOG_LEVELS = {"debug", "info", "warn", "error", "fatal"}
ROTATE_VALUES = {"none", "ccw90", "cw90", "180", "0", "90cw"}
TTS_LANGUAGES = {"ZH_MIX_EN", "ZH", "EN"}


def get_node(cfg: dict[str, Any], dotted: str) -> Any:
    cur: Any = cfg
    for part in dotted.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def is_bool(v: Any) -> bool:
    return isinstance(v, bool)


def is_int(v: Any) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def is_num(v: Any) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def check_type(key: str, kind: str, value: Any) -> str | None:
    if value is None:
        return "缺失"
    if kind == "str":
        if not isinstance(value, str):
            return f"应为字符串，实际为 {type(value).__name__}"
    elif kind == "int":
        if not is_int(value):
            return f"应为整数，实际为 {type(value).__name__}: {value!r}"
    elif kind == "bool":
        if not is_bool(value):
            return f"应为 true/false，实际为 {type(value).__name__}: {value!r}"
    elif kind == "num":
        if not is_num(value):
            return f"应为数字，实际为 {type(value).__name__}: {value!r}"
    elif kind == "int_array":
        if not isinstance(value, list) or not value or not all(is_int(x) for x in value):
            return "应为非空整数数组，例如 [0, 1]"
    return None


def check_ranges(cfg: dict[str, Any], errors: list[str]) -> None:
    def pct(key: str) -> int | None:
        v = get_node(cfg, key)
        return v if is_int(v) else None

    for key in (
        "model.yolo.person_threshold_percent",
        "model.scrfd.conf_threshold_percent",
        "model.scrfd.nms_threshold_percent",
        "input.display.max_screen_ratio_percent",
    ):
        v = pct(key)
        if v is not None and not (0 <= v <= 100):
            errors.append(f"{key} 应在 0~100，当前 {v}")

    model_type = get_node(cfg, "model.type")
    if isinstance(model_type, str) and model_type != "yolo":
        errors.append(f'model.type 当前仅支持 "yolo"，当前为 {model_type!r}')

    log_level = get_node(cfg, "system.log_level")
    if isinstance(log_level, str) and log_level not in LOG_LEVELS:
        errors.append(f"system.log_level 非法: {log_level!r}，允许 {sorted(LOG_LEVELS)}")

    rotate = get_node(cfg, "input.rotate")
    if isinstance(rotate, str) and rotate not in ROTATE_VALUES:
        errors.append(f"input.rotate 非法: {rotate!r}，允许 {sorted(ROTATE_VALUES)}")

    lang = get_node(cfg, "model.tts.language")
    if isinstance(lang, str) and lang not in TTS_LANGUAGES:
        errors.append(f"model.tts.language 非法: {lang!r}，允许 {sorted(TTS_LANGUAGES)}")

    speed = get_node(cfg, "model.tts.speed")
    if is_num(speed) and float(speed) <= 0:
        errors.append(f"model.tts.speed 应 > 0，当前 {speed}")

    infer_threads = get_node(cfg, "system.infer_threads")
    if is_int(infer_threads) and infer_threads < 1:
        errors.append(f"system.infer_threads 应 >= 1，当前 {infer_threads}")

    zh_min = get_node(cfg, "model.tts.planner.zh_min_chars")
    zh_max = get_node(cfg, "model.tts.planner.zh_max_chars")
    if is_int(zh_min) and is_int(zh_max) and zh_min > zh_max:
        errors.append(f"model.tts.planner.zh_min_chars ({zh_min}) 不能大于 zh_max_chars ({zh_max})")

    en_min = get_node(cfg, "model.tts.planner.en_min_words")
    en_max = get_node(cfg, "model.tts.planner.en_max_words")
    if is_int(en_min) and is_int(en_max) and en_min > en_max:
        errors.append(f"model.tts.planner.en_min_words ({en_min}) 不能大于 en_max_words ({en_max})")

    low = get_node(cfg, "model.tts.qos.low_watermark_chunks")
    high = get_node(cfg, "model.tts.qos.high_watermark_chunks")
    if is_int(low) and is_int(high) and low > high:
        errors.append(
            f"model.tts.qos.low_watermark_chunks ({low}) 不能大于 high_watermark_chunks ({high})"
        )

    fallback = get_node(cfg, "model.tts.planner.fallback_timeout_ms")
    if is_int(fallback) and fallback < 100:
        errors.append(f"model.tts.planner.fallback_timeout_ms 应 >= 100（main 会 clamp），当前 {fallback}")

    llm_enabled = get_node(cfg, "model.llm.enabled")
    tts_enabled = get_node(cfg, "model.tts.enabled")
    if llm_enabled is False and tts_enabled is True:
        errors.append("model.tts.enabled=true 但 model.llm.enabled=false；启动 TTS 链路仍要求 llm.enabled=true")

    llm_path = get_node(cfg, "model.llm.path")
    if llm_enabled is True and isinstance(llm_path, str) and llm_path.strip() == "":
        errors.append("model.llm.enabled=true 时 model.llm.path 不能为空")

    if llm_enabled is True and tts_enabled is True:
        for key in (
            "model.tts.encoder_path",
            "model.tts.decoder_path",
            "model.tts.lexicon_path",
            "model.tts.tokens_path",
        ):
            v = get_node(cfg, key)
            if isinstance(v, str) and v.strip() == "":
                errors.append(f"LLM+TTS 开启时 {key} 不能为空")


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="校验 edgeai runtime 配置文件")
    parser.add_argument(
        "config",
        nargs="?",
        default=str(repo_root / "runtime/config/default.yaml"),
        help="yaml 配置文件路径（默认 runtime/config/default.yaml）",
    )
    args = parser.parse_args()

    config_path = Path(args.config).resolve()

    if not config_path.is_file():
        print(f"FAIL 配置文件不存在: {config_path}", file=sys.stderr)
        return 1

    try:
        with config_path.open(encoding="utf-8") as f:
            cfg = yaml.safe_load(f)
    except yaml.YAMLError as e:
        print(f"FAIL YAML 解析错误: {e}", file=sys.stderr)
        return 1

    if not isinstance(cfg, dict):
        print("FAIL 根节点应为 mapping", file=sys.stderr)
        return 1

    errors: list[str] = []

    for key, kind in REQUIRED_KEYS:
        value = get_node(cfg, key)
        err = check_type(key, kind, value)
        if err:
            errors.append(f"{key}: {err}")

    check_ranges(cfg, errors)

    print(f"check_config: config={config_path}")

    if errors:
        for e in errors:
            print(f"FAIL  {e}")
        print(f"check_config: FAILED ({len(errors)} 项)")
        return 1

    print("check_config: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
