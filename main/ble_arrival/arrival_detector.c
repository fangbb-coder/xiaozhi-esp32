/**
 * @file arrival_detector.c
 * @brief 到达判定核心实现（纯 C99，可在主机侧单独编译）
 *
 * 语义约定（与 scripts/arrival_algo.py 的 Python 参照实现严格一致）：
 *  - 所有整数除法都是 C 的“向零截断”语义；Python 侧用 math.trunc / int() 对齐。
 *  - 中值窗口内样本数为偶数时，取中间两个值的算术平均（同样是向零截断）。
 *  - 时间差用无符号相减，天然兼容 uint32 回绕。
 */

#include "arrival_detector.h"

#include <string.h>

static uint8_t clamp_u8(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) {
        return (uint8_t)lo;
    }
    if (v > hi) {
        return (uint8_t)hi;
    }
    return (uint8_t)v;
}

/** 把窗口内的样本复制到临时数组并做插入排序，返回中值 */
static int8_t median_of(const int8_t *buf, uint8_t count) {
    int8_t tmp[ARRIVAL_MAX_MEDIAN_WINDOW];
    uint8_t i;
    uint8_t j;

    if (count == 0) {
        return ARRIVAL_RSSI_INVALID;
    }

    for (i = 0; i < count; i++) {
        int8_t key = buf[i];
        j = i;
        while (j > 0 && tmp[j - 1] > key) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = key;
    }

    if (count & 1u) {
        return tmp[count / 2];
    }
    /* 偶数个样本：取中间两个的平均。注意 C 的整数除法向零截断。 */
    return (int8_t)(((int)tmp[count / 2 - 1] + (int)tmp[count / 2]) / 2);
}

void arrival_config_defaults(arrival_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->median_window = 5;
    cfg->avg_window = 10;
    cfg->enter_threshold = -70;
    cfg->hysteresis_db = 12;
    cfg->hit_count = 3;
    cfg->miss_count = 4;
    cfg->cooldown_ms = 60000;
    cfg->stale_timeout_ms = 8000;
}

static void sanitize_config(arrival_config_t *cfg) {
    cfg->median_window = clamp_u8(cfg->median_window, 1, ARRIVAL_MAX_MEDIAN_WINDOW);
    cfg->avg_window = clamp_u8(cfg->avg_window, 1, ARRIVAL_MAX_AVG_WINDOW);
    if (cfg->enter_threshold > 0) {
        cfg->enter_threshold = -1;
    }
    if (cfg->hysteresis_db > 127) {
        cfg->hysteresis_db = 127;
    }
    if (cfg->hit_count == 0) {
        cfg->hit_count = 1;
    }
    if (cfg->miss_count == 0) {
        cfg->miss_count = 1;
    }
}

void arrival_detector_init(arrival_detector_t *det, const arrival_config_t *cfg) {
    if (det == NULL) {
        return;
    }
    memset(det, 0, sizeof(*det));
    if (cfg != NULL) {
        det->cfg = *cfg;
    } else {
        arrival_config_defaults(&det->cfg);
    }
    sanitize_config(&det->cfg);
    det->state = ARRIVAL_STATE_UNKNOWN;
    det->last_filtered_rssi = ARRIVAL_RSSI_INVALID;
}

void arrival_detector_reset(arrival_detector_t *det) {
    if (det == NULL) {
        return;
    }
    det->median_head = 0;
    det->median_count = 0;
    det->avg_head = 0;
    det->avg_count = 0;
    det->avg_sum = 0;
    det->hit_streak = 0;
    det->miss_streak = 0;
    det->last_filtered_rssi = ARRIVAL_RSSI_INVALID;
}

void arrival_detector_set_config(arrival_detector_t *det, const arrival_config_t *cfg) {
    if (det == NULL || cfg == NULL) {
        return;
    }
    det->cfg = *cfg;
    sanitize_config(&det->cfg);
    /* 阈值变了，旧窗口里的样本没有可比性 */
    arrival_detector_reset(det);
}

int8_t arrival_detector_filtered_rssi(const arrival_detector_t *det) {
    if (det == NULL || det->avg_count == 0) {
        return ARRIVAL_RSSI_INVALID;
    }
    return (int8_t)(det->avg_sum / (int32_t)det->avg_count);
}

