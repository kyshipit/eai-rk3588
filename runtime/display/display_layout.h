/*
 * display/display_layout.h — 5.5 寸竖屏等场景的预览窗口布局参数
 */
#pragma once

struct DisplayWindowConfig {
    // 是否启用窗口显示。
    bool enabled = true;
    // 目标屏幕宽高（用于窗口布局计算）。
    int screen_width = 1080;
    int screen_height = 1920;
    // 预览窗口占屏比例上限。
    float max_screen_ratio = 0.85f;
    // 是否全屏显示。
    bool fullscreen = false;
    int title_bar_reserve_px = 56;  // 顶部留白，避免挡住窗口标题栏按钮
};
