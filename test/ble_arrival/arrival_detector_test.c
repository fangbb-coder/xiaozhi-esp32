/**
 * @file arrival_detector_test.c
 * @brief 到达判定核心的主机侧单元测试
 *
 * 三层验证：
 *   1. 基础算子：中值滤波 / 滑动平均 / 迟滞死区 / 冷却 / 超时
 *   2. 事件时序：合成一段"靠近 -> 远离"的 RSSI 序列，断言事件序列
 *   3. 金标准交叉校验：用 Python 参照实现在**真实采集数据**上算出的逐步期望值，
 *      逐条比对 C 实现 —— 证明 C 与 Python 语义完全一致。
 *
 * 编译（项目 tools/w64devkit 里已带便携 GCC）：
 *   gcc -std=c99 -Wall -Wextra -Werror -I../../main/ble_arrival \
 *       arrival_detector_test.c ../../main/ble_arrival/arrival_detector.c -o arrival_test.exe
 */

#include <stdio.h>
#include <string.h>

#include "arrival_detector.h"
#include "arrival_golden_vectors.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        g_checks++;                                          \
        if (!(cond)) {                                       \
            g_failures++;                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                    \
            fprintf(stderr, "\n");                           \
        }                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* 1. 基础算子                                                        */
/* ------------------------------------------------------------------ */
static void test_median_and_average(void) {
    arrival_config_t cfg;
    arrival_detector_t det;

    arrival_config_defaults(&cfg);
    cfg.median_window = 3;
    cfg.avg_window = 3;
    cfg.enter_threshold = -70;
    cfg.hysteresis_db = 10;
    cfg.hit_count = 1;
    cfg.miss_count = 1;
    cfg.cooldown_ms = 0;
    cfg.stale_timeout_ms = 0;

    arrival_detector_init(&det, &cfg);

    /* hit_count=1 时，第一个超过阈值的样本就会触发到达 */
    CHECK(arrival_detector_feed(&det, -50, 100) == ARRIVAL_EVENT_ARRIVED,
          "hit_count=1 时首个样本应触发到达");
    CHECK(arrival_detector_filtered_rssi(&det) == -50, "滤波值应为 -50，实际 %d",
          arrival_detector_filtered_rssi(&det));

    /* 窗口还没填满时，均值窗口里只有已知的样本 */
    CHECK(arrival_detector_feed(&det, -50, 200) == ARRIVAL_EVENT_NONE, "已在场时不应重复触发");
    CHECK(arrival_detector_filtered_rssi(&det) == -50, "滤波值应为 -50，实际 %d",
          arrival_detector_filtered_rssi(&det));

    /* 毛刺：-90 混进 -50/-50 的中值窗口，中值应仍是 -50 */
    CHECK(arrival_detector_feed(&det, -90, 300) == ARRIVAL_EVENT_NONE, "毛刺不应触发事件");
    CHECK(arrival_detector_filtered_rssi(&det) == -50, "中值滤波应滤掉 -90 毛刺，实际 %d",
          arrival_detector_filtered_rssi(&det));
}

/* ------------------------------------------------------------------ */
/* 2. 事件时序：靠近 -> 停留 -> 远离                                    */
/* ------------------------------------------------------------------ */
static void test_event_sequence(void) {
    arrival_config_t cfg;
    arrival_detector_t det;
    uint32_t now = 1000;
    int arrived = 0;
    int left = 0;

    arrival_config_defaults(&cfg);
    cfg.median_window = 3;
    cfg.avg_window = 3;
    cfg.enter_threshold = -70;
    cfg.hysteresis_db = 12; /* 退出阈值 -82 */
    cfg.hit_count = 3;
    cfg.miss_count = 2;
    cfg.cooldown_ms = 60000;
    cfg.stale_timeout_ms = 0;

    arrival_detector_init(&det, &cfg);

    /* 远场 -90：不应到达 */
    for (int i = 0; i < 10; i++) {
        arrival_event_t ev = arrival_detector_feed(&det, -90, now += 100);
        CHECK(ev == ARRIVAL_EVENT_NONE, "远场不应触发事件，实际 %d", ev);
    }
    CHECK(arrival_detector_state(&det) == ARRIVAL_STATE_ABSENT, "远场状态应为 ABSENT");

    /*
     * 靠近 -60：滤波窗口需要时间填满，所以连续喂足够多的样本，
     * 期间只应恰好产生一次到达事件。
     */
    for (int i = 0; i < 12; i++) {
        arrival_event_t ev = arrival_detector_feed(&det, -60, now += 100);
        if (ev == ARRIVAL_EVENT_ARRIVED) {
            arrived++;
        } else {
            CHECK(ev == ARRIVAL_EVENT_NONE, "靠近阶段不应产生离开事件，实际 %d", ev);
        }
    }
    CHECK(arrived == 1, "靠近阶段应恰好触发一次到达事件，实际 %d 次", arrived);
    CHECK(arrival_detector_state(&det) == ARRIVAL_STATE_PRESENT, "靠近后状态应为 PRESENT");

    /* 死区 -75（介于 -82 与 -70 之间）：保持 PRESENT，不产生事件 */
    for (int i = 0; i < 12; i++) {
        arrival_event_t ev = arrival_detector_feed(&det, -75, now += 100);
        CHECK(ev == ARRIVAL_EVENT_NONE, "迟滞死区内不应产生事件，实际 %d", ev);
    }
    CHECK(arrival_detector_state(&det) == ARRIVAL_STATE_PRESENT, "死区内状态应保持 PRESENT");

    /* 远离 -95：连续低于退出阈值后离开 */
    for (int i = 0; i < 12; i++) {
        arrival_event_t ev = arrival_detector_feed(&det, -95, now += 100);
        if (ev == ARRIVAL_EVENT_LEFT) {
            left++;
        }
    }
    CHECK(left == 1, "远离阶段应恰好触发一次离开事件，实际 %d 次", left);
    CHECK(arrival_detector_state(&det) == ARRIVAL_STATE_ABSENT, "离开后状态应为 ABSENT");

    /* 再次靠近，但处于冷却期：状态会变 PRESENT，但不产生到达事件 */
    now += 100;
    for (int i = 0; i < 12; i++) {
        arrival_event_t ev = arrival_detector_feed(&det, -60, now += 100);
        CHECK(ev == ARRIVAL_EVENT_NONE, "冷却期内不应重复产生到达事件，实际 %d", ev);
    }
    CHECK(arrival_detector_state(&det) == ARRIVAL_STATE_PRESENT, "冷却期内状态仍应变为 PRESENT");
}

