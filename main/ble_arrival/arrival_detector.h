/**
 * @file arrival_detector.h
 * @brief 门口迎宾：BLE RSSI 到达判定核心
 *
 * 设计约束：
 *  - 纯 C99，无 ESP-IDF 依赖、无 STL、无动态内存 —— 既能在 ESP32-S3 固件里用，
 *    也能在主机侧（gcc/clang）单独编译做单元测试与回放校验。
 *  - 所有窗口都是定长数组，最大尺寸由宏决定，栈/静态占用可预估。
 *  - 判定链路：原始 RSSI -> 中值滤波(去毛刺) -> 滑动平均(平滑) -> 迟滞双阈值 ->
 *    连续命中计数 -> 冷却期 -> 事件。
 *
 * 为什么不直接用原始 RSSI 判阈值：
 *  实测（2026-11-23 办公环境，Realtek 适配器，78 个 BLE 设备）固定设备的 RSSI
 *  标准差可以低到 0.88 dB，但会移动的手表是 6.34 dB。原始 RSSI 单点越过阈值
 *  极易误触发，必须先滤掉毛刺再判。
 */
#ifndef ARRIVAL_DETECTOR_H_
#define ARRIVAL_DETECTOR_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 中值滤波窗口上限（建议奇数，偶数时取中间两个的算术平均） */
#define ARRIVAL_MAX_MEDIAN_WINDOW 15
/** 滑动平均窗口上限 */
#define ARRIVAL_MAX_AVG_WINDOW 30
/** BLE 规范里的“RSSI 不可用”取值，同时也把 >0 的值视为非法 */
#define ARRIVAL_RSSI_INVALID 127

/** 到达判定事件 */
typedef enum {
    ARRIVAL_EVENT_NONE = 0,    /**< 无变化 */
    ARRIVAL_EVENT_ARRIVED = 1, /**< 目标到达 */
    ARRIVAL_EVENT_LEFT = -1,   /**< 目标离开 */
} arrival_event_t;

/** 目标在场状态 */
typedef enum {
    ARRIVAL_STATE_UNKNOWN = 0, /**< 还没拿到有效样本 */
    ARRIVAL_STATE_ABSENT = 1,  /**< 确认不在场 */
    ARRIVAL_STATE_PRESENT = 2, /**< 确认在场 */
} arrival_state_t;

/**
 * @brief 到达判定参数
 *
 * 默认值只是占位，**必须在目标设备自身上标定**（绝对阈值不可跨设备移植）。
 */
typedef struct {
    uint8_t median_window;    /**< 中值滤波窗口，1..ARRIVAL_MAX_MEDIAN_WINDOW */
    uint8_t avg_window;       /**< 滑动平均窗口，1..ARRIVAL_MAX_AVG_WINDOW */
    int8_t enter_threshold;   /**< 进入阈值(dBm)，滤波后 >= 该值算“靠近” */
    uint8_t hysteresis_db;    /**< 迟滞(dB)，退出阈值 = enter_threshold - hysteresis_db */
    uint8_t hit_count;        /**< 连续命中多少次才判定“到达” */
    uint8_t miss_count;       /**< 连续低于退出阈值多少次才判定“离开” */
    uint16_t cooldown_ms;     /**< 两次“到达”事件之间的最小间隔 */
    uint16_t stale_timeout_ms;/**< 超过该时长没收到广播 => 判定“离开”（信标掉电/走远）。
                                     实测设备的广播间隔最大空档可达 4.7s，
                                     默认给到 8s，避免“人在门口却被判离开”。 */
} arrival_config_t;

/**
 * @brief 到达判定器状态
 *
 * 实例应当是静态/全局对象或放在长期存活的任务栈上，不要放临时栈帧。
 */
typedef struct {
    arrival_config_t cfg;

    int8_t median_buf[ARRIVAL_MAX_MEDIAN_WINDOW]; /**< 中值滤波环形缓冲 */
    uint8_t median_head;
    uint8_t median_count;

    int8_t avg_buf[ARRIVAL_MAX_AVG_WINDOW]; /**< 滑动平均环形缓冲 */
    uint8_t avg_head;
    uint8_t avg_count;
    int32_t avg_sum;

    arrival_state_t state;
    uint8_t hit_streak;
    uint8_t miss_streak;

    uint32_t last_rx_ms;        /**< 最后一次收到有效 RSSI 的时间戳 */
    uint32_t last_arrived_ms;   /**< 最后一次“到达”事件的时间戳 */
    uint32_t arrived_count;
    uint32_t left_count;
    uint32_t sample_count;

    int8_t last_raw_rssi;       /**< 最近一次原始 RSSI */
    int8_t last_filtered_rssi;  /**< 最近一次滤波后的 RSSI */
} arrival_detector_t;

/** 填入一组保守的默认参数（必须按目标设备实测覆盖） */
void arrival_config_defaults(arrival_config_t *cfg);

/** 初始化检测器（清空历史，状态置 UNKNOWN） */
void arrival_detector_init(arrival_detector_t *det, const arrival_config_t *cfg);

/** 清空滤波历史，保留配置与在场状态 */
void arrival_detector_reset(arrival_detector_t *det);

/**
 * @brief 运行时更新参数（用于 Web 页面上在线调阈值）
 *
 * 保留当前在场状态与统计计数，但会清空滤波历史，避免用旧窗口数据判断新阈值。
 */
void arrival_detector_set_config(arrival_detector_t *det, const arrival_config_t *cfg);

/**
 * @brief 喂入一个 RSSI 样本
 *
 * @param det    检测器
 * @param rssi   RSSI(dBm)。传 ARRIVAL_RSSI_INVALID 或 >0 表示“本轮没收到包”
 * @param now_ms 毫秒时间戳（esp_timer_get_time()/1000 或 xTaskGetTickCount 换算）
 * @return ARRIVAL_EVENT_ARRIVED / ARRIVAL_EVENT_LEFT / ARRIVAL_EVENT_NONE
 *
 * @note 长时间没有样本时，需要周期性调用本函数并传入非法 RSSI，
 *       否则 stale 超时无法触发（见 arrival_detector_feed 的实现）。
 */
arrival_event_t arrival_detector_feed(arrival_detector_t *det, int8_t rssi, uint32_t now_ms);

/** 当前滤波后的 RSSI（没有样本时返回 ARRIVAL_RSSI_INVALID） */
int8_t arrival_detector_filtered_rssi(const arrival_detector_t *det);

/** 当前在场状态 */
arrival_state_t arrival_detector_state(const arrival_detector_t *det);

/** 当前退出阈值(enter_threshold - hysteresis_db) */
int8_t arrival_detector_leave_threshold(const arrival_detector_t *det);

#ifdef __cplusplus
}
#endif

#endif  // ARRIVAL_DETECTOR_H_
