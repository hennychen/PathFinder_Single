#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t service_attitude_init(void);
bool service_attitude_is_ready(void);

/*
 * 姿态安装基准校准
 *
 * 设备实际使用场景为竖直安装 (屏幕面向用户), 传感器默认水平安装假设不再成立。
 * 校准时需保持设备静止并按实际安装姿态固定, 采集 N 组静止重力向量取均值,
 * 以该向量为 -Z 参考构造安装系正交基, 后续采样先旋转到该安装系再做姿态解算,
 * 使校准姿态下 roll/pitch 输出为 0。
 * 校准结果写入 NVS 持久记忆, 重启后自动加载, 无需重复校准。
 *
 * 返回:
 *   ESP_OK               校准成功并已记忆
 *   ESP_ERR_INVALID_STATE 姿态服务未就绪 (IMU 初始化失败)
 *   其他                  采样或 NVS 写入失败
 */
esp_err_t service_attitude_start_calibration(void);

/* 是否已存在有效的安装基准 (本次运行加载或校准得到) */
bool service_attitude_has_mount_reference(void);