arrival_state_t arrival_detector_state(const arrival_detector_t *det) {
    return det != NULL ? det->state : ARRIVAL_STATE_UNKNOWN;
}

int8_t arrival_detector_leave_threshold(const arrival_detector_t *det) {
    if (det == NULL) {
        return ARRIVAL_RSSI_INVALID;
    }
    return (int8_t)((int)det->cfg.enter_threshold - (int)det->cfg.hysteresis_db);
}

/** 超时检查：长时间没有有效样本 => 视为目标已离开 */
static arrival_event_t check_stale(arrival_detector_t *det, uint32_t now_ms) {
    if (det->state == ARRIVAL_STATE_PRESENT && det->cfg.stale_timeout_ms > 0 &&
        (uint32_t)(now_ms - det->last_rx_ms) > (uint32_t)det->cfg.stale_timeout_ms) {
        det->state = ARRIVAL_STATE_ABSENT;
        det->hit_streak = 0;
        det->miss_streak = 0;
        det->left_count++;
        return ARRIVAL_EVENT_LEFT;
    }
    return ARRIVAL_EVENT_NONE;
}

arrival_event_t arrival_detector_feed(arrival_detector_t *det, int8_t rssi, uint32_t now_ms) {
    if (det == NULL) {
        return ARRIVAL_EVENT_NONE;
    }

    /* 非法样本：只驱动超时检查 */
    if (rssi == ARRIVAL_RSSI_INVALID || rssi > 0) {
        return check_stale(det, now_ms);
    }

    det->last_raw_rssi = rssi;
    det->last_rx_ms = now_ms;
    det->sample_count++;

    /* 1) 中值滤波：环形缓冲写入后取窗口中值 */
    det->median_buf[det->median_head] = rssi;
    det->median_head = (uint8_t)((det->median_head + 1) % det->cfg.median_window);
    if (det->median_count < det->cfg.median_window) {
        det->median_count++;
    }
    int8_t med = median_of(det->median_buf, det->median_count);

    /* 2) 滑动平均：把中值喂进均值窗口 */
    if (det->avg_count < det->cfg.avg_window) {
        det->avg_buf[det->avg_head] = med;
        det->avg_count++;
        det->avg_sum += med;
    } else {
        det->avg_sum -= det->avg_buf[det->avg_head];
        det->avg_buf[det->avg_head] = med;
        det->avg_sum += med;
    }
    det->avg_head = (uint8_t)((det->avg_head + 1) % det->cfg.avg_window);

    int8_t filtered = arrival_detector_filtered_rssi(det);
    det->last_filtered_rssi = filtered;

    int8_t enter = det->cfg.enter_threshold;
    int8_t leave = arrival_detector_leave_threshold(det);

    if (filtered >= enter) {
        det->miss_streak = 0;
        if (det->hit_streak < 255) {
            det->hit_streak++;
        }
        if (det->state != ARRIVAL_STATE_PRESENT) {
            if (det->hit_streak >= det->cfg.hit_count) {
                det->state = ARRIVAL_STATE_PRESENT;
                det->hit_streak = 0;
                /* 冷却期内不再重复播报，但仍要进入 PRESENT 状态 */
                bool cooled = det->arrived_count == 0 ||
                              (uint32_t)(now_ms - det->last_arrived_ms) >= (uint32_t)det->cfg.cooldown_ms;
                if (cooled) {
                    det->last_arrived_ms = now_ms;
                    det->arrived_count++;
                    return ARRIVAL_EVENT_ARRIVED;
                }
            }
        } else {
            det->hit_streak = 0;
        }
    } else if (filtered < leave) {
        det->hit_streak = 0;
        if (det->miss_streak < 255) {
            det->miss_streak++;
        }
        if (det->state == ARRIVAL_STATE_PRESENT && det->miss_streak >= det->cfg.miss_count) {
            det->state = ARRIVAL_STATE_ABSENT;
            det->miss_streak = 0;
            det->left_count++;
            return ARRIVAL_EVENT_LEFT;
        }
        if (det->state == ARRIVAL_STATE_UNKNOWN) {
            det->state = ARRIVAL_STATE_ABSENT;
        }
    } else {
        /* 迟滞死区：证据不足，保持当前状态 */
        det->hit_streak = 0;
        det->miss_streak = 0;
    }

    return ARRIVAL_EVENT_NONE;
}
