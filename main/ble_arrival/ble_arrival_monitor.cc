/**
 * @file ble_arrival_monitor.cc
 * @brief 门口迎宾：BLE 扫描 + 到达判定 + 播报
 */

#include "ble_arrival_monitor.h"

#include <algorithm>
#include <cstring>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "settings.h"

namespace ble_arrival {

static const char* TAG = "BleArrival";

namespace {

constexpr int kQueueLength = 64;
constexpr int kTaskStackSize = 4096;
/* 优先级刻意压到 3：低于 audio_output(4) / opus(7) / audio_input(8) / 主任务(10)。
 * 蓝牙任务每 100ms 醒一次、每次只处理几十条记录，绝不能跟音频任务抢 CPU。 */
constexpr int kTaskPriority = 3;
constexpr int kMaxCandidates = 16;

/** 队列里的一条广播记录 */
struct ScanEntry {
    uint8_t mac[6];
    uint8_t flags;     /**< bit0: 携带 iBeacon */
    uint8_t uuid[16];
    uint16_t major;
    uint16_t minor;
    uint8_t rssi;
    uint32_t ts_ms;    /**< 抓包时刻，不是处理时刻 */
};

/** 毫秒 -> BLE 扫描参数的 0.625ms 单位 */
uint16_t MsToBleUnits(uint32_t ms) {
    uint32_t units = (ms * 8u) / 5u;
    if (units < 0x0004) {
        units = 0x0004;
    }
    if (units > 0x4000) {
        units = 0x4000;
    }
    return static_cast<uint16_t>(units);
}

}  // namespace

/**
 * GAP 回调 —— 运行在蓝牙协议栈任务里。
 *
 * 这里只允许做「解析 + 入队」这种廉价操作：
 * 不能打日志（会拖慢蓝牙栈）、不能访问显示/音频、不能加阻塞锁。
 */
static void ble_arrival_gap_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT || param == nullptr) {
        return;
    }
    const auto& rst = param->scan_rst;
    if (rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
        return;
    }
    if (rst.rssi == 0) {
        return;  // RSSI 不可用（BLE 规范里 0 表示未知）
    }
    uint8_t adv_len = rst.adv_data_len;
    if (adv_len > ESP_BLE_ADV_DATA_LEN_MAX) {
        adv_len = ESP_BLE_ADV_DATA_LEN_MAX;
    }
    ArrivalMonitor::GetInstance().OnScanResult(rst.bda, rst.rssi, rst.ble_adv, adv_len);
}

ArrivalMonitor& ArrivalMonitor::GetInstance() {
    static ArrivalMonitor instance;
    return instance;
}

ArrivalMonitor::~ArrivalMonitor() {
    Stop();
}

