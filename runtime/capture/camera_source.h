/*
 * capture/camera_source.h
 * V4L2 / OpenCV 摄像头采集（打开、读帧、释放）。
 */
#pragma once

#include <atomic>
#include <opencv2/opencv.hpp>
#include <string>

class CameraSource {
public:
    // source 可为设备路径（/dev/videoX）、数字索引字符串或视频文件路径。
    CameraSource(const std::string& source, int width = 0, int height = 0);

    // 打开输入源并按需设置分辨率。
    bool Open();
    // 释放采集资源。
    void Release();
    // 查询当前是否已打开。
    bool IsOpened() const;
    // 读取一帧；若 stop_flag 触发则尽快返回 false。
    bool ReadFrame(cv::Mat& frame, const std::atomic<bool>* stop_flag);

private:
    std::string source_;
    int width_ = 0;
    int height_ = 0;
    cv::VideoCapture capture_;
};
