/*
 * app/main.cc — 进程入口：读配置、注册适配器、组装各模块并运行 Pipeline。
 */
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstdlib>

#include "engine/pipeline.h"
#include "platform/logging.h"
#include "platform/model_coordinator.h"
#include "capture/camera_source.h"
#include "capture/frame_transform.h"
#include "display/display_sink.h"
#include "display/display_layout.h"
#include "display/result_overlay.h"
#include "adapters/yolo/yolo_adapter.h"
#include "adapters/scrfd/scrfd_adapter.h"
#include "adapters/llm/llm_worker.h"
#include "adapters/tts/melotts_session.h"
#include "adapters/tts/tts_planner.h"
#include "adapters/tts/tts_worker.h"
#include "app/config_parser.h"

#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

static std::atomic<bool> g_stop_requested{false};
static std::atomic<int> g_sigint_count{0};

// 致命信号处理：打印回溯后立即退出，避免进程继续处于未定义状态。
static void segv_handler(int sig) {
    void* bt[20];
    int bt_size = backtrace(bt, 20);
    LogFatal("signal %d, backtrace:", sig);
    fprintf(stderr, "Fatal signal %d, backtrace:\n", sig);
    backtrace_symbols_fd(bt, bt_size, STDERR_FILENO);
    fsync(STDERR_FILENO);
    _exit(128 + sig);
}

// 软停止信号处理：第一次请求优雅退出，第二次强制退出。
static void stop_handler(int sig) {
    (void)sig;
    const int n = g_sigint_count.fetch_add(1) + 1;
    g_stop_requested.store(true);
    fprintf(stderr, "\nSignal received, request stop (workers will exit)...\n");
    if (n >= 2) {
        fprintf(stderr, "Second interrupt, forcing exit.\n");
        _exit(130);
    }
}

