# YOLOv5 适配器

> 代码：`runtime/adapters/yolo/`。平台总览见 [系统架构与运行逻辑.md](系统架构与运行逻辑.md)。

## 源文件（本仓真源）


| 文件                     | 职责                                                     |
| ---------------------- | ------------------------------------------------------ |
| `yolo_postprocess.cpp` | 三尺度解码 `output[0..2]`，与正点原子 atk 例程逻辑对齐                  |
| `yolo_adapter.cpp`     | `init_yolov5_model`、RKNN 推理、`validate_yolov5_model_io` |


**修改检测逻辑时只改 `adapters/yolo/`**

## 模型与路径

- 板端仅使用 `**./model/yolov5.rknn**`、`**./model/coco_80_labels_list.txt**`
- **不要**用仓库根目录 `deploy/` 产物替换
- `**./model/` 目录勿随意更换文件**

## 后处理要点

- `post_process`：固定解码 `output[0..2]`，要求 `n_output==3` 且 `dims[1]==255`（Init 会校验，不满足直接失败）
- **禁止**：9 路 box/conf/cls 分离解码、按 buffer 大小乱猜 output 索引

## 预处理

- OpenCV **BGR** 帧直接作为 `IMAGE_FORMAT_RGB888` 送 letterbox（与正点原子例程一致，**不要** BGR2RGB）

## 平台扩展

- `GetAdapterSignals()`：`person_present` → `ModelCoordinator` 切换 SCRFD

排障见 [YOLO与SCRFD问题排查记录.md](YOLO与SCRFD问题排查记录.md)。