esp_err_t ArrivalMonitor::Start(AnnounceCallback announce, BusyCallback busy) {
    if (started_) {
        return ESP_OK;
    }
    announce_ = std::move(announce);
    busy_ = std::move(busy);

    /* 1) 读取绑定与参数：运行时 NVS 优先，其次编译期默认值 */
    arrival_config_t cfg;
    arrival_config_defaults(&cfg);
    cfg.median_window = CONFIG_BLE_ARRIVAL_MEDIAN_WINDOW;
    cfg.avg_window = CONFIG_BLE_ARRIVAL_AVG_WINDOW;
    cfg.enter_threshold = CONFIG_BLE_ARRIVAL_ENTER_THRESHOLD;
    cfg.hysteresis_db = CONFIG_BLE_ARRIVAL_HYSTERESIS_DB;
    cfg.hit_count = CONFIG_BLE_ARRIVAL_HIT_COUNT;
    cfg.miss_count = CONFIG_BLE_ARRIVAL_MISS_COUNT;
    cfg.cooldown_ms = CONFIG_BLE_ARRIVAL_COOLDOWN_MS;
    cfg.stale_timeout_ms = CONFIG_BLE_ARRIVAL_STALE_TIMEOUT_MS;

    Settings settings("ble_arrival", false);
    cfg.enter_threshold =
        static_cast<int8_t>(settings.GetInt("enter_threshold", cfg.enter_threshold));
    cfg.hysteresis_db = static_cast<uint8_t>(settings.GetInt("hysteresis_db", cfg.hysteresis_db));

    BeaconTarget target;
    if (!BeaconTarget::Load(&target)) {
        /* 编译期兜底：一次绑定，永久生效，零交互 */
        std::string def = CONFIG_BLE_ARRIVAL_TARGET_MAC;
        if (!def.empty()) {
            std::string err;
            BeaconTarget parsed;
            if (BeaconTarget::Parse(def, &parsed, &err)) {
                target = parsed;
                ESP_LOGI(TAG, "使用编译期默认目标: %s", target.Describe().c_str());
            } else {
                ESP_LOGW(TAG, "编译期默认目标无效: %s", err.c_str());
            }
        }
    }

    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
    }
    if (mutex_ == nullptr) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        return ESP_ERR_NO_MEM;
    }
    if (queue_ == nullptr) {
        queue_ = xQueueCreate(kQueueLength, sizeof(ScanEntry));
    }
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "创建队列失败");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    target_ = target;
    cfg_ = cfg;
    arrival_detector_init(&detector_, &cfg_);
    xSemaphoreGive(mutex_);

    /* 2) 初始化蓝牙控制器与主机栈 */
    /* 蓝牙栈是这套固件里唯一的大额内部 RAM 新增项，开机前后各打一次余量，
     * 刷完固件对着官方固件的 free sram 一眼就能看出吃掉了多少。 */
    ESP_LOGI(TAG, "蓝牙栈初始化前: 内部SRAM free=%u minimal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    esp_bt_controller_config_t controller_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&controller_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bt_controller_enable 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bluedroid_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_bluedroid_enable 失败: %s", esp_err_to_name(err));
        return err;
    }
    bt_ready_ = true;

    ESP_LOGI(TAG, "蓝牙栈初始化后: 内部SRAM free=%u minimal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

    /* 3) 注册回调之后再设置扫描参数 —— 顺序反了参数会失效 */
    esp_ble_gap_register_callback(ble_arrival_gap_handler);

    esp_ble_scan_params_t scan_params = {};
    scan_params.scan_type = CONFIG_BLE_ARRIVAL_SCAN_ACTIVE ? BLE_SCAN_TYPE_ACTIVE
                                                            : BLE_SCAN_TYPE_PASSIVE;
    scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    scan_params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    scan_params.scan_interval = MsToBleUnits(CONFIG_BLE_ARRIVAL_SCAN_INTERVAL_MS);
    scan_params.scan_window = MsToBleUnits(CONFIG_BLE_ARRIVAL_SCAN_WINDOW_MS);
    /* 必须关掉重复过滤：我们要的是同一个信标的连续 RSSI 样本，而不是“每个设备一条” */
    scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;

    if (scan_params.scan_window > scan_params.scan_interval) {
        scan_params.scan_window = scan_params.scan_interval;
    }

    err = esp_ble_gap_set_scan_params(&scan_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_set_scan_params 失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "扫描参数: interval=%u units(%.1fms) window=%u units(%.1fms) type=%s",
             scan_params.scan_interval, scan_params.scan_interval * 0.625,
             scan_params.scan_window, scan_params.scan_window * 0.625,
             scan_params.scan_type == BLE_SCAN_TYPE_ACTIVE ? "ACTIVE" : "PASSIVE");

    /* 4) 起工作任务 */
    if (task_ == nullptr) {
        xTaskCreate([](void* arg) {
                static_cast<ArrivalMonitor*>(arg)->WorkerTask();
            },
            "ble_arrival", kTaskStackSize, this, kTaskPriority, &task_);
    }
    if (task_ == nullptr) {
        ESP_LOGE(TAG, "创建工作任务失败");
        return ESP_ERR_NO_MEM;
    }

    started_ = true;
    ESP_LOGI(TAG, "门口迎宾已启动，%s",
             target_.IsValid() ? target_.Describe().c_str() : "未绑定目标（自动发现模式）");
    return ESP_OK;
}

void ArrivalMonitor::Stop() {
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (bt_ready_) {
        esp_ble_gap_stop_scanning();
        scanning_ = false;
        bt_ready_ = false;
    }
    if (queue_ != nullptr) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
    started_ = false;
}

void ArrivalMonitor::OnScanResult(const uint8_t* bda, int rssi, const uint8_t* ble_adv,
                                  uint8_t adv_data_len) {
    if (bda == nullptr || queue_ == nullptr) {
        return;
    }

    ScanEntry entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.mac, bda, 6);
    entry.rssi = static_cast<uint8_t>(static_cast<int8_t>(rssi));
    entry.ts_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    uint8_t uuid[16] = {0};
    uint16_t major = 0;
    uint16_t minor = 0;
    bool has_ibeacon = ParseIBeaconFromAdv(ble_adv, adv_data_len, uuid, &major, &minor);
    if (has_ibeacon) {
        entry.flags |= 0x01;
        memcpy(entry.uuid, uuid, 16);
        entry.major = major;
        entry.minor = minor;
    }

    if (xQueueSend(queue_, &entry, 0) != pdTRUE) {
        dropped_++;  // 队列满：丢包计数（原子性不敏感，只用于观测）
    }
}

