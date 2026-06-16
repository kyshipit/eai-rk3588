Language: **English** | [中文](tts-melotts_CN.md)

# TTS and voice dialogue design

> **Sole TTS design and acceptance document**. Code: `runtime/voice/` + `runtime/adapters/melotts/`; config: `runtime/config/default.yaml` → `model.tts` (requires `model.llm.enabled: true` at startup).

---

## 1. Target experience

On-board face-gated reference app:

```text
Stable face → static greeting (text + voice via PlayText)
→ YOU> → streaming AI> → TtsPlanner → Melo synth → continuous formal speech
```

User perception:

- Greeting (e.g. “您好，需要帮忙吗”) is **clear from the first syllable**;
- Formal answers **without mid-sentence dropouts**; **opening text at sentence start is audible**;
- New `YOU>` preempts old answer (`generation_`);
- Grace/Locked rejects new input after face leave.

**Principle**: **Audible continuity and intelligible onsets** over “play chunks ASAP”. Melo decoder ~**1.6–2.2 s** per call, multi-sentence **serial only** — Planner, synth, and playback must align.

Do not speak: `SYS>`, user input, vision debug, `<think>`. `TtsTextSanitizer` strips emoji before synth.

Stack: **MeloTTS `ZH_MIX_EN`** (RKNN + lexicon). Side path to RKLLM, not in vision Pipeline.

Requires **`gst-launch-1.0`**.

---

## 2. Acceptance criteria

### Must pass

| Item | Criterion |
|------|-----------|
| Static greeting | Text + voice after stable face; clear onset (baseline vs formal) |
| Formal continuity | Short answers play as one piece; long answers merge PCM per job — **no chunk gaps** |
| Onset | First 3–4 chars/words **audible** (not screen-only) |
| English | No Planner **word-level** cuts; short English uses single-shot |
| Gate | Reject input after Grace timeout |
| Vision | No `mode:none`; TTS active → skip **yolo infer only**, keep scrfd |
| Preemption | New `YOU>` → `Cancel()` + `generation_++` |
| Config | `enabled=false` → no TTS; `skip_static_greeting=true` → no greet |

### Acceptable trade-offs

| Item | Note |
|------|------|
| Formal latency | LLM FINISH + serial Melo (~2 s per decoder on short answers) |
| Long answers | Wait for job merge before play vs underrun from streaming chunks |
| Screen vs ear | `AI>` streams before TTS; **accept by ear** |

### Not sole Pass metrics

| Item | Note |
|------|------|
| `pcm underrun` log | Counted only when queue empty and synth still busy; **underrun=0 ≠ ear Pass** |
| `enqueued == played` ms | PCM reached gst; does not guarantee loud enough onset |

---

## 3. Module boundaries

| Module | Role |
|--------|------|
| `tts_ingress.*` | Filter thinking/tags; UTF-8; LLM chunk events |
| `tts_planner.*` | zh/en planning; short-answer FINISH defer; long-answer stream emit |
| `tts_worker.*` | Static / FormalAnswer queues; synth + play threads; `generation_` |
| `melotts_session.*` | RKNN; `split_sentence` or **single-shot**; streaming callbacks |
| `audio_player.*` | Single gst pipe; idle priming; `Cancel` keeps pipe |
| `tts_text_sanitizer.*` | Strip emoji; `max_speak_chars` |
| `lexicon.hpp` / `split.hpp` | Lexicon; Melo internal split |

Platform:

- **LLM**: `OnLlmChunk` → main `PollDeferred` → Ingress/Planner → `EnqueueFormalAnswer`.
- **Greeting**: `SetBannerLine` → **`PlayText` (Static)** — no Planner; same chain as short formal.
- **Preemption**: `SubmitPrompt` → **`Cancel()`** (clear queue + `generation_++`, **keep** gst).
- **Vision**: Skip yolo infer when TTS active during face dialogue.

---

## 4. Data flow

```text
[Greeting] stable face
  → SetBannerLine → PlayText (Static) → Melo → PushPcm(Static) → PlaybackLoop → gst

[Formal] YOU> accepted
  → SubmitPrompt → Cancel()
  → rkllm_run → OnLlmChunk → Ingress → Planner
       · ≤ short_answer_max_chars: Feed no emit, FINISH flush whole text
       · longer: stream emit by zh/en + fallback_timeout_ms
  → EnqueueFormalAnswer
       · ≤ single_shot_max_chars: enqueue as Static (same as PlayText)
       · longer: FormalAnswer
  → SynthesizeLoop
       · Static / short formal: trim absolute silence → PushPcm; Melo single-shot when short
       · long formal: merge PCM in job → one PushPcm
  → PlaybackLoop (play when queue non-empty) → AudioPlayer (idle prime) → gst
```

---

## 5. Static greeting vs formal answer

| | `PlayText` greeting | `EnqueueFormalAnswer` |
|--|---------------------|------------------------|
| Trigger | Stable face, banner + TTS together | Planner segments after LLM |
| Queue kind | Always Static | Short → **Static**; long → FormalAnswer |
| Synth | Streaming; `TrimAbsoluteLeadingSilence` only | Short = Static; long = merge in job |
| Before play | No `Cancel` | **`Cancel()` on SubmitPrompt** |

