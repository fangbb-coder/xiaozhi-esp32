/**
 * @file arrival_target.cc
 * @brief 信标目标：解析 / 持久化 / 匹配
 */

#include "arrival_target.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "settings.h"

namespace ble_arrival {

static const char* TAG = "BleArrivalTarget";

namespace {

constexpr const char* kNvsNamespace = "ble_arrival";
constexpr const char* kNvsKeyTarget = "target";

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::string Upper(const std::string& s) {
    std::string out = s;
    for (auto& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

/** 解析 "AA:BB:CC:DD:EE:FF" 或 "AABBCCDDEEFF" */
bool ParseMac(const std::string& text, uint8_t out[6]) {
    std::string hex;
    for (char c : text) {
        if (c == ':' || c == '-' || c == '.') {
            continue;
        }
        if (HexVal(c) < 0) {
            return false;
        }
        hex.push_back(c);
    }
    if (hex.size() != 12) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = static_cast<uint8_t>((HexVal(hex[i * 2]) << 4) | HexVal(hex[i * 2 + 1]));
    }
    return true;
}

/** 解析 UUID（32 位十六进制，可带连字符） */
bool ParseUuid(const std::string& text, uint8_t out[16]) {
    std::string hex;
    for (char c : text) {
        if (c == '-') {
            continue;
        }
        if (HexVal(c) < 0) {
            return false;
        }
        hex.push_back(c);
    }
    if (hex.size() != 32) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        out[i] = static_cast<uint8_t>((HexVal(hex[i * 2]) << 4) | HexVal(hex[i * 2 + 1]));
    }
    return true;
}

/** 解析 16 位无符号整数，支持十进制与 0x 前缀 */
bool ParseU16(const std::string& text, uint16_t* out) {
    std::string s = Trim(text);
    if (s.empty()) {
        return false;
    }
    int base = 10;
    size_t start = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        start = 2;
    }
    unsigned long value = 0;
    for (size_t i = start; i < s.size(); i++) {
        int digit;
        if (base == 16) {
            digit = HexVal(s[i]);
        } else {
            digit = (s[i] >= '0' && s[i] <= '9') ? s[i] - '0' : -1;
        }
        if (digit < 0) {
            return false;
        }
        value = value * static_cast<unsigned long>(base) + static_cast<unsigned long>(digit);
        if (value > 0xFFFF) {
            return false;
        }
    }
    *out = static_cast<uint16_t>(value);
    return true;
}

}  // namespace

std::string FormatMac(const uint8_t mac[6]) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    return std::string(buf);
}

std::string FormatUuid(const uint8_t uuid[16]) {
    char buf[37];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return std::string(buf);
}

bool BeaconTarget::IsValid() const {
    if (mode == TargetMode::kMac) {
        for (int i = 0; i < 6; i++) {
            if (mac[i] != 0) {
                return true;
            }
        }
        return false;
    }
    if (mode == TargetMode::kIBeacon) {
        for (int i = 0; i < 16; i++) {
            if (uuid[i] != 0) {
                return true;
            }
        }
        return false;
    }
    return false;
}

std::string BeaconTarget::Describe() const {
    switch (mode) {
        case TargetMode::kMac:
            return "MAC " + FormatMac(mac);
        case TargetMode::kIBeacon: {
            std::string s = "iBeacon " + FormatUuid(uuid);
            if (major_valid) {
                s += " major=" + std::to_string(major);
            }
            if (minor_valid) {
                s += " minor=" + std::to_string(minor);
            }
            return s;
        }
        default:
            return "未绑定";
    }
}

std::string BeaconTarget::Serialize() const {
    switch (mode) {
        case TargetMode::kMac:
            return "mac:" + FormatMac(mac);
        case TargetMode::kIBeacon: {
            char buf[64];
            snprintf(buf, sizeof(buf), "ibeacon:%s:%u:%u", FormatUuid(uuid).c_str(),
                     major_valid ? (unsigned)major : 0u, minor_valid ? (unsigned)minor : 0u);
            return std::string(buf);
        }
        default:
            return "";
    }
}