bool ArrivalMonitor::TargetMatchesLocked(const uint8_t* bda, const uint8_t* uuid, bool has_ibeacon,
                                         uint16_t major, uint16_t minor) {
    if (!target_.IsValid()) {
        return false;
    }
    if (target_.mode == TargetMode::kMac) {
        return memcmp(target_.mac, bda, 6) == 0;
    }
    // iBeacon 模式：先按广播内容匹配；UUID 相同但没带 major/minor 时按地址兜底
    if (has_ibeacon && memcmp(target_.uuid, uuid, 16) == 0) {
        if (!target_.major_valid || target_.major == major) {
            if (!target_.minor_valid || target_.minor == minor) {
                return true;
            }
        }
    }
    return false;
}

void ArrivalMonitor::UpdateCandidateLocked(const uint8_t* bda, int8_t rssi, bool has_ibeacon,
                                            const uint8_t* uuid, uint16_t major, uint16_t minor,
                                            uint32_t now_ms) {
    for (int i = 0; i < candidate_count_; i++) {
        if (memcmp(candidates_[i].mac, bda, 6) == 0) {
            candidates_[i].count++;
            candidates_[i].last_rssi = rssi;
            if (rssi > candidates_[i].best_rssi) {
                candidates_[i].best_rssi = rssi;
            }
            candidates_[i].last_seen_ms = now_ms;
            if (has_ibeacon && !candidates_[i].has_ibeacon) {
                candidates_[i].has_ibeacon = true;
                memcpy(candidates_[i].uuid, uuid, 16);
                candidates_[i].major = major;
                candidates_[i].minor = minor;
            }
            return;
        }
    }
    if (candidate_count_ < kMaxCandidates) {
        ScanCandidate& c = candidates_[candidate_count_++];
        c = ScanCandidate{};
        memcpy(c.mac, bda, 6);
        c.count = 1;
        c.best_rssi = rssi;
        c.last_rssi = rssi;
        c.last_seen_ms = now_ms;
        c.has_ibeacon = has_ibeacon;
        if (has_ibeacon) {
            memcpy(c.uuid, uuid, 16);
            c.major = major;
            c.minor = minor;
        }
    }
}

void ArrivalMonitor::WorkerTask() {
    uint32_t last_candidate_log_ms = 0;
    for (;;) {
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        bool matched_any = false;
        arrival_event_t event = ARRIVAL_EVENT_NONE;

        xSemaphoreTake(mutex_, portMAX_DELAY);
        ScanEntry entry;
        while (xQueueReceive(queue_, &entry, 0) == pdTRUE) {
            if (!target_.IsValid()) {
                /* 未绑定：只做候选发现 */
                UpdateCandidateLocked(entry.mac, static_cast<int8_t>(entry.rssi),
                                      (entry.flags & 0x01) != 0, entry.uuid, entry.major, entry.minor,
                                      entry.ts_ms);
                continue;
            }
            if (!TargetMatchesLocked(entry.mac, entry.uuid, (entry.flags & 0x01) != 0, entry.major,
                                     entry.minor)) {
                continue;
            }
            matched_any = true;
            matched_total_++;
            arrival_event_t ev = arrival_detector_feed(&detector_, static_cast<int8_t>(entry.rssi),
                                                       entry.ts_ms);
            if (ev != ARRIVAL_EVENT_NONE) {
                event = ev;
            }
        }
        /* 这一轮没有匹配到任何包 -> 喂非法样本，驱动 stale 超时检查 */
        if (!matched_any) {
            arrival_event_t ev = arrival_detector_feed(&detector_, ARRIVAL_RSSI_INVALID, now_ms);
            if (ev != ARRIVAL_EVENT_NONE) {
                event = ev;
            }
        }

        /* 未绑定时，每 10 秒在串口提示一次最强候选，方便无网环境下手工确认绑定 */
        if (!target_.IsValid() && candidate_count_ > 0 &&
            (uint32_t)(now_ms - last_candidate_log_ms) >= 10000) {
            int best = 0;
            for (int i = 1; i < candidate_count_; i++) {
                if (candidates_[i].count > candidates_[best].count) {
                    best = i;
                }
            }
            ESP_LOGI(TAG, "未绑定，候选设备 %d 个；最强：%s（广播 %u 次，最强 %d dBm）",
                     candidate_count_, FormatMac(candidates_[best].mac).c_str(),
                     candidates_[best].count, candidates_[best].best_rssi);
            last_candidate_log_ms = now_ms;
        }
        xSemaphoreGive(mutex_);

        if (event != ARRIVAL_EVENT_NONE && announce_) {
            if (event == ARRIVAL_EVENT_ARRIVED) {
                ESP_LOGI(TAG, "事件: 到达");
                announce_(AnnounceKind::kArrived, "arrival", CONFIG_BLE_ARRIVAL_ARRIVAL_TEXT);
            } else {
                ESP_LOGI(TAG, "事件: 离开");
                announce_(AnnounceKind::kLeft, "departure", CONFIG_BLE_ARRIVAL_DEPARTURE_TEXT);
            }
        }

        /* 设备正在说话/收音时暂停扫描，保护 Wi-Fi 音频通道 */
        bool busy = false;
        if (busy_) {
            busy = busy_();
        }
        if (busy && scanning_) {
            esp_ble_gap_stop_scanning();
            scanning_ = false;
            scan_pauses_++;
            ESP_LOGI(TAG, "设备繁忙，暂停 BLE 扫描");
        } else if (!busy && !scanning_ && bt_ready_) {
            esp_err_t err = esp_ble_gap_start_scanning(0);
            if (err == ESP_OK) {
                scanning_ = true;
                ESP_LOGI(TAG, "恢复 BLE 扫描");
            } else if (err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "启动扫描失败: %s", esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_BLE_ARRIVAL_TICK_MS));
    }
}

