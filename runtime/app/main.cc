/*
 * app/main.cc — 进程入口：读配置、注册适配器、组装各模块并运行 Pipeline。
 */
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>

#include "engine/pipeline.h"
#include "platform/logging.h"
#include "platform/model_coordinator.h"
#include "io/camera_source.h"
#include "io/frame_transform.h"
#include "viz/display_sink.h"
#include "viz/display_layout.h"
#include "viz/result_overlay.h"
#include "adapters/yolo/yolo_adapter.h"
#include "adapters/scrfd/scrfd_adapter.h"
#include "adapters/ppocr/ppocr_adapter.h"
#include "adapters/ppocr/text_region_probe.h"
#include "adapters/llm/llm_worker.h"
#include "utils/config_parser.h"

#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

static std::atomic<bool> g_stop_requested{false};
static std::atomic<int> g_sigint_count{0};
// SIGSEGV 打印回溯并立即退出，便于板端定位崩溃。
static void segv_handler(int sig) {
    void* bt[20];
    int bt_size = backtrace(bt, 20);
    LogFatal("signal %d, backtrace:", sig);
    fprintf(stderr, "Fatal signal %d, backtrace:\n", sig);
    backtrace_symbols_fd(bt, bt_size, STDERR_FILENO);
    fsync(STDERR_FILENO);
    _exit(128 + sig);
}