int main(int argc, char** argv) {
    // 配置路径：默认使用 config/default.yaml，允许命令行传入覆盖。
    std::string config_path = "config/default.yaml";
    if (argc == 2) {
        config_path = argv[1];
    }

    ConfigParser cfg;
    if (!cfg.LoadFromFile(config_path)) {
        std::cerr << "Failed to load config: " << config_path << std::endl;
        return -1;
    }

    // 模型类型：当前仅支持 "yolo"。
    std::string model_type = cfg.GetString("model.type");
    // YOLO 模型文件路径（RKNN）。
    std::string yolo_model_path = cfg.GetString("model.yolo.path");
    // SCRFD 模型文件路径（RKNN）。
    std::string scrfd_model_path = cfg.GetString("model.scrfd.path");
    // SCRFD 置信度阈值（百分比转 0~1 浮点）。
    float scrfd_conf_th = static_cast<float>(cfg.GetInt("model.scrfd.conf_threshold_percent")) / 100.0f;
    // SCRFD NMS 阈值（百分比转 0~1 浮点）。
    float scrfd_nms_th = static_cast<float>(cfg.GetInt("model.scrfd.nms_threshold_percent")) / 100.0f;
    // 推理线程数（每个启用槽位的工作线程配置）。
    int infer_threads = cfg.GetInt("system.infer_threads");
    // 是否始终启用 yolo 槽位（不随场景动态开关）。
    bool yolo_always_on = cfg.GetBool("system.slots.yolo_always_on");
    // 场景状态切换驻留帧数（防抖，避免频繁抖动切换）。
    int scene_dwell_frames = cfg.GetInt("system.slots.scene_dwell_frames");
    // 切换到 person_present 所需连续帧数。
    int switch_present_threshold = cfg.GetInt("system.switch.present_threshold");
    // 切换到 idle 所需连续帧数。
    int switch_absent_threshold = cfg.GetInt("system.switch.absent_threshold");
    // 是否使用单线程调度模式（调试/资源受限场景可用）。
    bool single_thread = cfg.GetBool("system.switch.single_thread");
    // 全局日志级别：debug/info/warn/error/fatal。
    std::string log_level = cfg.GetString("system.log_level");
    // NPU 核心绑定列表，例如 [0,1]。
    std::vector<int> npu_cores = cfg.GetIntArray("system.npu_cores");
    // 输入源：摄像头设备或视频文件路径。
    std::string input_source = cfg.GetString("input.source");
    // 采集宽度（像素）。
    int input_width = cfg.GetInt("input.width");
    // 采集高度（像素）。
    int input_height = cfg.GetInt("input.height");
    // 输入旋转：none/ccw90/cw90/180（兼容 0/90cw）。
    std::string input_rotate = cfg.GetString("input.rotate");
    // yolo person 类分数阈值（百分比）。
    int yolo_person_threshold_percent = cfg.GetInt("model.yolo.person_threshold_percent");
    // 是否显示可视化窗口（无 GUI 环境请设 false）。
    bool show_window = cfg.GetBool("input.show_window");
    // 显示目标屏幕宽度（像素）。
    int display_screen_w = cfg.GetInt("input.display.screen_width");
    // 显示目标屏幕高度（像素）。
    int display_screen_h = cfg.GetInt("input.display.screen_height");
    // 画面占屏最大比例（百分比）。
    int display_max_ratio_percent = cfg.GetInt("input.display.max_screen_ratio_percent");
    // 是否全屏显示。
    bool display_fullscreen = cfg.GetBool("input.display.fullscreen");
    // 标题栏保留像素（用于避免被系统栏遮挡）。
    int display_title_reserve = cfg.GetInt("input.display.title_bar_reserve_px");
    // 是否启用 LLM 模块。
    bool llm_enabled = cfg.GetBool("model.llm.enabled");
    LogInfo("Main: config %s model.llm.enabled=%s", config_path.c_str(),
            llm_enabled ? "true" : "false");
    // LLM 模型文件路径（.rkllm）。
    std::string llm_model_path = cfg.GetString("model.llm.path");
    // 单轮最大生成 token 数。
    int llm_max_new_tokens = cfg.GetInt("model.llm.max_new_tokens");
    // 上下文窗口长度上限。
    int llm_max_context_len = cfg.GetInt("model.llm.max_context_len");
    // 人脸稳定连续帧阈值（达到后打开对话门控）。
    int llm_face_stable_frames = cfg.GetInt("model.llm.face_stable_frames");
    // 人脸缺失连续帧阈值（达到后进入 Grace 宽限态）。
    int llm_face_absent_frames = cfg.GetInt("model.llm.face_absent_frames");
    // 人脸缺失后的会话宽限期（毫秒）。
    int llm_grace_timeout_ms = cfg.GetInt("model.llm.grace_timeout_ms");
    // 会话空闲超时（毫秒，超时回到锁定态）。
    int llm_idle_timeout_ms = cfg.GetInt("model.llm.idle_timeout_ms");
    // 是否在 SCRFD 激活时预加载 LLM。
    bool llm_preload_on_scrfd = cfg.GetBool("model.llm.preload_on_scrfd");
    // 是否程序启动后立即异步预加载 LLM。
    bool llm_preload_on_startup = cfg.GetBool("model.llm.preload_on_startup");
    // 自动问候语（检测到稳定人脸后输出）。
    std::string llm_auto_greeting_text = cfg.GetString("model.llm.auto_greeting_text");
    // 拼入 User 段末尾的约束文案（空串则 User 段仅含终端输入）。
    std::string llm_user_prompt_prefix = cfg.GetString("model.llm.user_prompt_prefix");
    bool llm_tts_enabled = cfg.GetBool("model.tts.enabled");
    bool llm_tts_skip_greeting = cfg.GetBool("model.tts.skip_static_greeting");
    int llm_tts_max_chars = cfg.GetInt("model.tts.max_speak_chars");
    int llm_tts_split_min_chars = cfg.GetInt("model.tts.split_min_chars", 4);
    int llm_tts_planner_zh_min = cfg.GetInt("model.tts.planner.zh_min_chars", 8);
    int llm_tts_planner_zh_max = cfg.GetInt("model.tts.planner.zh_max_chars", 15);
    int llm_tts_planner_en_min = cfg.GetInt("model.tts.planner.en_min_words", 4);
    int llm_tts_planner_en_max = cfg.GetInt("model.tts.planner.en_max_words", 8);
    int llm_tts_planner_fallback_ms = cfg.GetInt("model.tts.planner.fallback_timeout_ms", 600);
    bool llm_tts_preload = cfg.GetBool("model.tts.preload_on_startup");
    float llm_tts_speed = static_cast<float>(std::atof(cfg.GetString("model.tts.speed", "1.0").c_str()));
    if (llm_tts_speed <= 0.0f) {
        llm_tts_speed = 1.0f;
    }
    bool llm_tts_visual_throttle = cfg.GetBool("model.tts.qos.enable_visual_throttle", true);
    int llm_tts_low_watermark_chunks = cfg.GetInt("model.tts.qos.low_watermark_chunks", 1);
    int llm_tts_high_watermark_chunks = cfg.GetInt("model.tts.qos.high_watermark_chunks", 3);
    int llm_tts_min_start_chunks = cfg.GetInt("model.tts.qos.min_start_pcm_chunks", 2);

    std::shared_ptr<IModelAdapter> base_adapter;
    if (model_type == "yolo") {
        auto yolo = std::make_shared<YoloAdapter>();
        yolo->SetPersonScoreThreshold(yolo_person_threshold_percent / 100.0f);
        base_adapter = yolo;
    } else {
        std::cerr << "Unsupported model type: " << model_type << std::endl;
        return -1;
    }

    try {
        SetLogLevelByName(log_level);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        signal(SIGSEGV, segv_handler);
        signal(SIGINT, stop_handler);
        signal(SIGTERM, stop_handler);

        ModelCoordinator coordinator;
        coordinator.SetSlotOptions(yolo_always_on);
        coordinator.SetSceneDwellFrames(scene_dwell_frames);
        coordinator.SetTtsVisualThrottleEnabled(llm_tts_visual_throttle);

        std::shared_ptr<LlmWorker> llm_worker;
        std::shared_ptr<TtsWorker> tts_worker;
        coordinator.GetLlmGreeting().SetTriggerThreshold(llm_face_stable_frames);
        coordinator.GetLlmGreeting().SetFaceAbsentThreshold(llm_face_absent_frames);
        coordinator.GetLlmGreeting().SetGraceTimeoutMs(llm_grace_timeout_ms);
        coordinator.GetLlmGreeting().SetIdleTimeoutMs(llm_idle_timeout_ms);
        coordinator.GetLlmGreeting().SetPreloadOnScrfd(llm_preload_on_scrfd);
        coordinator.GetLlmGreeting().SetAutoGreetingText(llm_auto_greeting_text);
        if (llm_enabled) {
            llm_worker = std::make_shared<LlmWorker>();
            llm_worker->Configure(llm_model_path, llm_max_new_tokens, llm_max_context_len,
                                  llm_user_prompt_prefix);
            coordinator.GetLlmGreeting().SetLlmWorker(llm_worker.get());
            if (llm_preload_on_startup) {
                llm_worker->RequestInitializeAsync();
            }
            if (llm_tts_enabled) {
                MeloTtsConfig tts_cfg;
                tts_cfg.encoder_path = cfg.GetString("model.tts.encoder_path");
                tts_cfg.decoder_path = cfg.GetString("model.tts.decoder_path");
                tts_cfg.lexicon_path = cfg.GetString("model.tts.lexicon_path");
                tts_cfg.tokens_path = cfg.GetString("model.tts.tokens_path");
                tts_cfg.language = cfg.GetString("model.tts.language");
                tts_cfg.speak_id = cfg.GetInt("model.tts.speak_id");
                tts_cfg.speed = llm_tts_speed;
                tts_cfg.disable_bert = cfg.GetBool("model.tts.disable_bert");
                tts_cfg.split_min_chars = llm_tts_split_min_chars;
                tts_worker = std::make_shared<TtsWorker>();
                tts_worker->Configure(tts_cfg, llm_tts_max_chars);
                tts_worker->SetPlaybackProtectionThresholds(
                    static_cast<size_t>(llm_tts_low_watermark_chunks),
                    static_cast<size_t>(llm_tts_high_watermark_chunks));
                tts_worker->SetMinStartPcmChunks(static_cast<size_t>(llm_tts_min_start_chunks));
                llm_worker->SetTtsWorker(tts_worker.get());
                TtsPlannerConfig planner_cfg;
                planner_cfg.zh_min_chars = static_cast<size_t>(std::max(1, llm_tts_planner_zh_min));
                planner_cfg.zh_max_chars = static_cast<size_t>(std::max(1, llm_tts_planner_zh_max));
                planner_cfg.en_min_words = static_cast<size_t>(std::max(1, llm_tts_planner_en_min));
                planner_cfg.en_max_words = static_cast<size_t>(std::max(1, llm_tts_planner_en_max));
                planner_cfg.fallback_timeout_ms = std::max(100, llm_tts_planner_fallback_ms);
                llm_worker->ConfigureTtsPlanner(planner_cfg);
                llm_worker->SetTtsEnabled(true);
                coordinator.GetLlmGreeting().SetTtsWorker(tts_worker.get(),
                                                          llm_tts_skip_greeting);
                if (llm_tts_preload) {
                    tts_worker->RequestInitializeAsync();
                }
                LogInfo("Main: TTS enabled encoder=%s", tts_cfg.encoder_path.c_str());
            }
            LogInfo("Main: LLM configured path=%s preload_on_startup=%d preload_on_scrfd=%d",
                    llm_model_path.c_str(), llm_preload_on_startup ? 1 : 0,
                    llm_preload_on_scrfd ? 1 : 0);
        } else {
            LogInfo("Main: LLM disabled (model.llm.enabled=false)");
        }

        CameraSource camera(input_source, input_width, input_height);
        FrameTransform frame_transform(input_rotate);
        ResultOverlay overlay;
        DisplayWindowConfig display_cfg;
        display_cfg.enabled = show_window;
        display_cfg.screen_width = display_screen_w;
        display_cfg.screen_height = display_screen_h;
        display_cfg.max_screen_ratio =
            std::max(10, std::min(100, display_max_ratio_percent)) / 100.0f;
        display_cfg.fullscreen = display_fullscreen;
        display_cfg.title_bar_reserve_px = display_title_reserve;
        std::unique_ptr<IDisplaySink> display = CreateOpenCVDisplaySink(display_cfg);

        Pipeline pipeline(coordinator, camera, frame_transform, overlay, *display,
                          base_adapter, yolo_model_path, infer_threads, npu_cores, single_thread);
        pipeline.SetExternalStopFlag(&g_stop_requested);

        pipeline.RegisterFactory("scrfd",
                                 [scrfd_conf_th, scrfd_nms_th]() {
                                     auto adapter = std::make_shared<ScrfdAdapter>();
                                     adapter->SetThresholds(scrfd_conf_th, scrfd_nms_th);
                                     return adapter;
                                 },
                                 scrfd_model_path);
        pipeline.SetSwitchDebounceThresholds(switch_present_threshold, switch_absent_threshold);

        LogInfo("Main: warming up scrfd slot...");
        coordinator.WarmupSlot("scrfd");

        LogInfo("Main: yolo=%s scrfd=%s infer_threads=%d llm=%s",
                yolo_model_path.c_str(), scrfd_model_path.c_str(), infer_threads,
                llm_enabled ? "on" : "off");

        pipeline.Run();
        if (tts_worker) {
            tts_worker->Shutdown();
        }
        if (llm_worker) {
            llm_worker->Shutdown();
        }
    } catch (const std::exception& e) {
        LogError("Pipeline failed: %s", e.what());
        std::cerr << "Pipeline failed: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}