bool ArrivalMonitor::Bind(const std::string& text, std::string* error) {
    BeaconTarget parsed;
    std::string err;
    if (!BeaconTarget::Parse(text, &parsed, &err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    if (!BeaconTarget::Save(parsed)) {
        if (error) {
            *error = "写入 NVS 失败";
        }
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    target_ = parsed;
    arrival_detector_reset(&detector_);
    candidate_count_ = 0;
    xSemaphoreGive(mutex_);
    if (error) {
        *error = "";
    }
    return true;
}

bool ArrivalMonitor::Unbind() {
    BeaconTarget::Clear();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    target_ = BeaconTarget();
    arrival_detector_reset(&detector_);
    candidate_count_ = 0;
    xSemaphoreGive(mutex_);
    return true;
}

bool ArrivalMonitor::GetTarget(BeaconTarget* out) const {
    if (out == nullptr) {
        return false;
    }
    if (mutex_ == nullptr) {
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    *out = target_;
    bool valid = target_.IsValid();
    xSemaphoreGive(mutex_);
    return valid;
}

bool ArrivalMonitor::GetConfig(arrival_config_t* out) const {
    if (out == nullptr) {
        return false;
    }
    if (mutex_ == nullptr) {
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    *out = cfg_;
    xSemaphoreGive(mutex_);
    return true;
}

bool ArrivalMonitor::SetThresholds(int enter_threshold, int hysteresis_db, std::string* error) {
    if (enter_threshold > 0 || enter_threshold < -127) {
        if (error) {
            *error = "进入阈值必须在 -127 ~ 0 dBm 之间";
        }
        return false;
    }
    if (hysteresis_db < 0 || hysteresis_db > 127) {
        if (error) {
            *error = "迟滞必须在 0 ~ 127 dB 之间";
        }
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    cfg_.enter_threshold = static_cast<int8_t>(enter_threshold);
    cfg_.hysteresis_db = static_cast<uint8_t>(hysteresis_db);
    arrival_detector_set_config(&detector_, &cfg_);
    xSemaphoreGive(mutex_);

    Settings settings("ble_arrival", true);
    settings.SetInt("enter_threshold", enter_threshold);
    settings.SetInt("hysteresis_db", hysteresis_db);
    if (error) {
        *error = "";
    }
    return true;
}

void ArrivalMonitor::GetStatus(MonitorStatus* out) const {
    if (out == nullptr) {
        return;
    }
    MonitorStatus s;
    if (mutex_ != nullptr) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        s.started = started_;
        s.scanning = scanning_;
        s.bound = target_.IsValid();
        s.target_desc = target_.Describe();
        switch (target_.mode) {
            case TargetMode::kMac:
                s.target_kind = "mac";
                break;
            case TargetMode::kIBeacon:
                s.target_kind = "ibeacon";
                break;
            default:
                s.target_kind = "none";
                break;
        }
        s.filtered_rssi = arrival_detector_filtered_rssi(&detector_);
        s.last_raw_rssi = detector_.last_raw_rssi;
        s.state = detector_.state;
        s.arrived_count = detector_.arrived_count;
        s.left_count = detector_.left_count;
        s.samples = detector_.sample_count;
        s.cfg = cfg_;
        xSemaphoreGive(mutex_);
    }
    s.matched_total = matched_total_;
    s.dropped = dropped_;
    s.scan_pauses = scan_pauses_;
    *out = s;
}

void ArrivalMonitor::GetCandidates(std::vector<ScanCandidate>* out) const {
    if (out == nullptr) {
        return;
    }
    out->clear();
    if (mutex_ == nullptr) {
        return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < candidate_count_; i++) {
        out->push_back(candidates_[i]);
    }
    xSemaphoreGive(mutex_);
    std::sort(out->begin(), out->end(), [](const ScanCandidate& a, const ScanCandidate& b) {
        if (a.count != b.count) {
            return a.count > b.count;
        }
        return a.best_rssi > b.best_rssi;
    });
}

}  // namespace ble_arrival
