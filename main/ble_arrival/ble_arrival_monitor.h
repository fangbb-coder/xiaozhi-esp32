/**
 * @file ble_arrival_monitor.h
 * @brief 门口迎宾：BLE 扫描 + 到达判定 + 播报
 *
 * 数据流：
 *
 *   射频 -> Bluedroid GAP 回调(蓝牙任务)
 *            |  只做「轻量过滤 + 入队」，绝不做耗时操作
 *            v
 *         FreeRTOS 队列(64 条)
 *            |
 *            v
 *   监控任务(100ms tick)
 *            |  匹配目标 -> arrival_detector_feed -> 到达/离开事件
 *            v
 *        播报回调 -> Application::Alert(屏幕文字 + 提示音)
 *
 * 三条实战约束（官方文档没强调，但踩过坑）：
 *  1. esp_ble_gap_set_scan_params() 必须在 esp_ble_gap_register_callback() 之后调用，
 *     否则参数设置会被忽略/报错。
 *  2. GAP 回调运行在蓝牙协议栈任务里，**绝不能**在里面做耗时操作
 *     （日志、显示、文件、网络都不行），否则会拖垮蓝牙栈。
 *  3. ESP32-S3 只有一颗 2.4GHz 射频，Wi-Fi 与 BLE 分时复用。
 *     官方矩阵里「BLE Scan + Wi-Fi STA 已连接」是 C1（可用但性能不稳定），
 *     所以这里用短占空比扫描，并在设备说话/收音时主动暂停扫描。
 */
#ifndef BLE_ARRIVAL_MONITOR_H_
#define BLE_ARRIVAL_MONITOR_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "arrival_detector.h"
#include "arrival_target.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace ble_arrival {

/** 播报事件类型 */
enum class AnnounceKind {
    kArrived = 0, /**< 有人到了门口 */
    kLeft = 1,    /**< 人离开了门口 */
};

/** 未绑定时的候选设备（用于自动发现该绑哪个信标） */
struct ScanCandidate {
    uint8_t mac[6] = {0};
    uint32_t count = 0;      /**< 观测到的广播次数 */
    int8_t best_rssi = 0;    /**< 历史最强 RSSI */
    int8_t last_rssi = 0;    /**< 最近一次 RSSI */
    bool has_ibeacon = false;
    uint8_t uuid[16] = {0};
    uint16_t major = 0;
    uint16_t minor = 0;
    uint32_t last_seen_ms = 0;
};

/** 监控器状态快照（给 Web 页面 / 串口日志） */
struct MonitorStatus {
    bool started = false;
    bool scanning = false;
    bool bound = false;
    std::string target_desc;
    std::string target_kind;  /**< "mac" / "ibeacon" / "none" */
    int8_t filtered_rssi = ARRIVAL_RSSI_INVALID;
    int8_t last_raw_rssi = 0;
    arrival_state_t state = ARRIVAL_STATE_UNKNOWN;
    uint32_t arrived_count = 0;
    uint32_t left_count = 0;
    uint32_t samples = 0;     /**< 送进判定器的有效样本数 */
    uint32_t matched_total = 0;/**< 匹配上的广播总数 */
    uint32_t dropped = 0;     /**< 队列满被丢弃的广播数 */
    uint32_t scan_pauses = 0; /**< 因设备繁忙而暂停扫描的次数 */
    arrival_config_t cfg{};
};

class ArrivalMonitor {
public:
    /** 播报回调：kind 决定用哪个提示音，status 是屏幕状态行，message 是屏幕正文 */
    using AnnounceCallback = std::function<void(AnnounceKind kind, const char* status, const char* message)>;
    /** 设备繁忙回调：返回 true 时暂停 BLE 扫描（保护 Wi-Fi 音频通道） */
    using BusyCallback = std::function<bool()>;

    static ArrivalMonitor& GetInstance();

    /** 启动蓝牙栈、扫描与检测任务 */
    esp_err_t Start(AnnounceCallback announce, BusyCallback busy);

    /** 停止（一般只在异常路径用） */
    void Stop();

    /** 运行时绑定目标（写 NVS + 立即生效） */
    bool Bind(const std::string& text, std::string* error);

    /** 解除绑定 */
    bool Unbind();

    /** 读取当前目标；未绑定返回 false */
    bool GetTarget(BeaconTarget* out) const;

    /** 读取当前判定参数 */
    bool GetConfig(arrival_config_t* out) const;

    /** 在线调整阈值（写 NVS + 立即生效） */
    bool SetThresholds(int enter_threshold, int hysteresis_db, std::string* error);

    /** 取状态快照 */
    void GetStatus(MonitorStatus* out) const;

    /** 取候选设备列表（按观测次数降序） */
    void GetCandidates(std::vector<ScanCandidate>* out) const;

    /**
     * @brief 蓝牙协议栈的扫描结果回调入口
     *
     * 仅供内部 GAP 回调使用：只做解析与入队，不允许在这里做耗时操作。
     */
    void OnScanResult(const uint8_t* bda, int rssi, const uint8_t* ble_adv, uint8_t adv_data_len);

    ArrivalMonitor(const ArrivalMonitor&) = delete;
    ArrivalMonitor& operator=(const ArrivalMonitor&) = delete;

private:
    ArrivalMonitor() = default;
    ~ArrivalMonitor();

    void WorkerTask();
    bool TargetMatchesLocked(const uint8_t* bda, const uint8_t* uuid, bool has_ibeacon, uint16_t major,
                             uint16_t minor);
    void UpdateCandidateLocked(const uint8_t* bda, int8_t rssi, bool has_ibeacon, const uint8_t* uuid,
                               uint16_t major, uint16_t minor, uint32_t now_ms);

    AnnounceCallback announce_;
    BusyCallback busy_;
    SemaphoreHandle_t mutex_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;

    BeaconTarget target_;
    arrival_config_t cfg_{};
    arrival_detector_t detector_{};

    bool started_ = false;
    bool scanning_ = false;
    bool bt_ready_ = false;

    uint32_t matched_total_ = 0;
    uint32_t dropped_ = 0;
    uint32_t scan_pauses_ = 0;

    ScanCandidate candidates_[16];
    int candidate_count_ = 0;
};

}  // namespace ble_arrival

#endif  // BLE_ARRIVAL_MONITOR_H_