**Board debug lessons**:

- Greeting OK + formal onset missing → **Static vs Formal path**, not “Melo always weak”.
- After `Cancel()`, long LLM gap with **no gst writes** → next `PlayPcm` may **lose PCM head** → **idle priming** (≥300 ms gap, ~100 ms discardable silence). Log: `AudioPlayer: primed stream`.
- **RMS/peak trim on merged PCM** deleted weak first sentence when second was louder — **forbidden**.

---

## 6. TtsIngress / TtsTextSanitizer

- Ingress: strip thinking; UTF-8 safe; no hard time slice.
- Sanitizer: strip **4-byte UTF-8 emoji**; truncate by `max_speak_chars`.

---

## 7. TtsPlanner

`Feed` → `TryEmitSegments`; `FINISH` → `Flush`.

| Lang | Policy |
|------|--------|
| Chinese | Sentence first; `zh_max_chars` or `zh_min_chars` + `fallback_timeout_ms` |
| English | **No** word-level emit |

**Short-answer defer** (`short_answer_max_chars`, default 96): while pending ≤ threshold, **Feed does not emit**; whole text on **FINISH** only — avoids 800 ms mid-stream splits on short replies.

Long answers stream; `CoalesceFormalAnswerLocked` merges adjacent Formal jobs before synth.

---

## 8. Melo synthesis

| Condition | Behavior |
|-----------|----------|
| `utf8_strlen(text) ≤ single_shot_max_chars` (96) | One `SynthesizeOneSentenceUnlocked`, **no** `split_sentence` |
| Longer | `split_sentence` → serial decoders |

Limits:

- ~1.6–2.2 s per decoder; serial multi-sentence.
- Text over `PREDICTED_LENGTHS_MAX` **truncates tail** — do not single-shot overlong text.

---

## 9. TtsWorker playback

Two threads: `SynthesizeLoop` + `PlaybackLoop`.

| Case | Synth | Play |
|------|-------|------|
| Static / short formal | single-shot or split; **absolute silence trim only** | play when `pcm_queue` non-empty |
| Long formal | merge chunks in job → one PushPcm | same |

**Do not** (reverted failures):

- Formal streaming multiple PCM chunks (underrun / eaten words);
- RMS trim on merged PCM;
- unbounded trim / whole long answer single-shot over model limit.

**Onset trim**: `TrimAbsoluteLeadingSilence` removes near-zero samples only (`<5e-5`), 10 ms pad.

---

## 10. AudioPlayer (gst)

- Persistent `gst-launch-1.0`; `Cancel` does not kill child.
- **Cold prime**: ~50 ms silence after new pipe.
- **Idle prime**: if ≥ **300 ms** since last PCM (typical after `Cancel` + LLM), write ~100 ms silence before real PCM.

---

## 11. Vision / gate

TTS active + face dialogue → skip yolo infer only; scrfd and Grace/Locked unchanged.

---

## 12. Configuration

See [`default.yaml`](../runtime/config/default.yaml) → `model.tts`.

| Field | Meaning |
|-------|---------|
| `single_shot_max_chars` | One decoder if text ≤ this (default 96; ≥ `planner.short_answer_max_chars`) |
| `planner.short_answer_max_chars` | Defer stream emit; flush on FINISH |
| `split_min_chars` | Melo internal split for **long** text |
| `planner.*` | Long-answer stream thresholds |

---

## 13. Logs and troubleshooting

| Log | Meaning |
|-----|---------|
| `first pcm enqueued/played in X ms` | Includes serial Melo time |
| `pcm underrun #N` | Queue empty while synth pending |
| `AudioPlayer: primed stream` | Idle priming before real PCM |
| Multiple `inference_decoder_model` | Text over single-shot; short reply should be **once** |
| `predicted_lengths_max_real > PREDICTED_LENGTHS_MAX` | Tail truncated |

### Inaudible opening / only hear latter half

1. Compare **greeting PlayText** vs formal — path / idle priming / `primed stream` log.
2. **Decoder count** on short reply — should be 1; check `single_shot_max_chars` deployed.
3. No RMS trim on merge.
4. Underrun + choppy → need merge-in-job, not stream chunks.
5. OOV / English lexicon.

### Issue / fix summary

| Symptom | Cause | Fix |
|---------|-------|-----|
| Mid-sentence gaps | Streaming formal PCM chunks | Merge per job, one PlayPcm |
| Short answer: silent onset + choppy mid | Planner 800 ms split + stream play | `short_answer_max_chars`; short → Static queue |
| Inaudible sentence onset | Formal vs Static path; gst idle after `Cancel` | Short → Static; idle priming |
| Whole sentence gone | RMS trim on merge | Reverted; absolute silence trim only |

Platform: [architecture-and-runtime.md](architecture-and-runtime.md) §8.

---

*Code wins on conflict; acceptance per §2.*
