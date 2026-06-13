Language: **English** | [中文](troubleshooting_CN.md)

# Runtime troubleshooting

> Common on-board issues and decision paths. Platform architecture: [architecture-and-runtime.md](architecture-and-runtime.md); adapters: [adapters.md](adapters.md).

---

## Vision models

### YOLO: zero detections

**Typical signs**: Logs stay at `det_lines=0`, `person_present=0`; person→scrfd never triggers.

**Check in order**:

1. **Config paths**: Init logs `model_path=` / `path=` vs the yaml you pass on the command line.
2. **Output topology**: YOLO must be **3** fused heads (`output num: 3`, `dims[1]=255`). Nine tensors `score_8/bbox_8/kps_*` mean SCRFD topology was loaded — see below.
3. **Postprocess**: `yolo_postprocess.cpp` must decode `output[i]` for `i = 0..2`; Init runs `validate_yolov5_model_io` for `n_output==3` and first tensor `dims[1]==255`.

**Do not**: Assume “two different rknn files” from md5 alone while ignoring a wrong `model.yolo.path`; do not add 9-head SCRFD decoding to YOLO.

### YoloAdapter reports output num: 9

**Typical log**:

```text
output tensors:
  index=0, name=score_8, dims=[1, 12800, 1, 0], type=FP16
  ...
YoloAdapter: unsupported RKNN outputs=9 (need 3 fused heads), path=./model/scrfd.rknn
```

**Conclusion**: `model.yolo.path` **points at `scrfd.rknn`** (or is swapped with `model.scrfd.path`). Nine `score_8` tensors are SCRFD heads, not YOLOv5.

**Fix**:

```yaml
model.yolo.path: ./model/yolov5.rknn
model.scrfd.path: ./model/scrfd.rknn
```

With `infer_threads: 3`, three identical Init log blocks are normal.

### SCRFD: screen full of boxes

**Typical sign**: Dense unstable boxes after scrfd enables.

**Root cause**: Nine output indices do not match postprocess layout. On this board the usual order is **grouped**: three scores, then three bboxes, then three kps (not interleaved per stride).

**Checks**:

1. `ResolveScrfdHeadOutputs` in `scrfd_postprocess.cpp` matches names `score_{stride}` / `bbox_{stride}` / `kps_{stride}`.
2. If still too many boxes, raise `model.scrfd.conf_threshold_percent` (e.g. 65–75).
3. **Do not** change YOLO to 9-head decoding “for SCRFD”.

### Working directory and model paths

- Relative `./model/...` depends on **cwd at launch**; from `install/...` compare `install/model/` with the tree `model/`.
- Use `model.yolo.path` / `model.scrfd.path`; legacy top-level `model.path` is removed.
- Before push, from repo root: `python3 tools/check_config.py` (yaml structure) and `./tools/check_models.sh` (model files; use `--cwd` matching the board launch directory).
- Optional: `YoloAdapter: model realpath=...`, file size, RKNN api/driver version in logs.

### On-board checklist

1. **Startup**: `YoloAdapter` Init shows `model_path=./model/yolov5.rknn`; `output num: 3`, `out0` `dims[1]=255`.
2. **YOLO phase**: Periodic `det_lines>0`; COCO boxes on screen; `person` can enable scrfd.
3. **SCRFD phase**: Reasonable face count, not full screen.
4. **Exit**: Single `Ctrl+C` exits cleanly.
5. **Still 9 × `score_8`**: Check `path=` is not `scrfd.rknn`.

---

## Exit and crashes

### Ctrl+C does not exit

**Typical sign**: Repeated `Signal received, request stop` but process hangs.

**Root cause**: `PreprocessLoop` / `InferenceLoop` block forever on full `BoundedQueue::Push`; after `Stop()` the main thread may leave the display loop while preprocess still blocks on `Push`.

**Checks**:

- `pipeline.cpp` uses `TryPush(..., 100ms)` and checks `ShouldStop()` on timeout.
- `Stop()` posts `frame_id=-1` quit sentinels to `infer_queues_` and `post_queue_`.
- If LLM thread blocks: `LlmWorker::Shutdown` and `rkllm_abort`.

### ESC hang on exit

- Main thread runs OpenCV and stdin; blocked `rkllm_run` or display can make ESC unresponsive.
- `Pipeline::Stop()` order: `AbortActiveGeneration` → release camera → quit sentinels → `JoinWorkerThreads` → `display_.Shutdown()`.

### SIGSEGV on exit

**Typical sign**: Segfault after long run or on ESC / Ctrl+C.

**Debug**:

1. `setvbuf` + SIGSEGV handler in `app/main.cc` for backtrace.
2. Safe teardown order for GUI, camera, and workers (no display/worker races).
3. `yolo_adapter.cpp`: `is_quant` / `want_float` must match model outputs; no uninitialized `rknn_output`.

### Verifying a fix

1. Build: `cd runtime && ./build-linux.sh`.
2. Run: no SIGSEGV backtrace; ESC and Ctrl+C exit; periodic FPS (e.g. every 100 frames).
3. Regression: long run; multiple start/stop cycles.

### Current shutdown sequence (matches code)

1. `Pipeline::Stop()`: `LlmGreeting::AbortActiveGeneration`, `camera_.Release`, `frame_id=-1` sentinels.
2. Workers: `TryPush` timeout sees `ShouldStop()` and exits.
3. Before `main` returns: `tts_worker->Shutdown()`, `llm_worker->Shutdown()` (join infer thread, `rkllm_destroy`).
4. `display_.Shutdown()` closes OpenCV windows.

---

## Other issues

### Missing `.rkllm` or LLM load failure

- Vision (YOLO/SCRFD) should work; `LlmWorker` **stat** pre-check in `RequestInitializeAsync` skips `rkllm_init` when missing → vision-only degrade.
- Terminal: `SYS> 仅视觉模式（对话模型未加载）`; no “input channel ready” or static `AI>` greeting.
- See [architecture-and-runtime.md](architecture-and-runtime.md) §1, §5 and [llm-model-coordinator.md](llm-model-coordinator.md) §5–§6.

### Truncated LLM replies

- Check `max_new_tokens`; R1-style thinking consumes tokens → [llm-model-coordinator.md](llm-model-coordinator.md).

### TTS

- Voice UX and TTS debug: [tts-melotts.md](tts-melotts.md) §13.

---

*Authoritative source: current repository code.*
