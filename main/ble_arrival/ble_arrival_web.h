/**
 * @file ble_arrival_web.h
 * @brief 门口迎宾：固件自起的极简 Web 管理页（绑定信标 / 看状态 / 调阈值）
 *
 * 为什么自己起一个 httpd，而不是改官方配网页：
 *  - 官方配网页在外部组件 78/esp-wifi-connect 里，改它要 fork 组件，升级会打架；
 *  - 串口 console 被日志占用，拿来当交互通道不现实；
 *  - 自己起 httpd 不碰任何外部组件，局域网内用手机浏览器就能绑信标，
 *    顺便还能当调试状态页（RSSI 实时值、事件计数、丢包数）。
 *
 * 安全边界：只监听局域网，不做鉴权。任何能连到这台设备的人都能改绑定与阈值，
 * 所以默认端口 8080，不要把它映射到公网。
 */
#ifndef BLE_ARRIVAL_WEB_H_
#define BLE_ARRIVAL_WEB_H_

#include "esp_err.h"

namespace ble_arrival {

/**
 * @brief 启动 Web 管理页
 *
 * @return ESP_OK 成功；失败时只打印日志，不影响语音助手主功能。
 */
esp_err_t StartWebServer();

/** 停止 Web 管理页 */
void StopWebServer();

}  // namespace ble_arrival

#endif  // BLE_ARRIVAL_WEB_H_
