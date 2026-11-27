/**
 * @file ble_arrival_web.cc
 * @brief 门口迎宾：固件自起的极简 Web 管理页
 */

#include "ble_arrival_web.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ble_arrival_monitor.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"
#include "sdkconfig.h"

namespace ble_arrival {

static const char* TAG = "BleArrivalWeb";

namespace {

httpd_handle_t s_httpd = nullptr;

/* ---------------------------------------------------------------- */
/* 工具函数                                                          */
/* ---------------------------------------------------------------- */

std::string DeviceIp() {
    wifi_sta_ip_info_t info = {};
    if (esp_wifi_sta_get_ip_info(&info) == ESP_OK && info.ip.addr != 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
        return std::string(buf);
    }
    return std::string("192.168.4.1");  // 配网 AP 模式的固定地址
}

int HexToInt(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** 简单的 URL 解码（够用即可：+ 与 %XX） */
std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '+') {
            out.push_back(' ');
        } else if (in[i] == '%' && i + 2 < in.size()) {
            int hi = HexToInt(in[i + 1]);
            int lo = HexToInt(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
            out.push_back(in[i]);
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

/** 解析 "a=1&b=2" 形式的请求体 */
std::string ParseFormValue(const std::string& body, const std::string& key) {
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        std::string pair = (amp == std::string::npos) ? body.substr(pos) : body.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = UrlDecode(pair.substr(0, eq));
            if (k == key) {
                return UrlDecode(pair.substr(eq + 1));
            }
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return std::string();
}

std::string EscapeHtml(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

const char* StateName(arrival_state_t state) {
    switch (state) {
        case ARRIVAL_STATE_PRESENT: return "在场";
        case ARRIVAL_STATE_ABSENT: return "不在场";
        default: return "未知";
    }
}

std::string JsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

/* ---------------------------------------------------------------- */
/* 处理器                                                            */
/* ---------------------------------------------------------------- */

esp_err_t RootHandler(httpd_req_t* req) {
    MonitorStatus status;
    ArrivalMonitor::GetInstance().GetStatus(&status);

    std::string html;
    html.reserve(8192);
    html += "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">";
    html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    html += "<title>门口迎宾</title><style>";
    html += "body{font-family:system-ui,-apple-system,'Segoe UI',sans-serif;margin:0;padding:16px;"
            "background:#f5f5f7;color:#1c1c1e}"
            "h1{font-size:20px;margin:0 0 4px} .sub{color:#6e6e73;font-size:13px;margin-bottom:16px}"
            "div.card{background:#fff;border-radius:12px;padding:14px;margin-bottom:12px;"
            "box-shadow:0 1px 3px rgba(0,0,0,.08)}"
            "table{width:100%;border-collapse:collapse;font-size:13px}"
            "td,th{padding:6px 4px;text-align:left;border-bottom:1px solid #eee}"
            "th{color:#6e6e73;font-weight:500}"
            "input{width:100%;box-sizing:border-box;padding:10px;font-size:15px;border:1px solid #d1d1d6;"
            "border-radius:8px;margin:6px 0}"
            "button{background:#007aff;color:#fff;border:0;border-radius:8px;padding:10px 16px;"
            "font-size:15px;margin-right:8px} button.sec{background:#8e8e93}"
            ".big{font-size:26px;font-weight:600} .muted{color:#8e8e93;font-size:12px}"
            "</style></head><body>";

    html += "<h1>门口迎宾 · BLE 接近检测</h1>";
    html += "<div class=\"sub\">设备 " + EscapeHtml(DeviceIp()) +
            " · 扫描中=" + (status.scanning ? "是" : "否") + "</div>";

    /* 状态卡片 */
    html += "<div class=\"card\"><table>";
    html += "<tr><th>绑定目标</th><td>" + EscapeHtml(status.target_desc) + "</td></tr>";
    html += std::string("<tr><th>当前状态</th><td class=\"big\">") + StateName(status.state) +
            "</td></tr>";
    html += "<tr><th>滤波 RSSI</th><td class=\"big\">";
    html += (status.filtered_rssi == ARRIVAL_RSSI_INVALID) ? "--" : std::to_string(status.filtered_rssi);
    html += " dBm</td></tr>";
    html += "<tr><th>原始 RSSI</th><td>" + std::to_string(status.last_raw_rssi) + " dBm</td></tr>";
    html += "<tr><th>到达 / 离开次数</th><td>" + std::to_string(status.arrived_count) + " / " +
            std::to_string(status.left_count) + "</td></tr>";
    html += "<tr><th>有效样本 / 匹配广播</th><td>" + std::to_string(status.samples) + " / " +
            std::to_string(status.matched_total) + "</td></tr>";
    html += "<tr><th>丢包 / 暂停扫描</th><td>" + std::to_string(status.dropped) + " / " +
            std::to_string(status.scan_pauses) + "</td></tr>";
    html += "<tr><th>进入 / 退出阈值</th><td>" + std::to_string(status.cfg.enter_threshold) + " / " +
            std::to_string(status.cfg.enter_threshold - status.cfg.hysteresis_db) + " dBm</td></tr>";
    html += "</table></div>";

    /* 绑定表单 */
    html += "<div class=\"card\"><b>绑定信标</b>";
    html += "<form method=\"POST\" action=\"/api/bind\">"
            "<input name=\"target\" placeholder=\"AA:BB:CC:DD:EE:FF 或 UUID[:major:minor]\" value=\"\">"
            "<button type=\"submit\">绑定</button>"
            "<button type=\"submit\" class=\"sec\" formaction=\"/api/unbind\">解绑</button></form>";
    html += "<div class=\"muted\">未绑定时会自动列出候选设备（按广播次数排序）。</div></div>";

    /* 阈值表单 */
    html += "<div class=\"card\"><b>阈值标定</b>";
    html += "<form method=\"POST\" action=\"/api/threshold\">";
    html += "<input name=\"enter\" type=\"number\" step=\"1\" placeholder=\"进入阈值 dBm，例如 -70\" value=\"" +
            std::to_string(status.cfg.enter_threshold) + "\">";
    html += "<input name=\"hysteresis\" type=\"number\" step=\"1\" placeholder=\"迟滞 dB，例如 12\" value=\"" +
            std::to_string(status.cfg.hysteresis_db) + "\">";
    html += "<button type=\"submit\">保存</button></form>";
    html += "<div class=\"muted\">进入阈值要<b>在设备自身上</b>标定：站在门口读数，再退到楼下读数，"
            "取两者中间偏门口一侧。</div></div>";

    /* 候选设备 */
    if (!status.bound) {
        std::vector<ScanCandidate> candidates;
        ArrivalMonitor::GetInstance().GetCandidates(&candidates);
        html += "<div class=\"card\"><b>候选设备</b><table>";
        html += "<tr><th>MAC</th><th>广播数</th><th>最强</th><th>最近</th><th>iBeacon</th></tr>";
        for (const auto& c : candidates) {
            html += "<tr><td>" + EscapeHtml(FormatMac(c.mac)) + "</td><td>" +
                    std::to_string(c.count) + "</td><td>" + std::to_string(c.best_rssi) +
                    "</td><td>" + std::to_string(c.last_rssi) + "</td><td>";
            if (c.has_ibeacon) {
                html += EscapeHtml(FormatUuid(c.uuid));
            } else {
                html += "-";
            }
            html += "</td></tr>";
        }
        html += "</table></div>";
    }

    html += "<div class=\"sub\">每 3 秒自动刷新 · 仅限局域网访问</div>";
    html += "<script>setTimeout(function(){location.reload();},3000);</script>";
    html += "</body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, html.c_str());
    return ESP_OK;
}

esp_err_t StatusApiHandler(httpd_req_t* req) {
    MonitorStatus status;
    ArrivalMonitor::GetInstance().GetStatus(&status);

    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"bound\":%s,\"scanning\":%s,\"target\":\"%s\",\"kind\":\"%s\",\"state\":%d,"
             "\"filtered\":%d,\"raw\":%d,\"arrived\":%u,\"left\":%u,\"samples\":%u,\"matched\":%u,"
             "\"dropped\":%u,\"enter\":%d,\"hysteresis\":%d,\"ip\":\"%s\"}",
             status.bound ? "true" : "false", status.scanning ? "true" : "false",
             JsonEscape(status.target_desc).c_str(), status.target_kind.c_str(), (int)status.state,
             (int)status.filtered_rssi, (int)status.last_raw_rssi, (unsigned)status.arrived_count,
             (unsigned)status.left_count, (unsigned)status.samples, (unsigned)status.matched_total,
             (unsigned)status.dropped,
             status.cfg.enter_threshold, status.cfg.hysteresis_db, DeviceIp().c_str());

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

std::string ReadBody(httpd_req_t* req) {
    // ESP-IDF 6.1 去掉了 httpd_req_get_content_len()，直接读结构体字段。
    size_t len = req->content_len;
    if (len == 0 || len > 512) {
        return std::string();
    }
    std::string body(len, '\0');
    int received = 0;
    while (received < (int)len) {
        int ret = httpd_req_recv(req, &body[received], len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            break;
        }
        received += ret;
    }
    body.resize(received);
    return body;
}

esp_err_t BindHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    std::string target = ParseFormValue(body, "target");
    std::string error;
    bool ok = ArrivalMonitor::GetInstance().Bind(target, &error);
    if (ok) {
        ESP_LOGI(TAG, "Web 绑定成功: %s", target.c_str());
    } else {
        ESP_LOGW(TAG, "Web 绑定失败: %s", error.c_str());
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, ok ? "OK" : ("绑定失败：" + error).c_str());
    return ESP_OK;
}

esp_err_t UnbindHandler(httpd_req_t* req) {
    ArrivalMonitor::GetInstance().Unbind();
    ESP_LOGI(TAG, "Web 解绑");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

esp_err_t ThresholdHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    std::string enter_text = ParseFormValue(body, "enter");
    std::string hyst_text = ParseFormValue(body, "hysteresis");

    int enter = atoi(enter_text.c_str());
    int hysteresis = atoi(hyst_text.c_str());
    std::string error;
    bool ok = ArrivalMonitor::GetInstance().SetThresholds(enter, hysteresis, &error);
    if (ok) {
        ESP_LOGI(TAG, "Web 调整阈值: enter=%d hysteresis=%d", enter, hysteresis);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, ok ? "OK" : ("设置失败：" + error).c_str());
    return ESP_OK;
}

}  // namespace

esp_err_t StartWebServer() {
    if (s_httpd != nullptr) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_BLE_ARRIVAL_WEB_PORT;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 4;
    config.stack_size = 4096;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = RootHandler,
        .user_ctx = nullptr,
    };
    static const httpd_uri_t status_api = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = StatusApiHandler,
        .user_ctx = nullptr,
    };
    static const httpd_uri_t bind_api = {
        .uri = "/api/bind",
        .method = HTTP_POST,
        .handler = BindHandler,
        .user_ctx = nullptr,
    };
    static const httpd_uri_t unbind_api = {
        .uri = "/api/unbind",
        .method = HTTP_POST,
        .handler = UnbindHandler,
        .user_ctx = nullptr,
    };
    static const httpd_uri_t threshold_api = {
        .uri = "/api/threshold",
        .method = HTTP_POST,
        .handler = ThresholdHandler,
        .user_ctx = nullptr,
    };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &status_api);
    httpd_register_uri_handler(s_httpd, &bind_api);
    httpd_register_uri_handler(s_httpd, &unbind_api);
    httpd_register_uri_handler(s_httpd, &threshold_api);

    ESP_LOGI(TAG, "Web 管理页已启动: http://%s:%d/", DeviceIp().c_str(), CONFIG_BLE_ARRIVAL_WEB_PORT);
    return ESP_OK;
}

void StopWebServer() {
    if (s_httpd != nullptr) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }
}

}  // namespace ble_arrival