bool BeaconTarget::Parse(const std::string& text, BeaconTarget* out, std::string* error) {
    if (out == nullptr) {
        return false;
    }
    std::string input = Trim(text);
    if (input.empty()) {
        if (error) *error = "输入为空";
        return false;
    }

    BeaconTarget result;

    // 拆出可能的 major/minor 后缀
    std::string body = input;
    std::string major_text;
    std::string minor_text;
    size_t colon = body.rfind(':');
    if (colon != std::string::npos) {
        std::string tail = body.substr(colon + 1);
        std::string head = body.substr(0, colon);
        // 只有当 tail 看起来像数字时才当作 major/minor
        bool numeric = !tail.empty();
        for (char c : tail) {
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != 'x' && c != 'X') {
                numeric = false;
                break;
            }
        }
        if (numeric && head.find(':') != std::string::npos) {
            // "UUID:major:minor" 或 "AA:BB:CC:DD:EE:FF:xxx"（后者会被 MAC 解析拒绝）
            size_t colon2 = head.rfind(':');
            minor_text = tail;
            major_text = head.substr(colon2 + 1);
            body = head.substr(0, colon2);
        }
    }

    uint8_t mac[6] = {0};
    if (ParseMac(body, mac)) {
        result.mode = TargetMode::kMac;
        memcpy(result.mac, mac, 6);
        *out = result;
        return true;
    }

    uint8_t uuid[16] = {0};
    if (ParseUuid(body, uuid)) {
        result.mode = TargetMode::kIBeacon;
        memcpy(result.uuid, uuid, 16);
        if (!major_text.empty() && ParseU16(major_text, &result.major)) {
            result.major_valid = true;
        }
        if (!minor_text.empty() && ParseU16(minor_text, &result.minor)) {
            result.minor_valid = true;
        }
        *out = result;
        return true;
    }

    if (error) {
        *error = "无法识别的格式。支持 AA:BB:CC:DD:EE:FF 或 UUID[:major:minor]";
    }
    return false;
}

bool BeaconTarget::Load(BeaconTarget* out) {
    if (out == nullptr) {
        return false;
    }
    Settings settings(kNvsNamespace, false);
    std::string raw = settings.GetString(kNvsKeyTarget, "");
    if (raw.empty()) {
        return false;
    }
    BeaconTarget parsed;
    std::string err;
    if (!Parse(raw, &parsed, &err)) {
        ESP_LOGW(TAG, "NVS 中的绑定内容无法解析: %s", err.c_str());
        return false;
    }
    *out = parsed;
    return true;
}

bool BeaconTarget::Save(const BeaconTarget& target) {
    if (!target.IsValid()) {
        return Clear();
    }
    Settings settings(kNvsNamespace, true);
    settings.SetString(kNvsKeyTarget, target.Serialize());
    ESP_LOGI(TAG, "已保存绑定: %s", target.Describe().c_str());
    return true;
}

bool BeaconTarget::Clear() {
    Settings settings(kNvsNamespace, true);
    settings.EraseKey(kNvsKeyTarget);
    ESP_LOGI(TAG, "已清除绑定");
    return true;
}

bool BeaconTarget::MatchesAddress(const uint8_t addr[6]) const {
    if (mode != TargetMode::kMac || addr == nullptr) {
        return false;
    }
    return memcmp(mac, addr, 6) == 0;
}

bool ParseIBeaconFromAdv(const uint8_t* adv, size_t adv_len, uint8_t uuid[16], uint16_t* major,
                         uint16_t* minor) {
    if (adv == nullptr || adv_len < 11) {
        return false;
    }
    size_t offset = 0;
    while (offset < adv_len) {
        uint8_t len = adv[offset];
        if (len == 0) {
            break;
        }
        // AD 结构: [len][type][payload...]，len 包含 type 本身
        if (offset + 1 + len > adv_len) {
            break;
        }
        uint8_t type = adv[offset + 1];
        if (type == 0xFF && len >= 25) {
            const uint8_t* payload = adv + offset + 2;
            uint16_t company = static_cast<uint16_t>(payload[0] | (payload[1] << 8));
            if (company == 0x004C && payload[2] == 0x02 && payload[3] == 0x15) {
                if (uuid != nullptr) {
                    memcpy(uuid, payload + 4, 16);
                }
                if (major != nullptr) {
                    *major = static_cast<uint16_t>(payload[20] | (payload[21] << 8));
                }
                if (minor != nullptr) {
                    *minor = static_cast<uint16_t>(payload[22] | (payload[23] << 8));
                }
                return true;
            }
        }
        offset += 1 + len;
    }
    return false;
}

bool BeaconTarget::MatchesIBeacon(const uint8_t* adv, size_t adv_len) const {
    if (mode != TargetMode::kIBeacon) {
        return false;
    }
    uint8_t uuid[16] = {0};
    uint16_t major = 0;
    uint16_t minor = 0;
    if (!ParseIBeaconFromAdv(adv, adv_len, uuid, &major, &minor)) {
        return false;
    }
    if (memcmp(uuid, this->uuid, 16) != 0) {
        return false;
    }
    if (major_valid && major != this->major) {
        return false;
    }
    if (minor_valid && minor != this->minor) {
        return false;
    }
    return true;
}

bool BeaconTarget::Matches(const uint8_t addr[6], const uint8_t* adv, size_t adv_len,
                            bool allow_address_fallback) const {
    if (!IsValid()) {
        return false;
    }
    if (mode == TargetMode::kMac) {
        return MatchesAddress(addr);
    }
    if (MatchesIBeacon(adv, adv_len)) {
        return true;
    }
    if (allow_address_fallback && MatchesAddress(addr)) {
        return true;
    }
    return false;
}

}  // namespace ble_arrival