// 第一次 SIGINT 请求协作退出；第二次若仍卡在 mutex/RKNN 则强制 _exit。
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
    std::string config_path = "config/default.yaml";
    if (argc == 2) {
        config_path = argv[1];
    }

    ConfigParser cfg;
    if (!cfg.LoadFromFile(config_path)) {
        std::cerr << "Failed to load config: " << config_path << std::endl;
        return -1;
    }

    std::string model_type = cfg.GetString("model.type", "yolo");
    std::string yolo_model_path = cfg.GetString("model.yolo.path", "./model/yolov5.rknn");
    std::string scrfd_model_path = cfg.GetString("model.scrfd.path", "./model/scrfd.rknn");
    float scrfd_conf_th = static_cast<float>(cfg.GetInt("model.scrfd.conf_threshold_percent", 50)) / 100.0f;
    float scrfd_nms_th = static_cast<float>(cfg.GetInt("model.scrfd.nms_threshold_percent", 50)) / 100.0f;
    std::string ppocr_det_path = cfg.GetString("model.ppocr.det_path", "./model/ppocrv4_det.rknn");
    std::string ppocr_rec_path = cfg.GetString("model.ppocr.rec_path", "./model/ppocrv4_rec.rknn");
    int infer_threads = cfg.GetInt("system.infer_threads", 1);
    bool yolo_always_on = cfg.GetInt("system.slots.yolo_always_on", 1) != 0;
    bool document_disable_yolo = cfg.GetInt("system.slots.document_disable_yolo", 1) != 0;
    int scene_dwell_frames = cfg.GetInt("system.slots.scene_dwell_frames", 5);
    int text_probe_interval = cfg.GetInt("system.slots.text_probe_interval", 5);
    int text_probe_min_infer_w = cfg.GetInt("system.slots.text_probe_min_infer_width", 640);
    int switch_present_threshold = cfg.GetInt("system.switch.present_threshold", 15);
    int switch_absent_threshold = cfg.GetInt("system.switch.absent_threshold", 30);
    int switch_text_absent_threshold = cfg.GetInt("system.switch.text_absent_threshold", 30);
    bool single_thread = cfg.GetInt("system.switch.single_thread", 0) != 0;
    std::vector<int> npu_cores = cfg.GetIntArray("system.npu_cores");
    if (npu_cores.empty()) {
        npu_cores = {0, 1, 2};
    }
    std::string input_source = cfg.GetString("input.source", "/dev/video0");
    int input_width = cfg.GetInt("input.width", 0);
    int input_height = cfg.GetInt("input.height", 0);
    std::string input_rotate = cfg.GetString("input.rotate", "ccw90");
    std::string to_ppocr = cfg.GetString("system.switch.to_ppocr", "text");
    int yolo_person_threshold_percent = cfg.GetInt("model.yolo.person_threshold_percent", 35);
    bool show_window = cfg.GetInt("input.show_window", 1) != 0;
    int display_screen_w = cfg.GetInt("input.display.screen_width", 1080);
    int display_screen_h = cfg.GetInt("input.display.screen_height", 1920);
    int display_max_ratio_percent = cfg.GetInt("input.display.max_screen_ratio_percent", 85);
    bool display_fullscreen = cfg.GetInt("input.display.fullscreen", 0) != 0;
    int display_title_reserve = cfg.GetInt("input.display.title_bar_reserve_px", 56);
    float ppocr_det_th = static_cast<float>(cfg.GetInt("model.ppocr.det_threshold_percent", 20)) / 100.0f;
    float ppocr_box_th = static_cast<float>(cfg.GetInt("model.ppocr.box_threshold_percent", 45)) / 100.0f;
    float ppocr_rec_th = static_cast<float>(cfg.GetInt("model.ppocr.rec_score_threshold_percent", 35)) / 100.0f;
    int ppocr_min_infer_w = cfg.GetInt("model.ppocr.min_infer_width", 960);
    int ocr_log_interval = cfg.GetInt("input.log_ocr_interval_frames", 30);
    bool auto_back_to_yolo = cfg.GetInt("system.switch.auto_back_to_yolo", 1) != 0;
    int min_ppocr_frames = cfg.GetInt("system.switch.min_ppocr_frames", 90);
    std::string back_to_yolo_on = cfg.GetString("system.switch.back_to_yolo_on", "no_text");
    bool llm_enabled = cfg.GetInt("model.llm.enabled", 0) != 0;
    std::string llm_model_path = cfg.GetString("model.llm.path", "./model/deepseek.rkllm");
    int llm_max_new_tokens = cfg.GetInt("model.llm.max_new_tokens", 64);
    int llm_max_context_len = cfg.GetInt("model.llm.max_context_len", 4096);
    int llm_face_stable_frames = cfg.GetInt("model.llm.face_stable_frames", 10);
    std::string llm_greeting_prompt = cfg.GetString(
        "model.llm.greeting_prompt",
        "请用一句简短、自然的中文向镜头前的人问好，不要超过二十个字。");

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
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        signal(SIGSEGV, segv_handler);
        signal(SIGINT, stop_handler);
        signal(SIGTERM, stop_handler);

        ModelCoordinator coordinator;
        auto text_probe = std::make_shared<PpocrTextRegionProbe>();
        int probe_core = npu_cores.empty() ? 0 : npu_cores[0];
        int probe_mask = 1 << probe_core;
        if (text_probe->Init(ppocr_det_path, probe_mask, ppocr_det_th, ppocr_box_th,
                             text_probe_min_infer_w) != 0) {
            LogWarn("Main: text region probe init failed, OCR trigger may not work in text mode");
        }
        coordinator.SetTextRegionProbe(text_probe);
        coordinator.SetSlotOptions(yolo_always_on, text_probe_interval, document_disable_yolo);
        coordinator.SetSceneDwellFrames(scene_dwell_frames);

        std::shared_ptr<LlmWorker> llm_worker;
        coordinator.GetLlmGreeting().SetTriggerThreshold(llm_face_stable_frames);
        coordinator.GetLlmGreeting().SetGreetingPrompt(llm_greeting_prompt);
        if (llm_enabled) {
            llm_worker = std::make_shared<LlmWorker>();
            if (llm_worker->Init(llm_model_path, llm_max_new_tokens, llm_max_context_len) != 0) {
                LogWarn("Main: LLM init failed path=%s", llm_model_path.c_str());
                llm_worker.reset();
            } else {
                coordinator.GetLlmGreeting().SetLlmWorker(llm_worker.get());
                LogInfo("Main: LLM ready path=%s", llm_model_path.c_str());
            }
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
        pipeline.SetOcrLogIntervalFrames(ocr_log_interval);

        pipeline.RegisterFactory("scrfd",
                                 [scrfd_conf_th, scrfd_nms_th]() {
                                     auto adapter = std::make_shared<ScrfdAdapter>();
                                     adapter->SetThresholds(scrfd_conf_th, scrfd_nms_th);
                                     return adapter;
                                 },
                                 scrfd_model_path);
        pipeline.RegisterFactory("ppocr",
                                 [ppocr_det_path, ppocr_rec_path, ppocr_det_th, ppocr_box_th, ppocr_rec_th,
                                  ppocr_min_infer_w]() {
                                     auto adapter = std::make_shared<PPOCRAdapter>(ppocr_det_path, ppocr_rec_path);
                                     adapter->SetDetThresholds(ppocr_det_th, ppocr_box_th, ppocr_rec_th);
                                     adapter->SetMinInferWidth(ppocr_min_infer_w);
                                     return adapter;
                                 },
                                 ppocr_det_path);
        pipeline.SetSwitchDebounceThresholds(switch_present_threshold, switch_absent_threshold,
                                             switch_text_absent_threshold);
        pipeline.SetToPpocrMode(to_ppocr);
        const bool back_on_no_text = (back_to_yolo_on == "no_text");
        if (back_to_yolo_on == "never") {
            auto_back_to_yolo = false;
        }
        pipeline.SetPpocrBackPolicy(auto_back_to_yolo, min_ppocr_frames, back_on_no_text);

        LogInfo("Main: warming up scrfd/ppocr slots (avoid runtime freeze on switch)...");
        coordinator.WarmupSlot("scrfd");
        coordinator.WarmupSlot("ppocr");

        LogInfo("Main: slot mode A yolo=%s scrfd=%s ppocr det=%s to_ppocr='%s' infer_threads=%d",
                yolo_model_path.c_str(), scrfd_model_path.c_str(), ppocr_det_path.c_str(),
                to_ppocr.c_str(), infer_threads);

        pipeline.Run();
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