/* ------------------------------------------------------------------ */
/* 3. 超时（信标掉电 / 走远）                                          */
/* ------------------------------------------------------------------ */
static void test_stale_timeout(void) {
    arrival_config_t cfg;
    arrival_detector_t det;
    uint32_t now = 0;

    arrival_config_defaults(&cfg);
    cfg.median_window = 1;
    cfg.avg_window = 1;
    cfg.enter_threshold = -70;
    cfg.hysteresis_db = 12;
    cfg.hit_count = 1;
    cfg.miss_count = 1;
    cfg.cooldown_ms = 0;
    cfg.stale_timeout_ms = 3000;

    arrival_detector_init(&det, &cfg);

    CHECK(arrival_detector_feed(&det, -50, now) == ARRIVAL_EVENT_ARRIVED, "应触发到达");
    CHECK(arrival_detector_state(&det) == ARRIVAL_STATE_PRESENT, "应为 PRESENT");

    /* 停止喂样本，模拟信标消失；喂非法 RSSI 驱动超时 */
    now += 1000;
    CHECK(arrival_detector_feed(&det, ARRIVAL_RSSI_INVALID, now) == ARRIVAL_EVENT_NONE,
          "未超时前不应触发离开");
    now += 1000;
    CHECK(arrival_detector_feed(&det, ARRIVAL_RSSI_INVALID, now) == ARRIVAL_EVENT_NONE,
          "未超时前不应触发离开");
    now += 2000; /* 累计 4000ms > 3000ms */
    CHECK(arrival_detector_feed(&det, ARRIVAL_RSSI_INVALID, now) == ARRIVAL_EVENT_LEFT,
          "超过 stale_timeout 应触发离开");
    CHECK(arrival_detector_state(&det) == ARRIVAL_STATE_ABSENT, "应为 ABSENT");
}

/* ------------------------------------------------------------------ */
/* 4. 金标准交叉校验（真实采集数据）                                   */
/* ------------------------------------------------------------------ */
static void test_golden_vectors(void) {
    arrival_config_t cfg;
    arrival_detector_t det;
    int mismatches = 0;

    cfg.median_window = ARRIVAL_GOLDEN_MEDIAN_WINDOW;
    cfg.avg_window = ARRIVAL_GOLDEN_AVG_WINDOW;
    cfg.enter_threshold = ARRIVAL_GOLDEN_ENTER_THRESHOLD;
    cfg.hysteresis_db = ARRIVAL_GOLDEN_HYSTERESIS_DB;
    cfg.hit_count = ARRIVAL_GOLDEN_HIT_COUNT;
    cfg.miss_count = ARRIVAL_GOLDEN_MISS_COUNT;
    cfg.cooldown_ms = ARRIVAL_GOLDEN_COOLDOWN_MS;
    cfg.stale_timeout_ms = ARRIVAL_GOLDEN_STALE_TIMEOUT_MS;

    arrival_detector_init(&det, &cfg);

    for (int i = 0; i < ARRIVAL_GOLDEN_STEPS; i++) {
        const arrival_golden_step_t* step = &arrival_golden_steps[i];
        arrival_event_t ev = arrival_detector_feed(&det, step->rssi, step->dt_ms);
        int8_t filtered = arrival_detector_filtered_rssi(&det);
        if (ev != step->expected_event || filtered != step->expected_filtered) {
            if (mismatches < 10) {
                fprintf(stderr,
                        "MISMATCH step %d: rssi=%d dt=%ums -> event %d (期望 %d), filtered %d (期望 %d)\n",
                        i, step->rssi, step->dt_ms, (int)ev, (int)step->expected_event, (int)filtered,
                        (int)step->expected_filtered);
            }
            mismatches++;
        }
    }

    CHECK(mismatches == 0, "金标准比对不一致：%d / %d 步", mismatches, ARRIVAL_GOLDEN_STEPS);
    printf("  金标准比对: %d 步全部一致\n", ARRIVAL_GOLDEN_STEPS);
}

int main(void) {
    printf("== arrival_detector 单元测试 ==\n");

    printf("- 基础算子\n");
    test_median_and_average();
    printf("- 事件时序\n");
    test_event_sequence();
    printf("- 超时判定\n");
    test_stale_timeout();
    printf("- 金标准交叉校验\n");
    test_golden_vectors();

    printf("\n断言 %d 条，失败 %d 条\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
