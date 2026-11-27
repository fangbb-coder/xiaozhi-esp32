/**
 * @file arrival_target.h
 * @brief 门口迎宾：信标目标（绑定对象）的描述、解析、持久化与匹配
 *
 * 支持两种绑定方式：
 *  1. MAC 模式     —— 直接锁定广播设备的 6 字节地址。
 *     优点：白名单可用、省电、抗误判；缺点：设备的 BLE 地址必须固定
 *           （手机/防丢器普遍做了 MAC 随机化，不可作为信标）。
 *  2. iBeacon 模式 —— 锁定 iBeacon 广播里的 UUID，可选再限定 major/minor。
 *     优点：广播内容可配置；缺点：UUID 必须自己烧进信标，且同 UUID 的信标会混淆
 *           （此时必须再限定 major/minor）。
 *
 * 三层绑定优先级（高 -> 低）：
 *   1. 运行时 NVS 里的绑定（Web 页面绑定后写入，重启仍生效）
 *   2. 编译期 Kconfig 默认值 CONFIG_BLE_ARRIVAL_TARGET_MAC
 *   3. 未绑定 => 进入"自动发现"模式，持续广播且信号最强的设备会被列为候选
 */
#ifndef ARRIVAL_TARGET_H_
#define ARRIVAL_TARGET_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace ble_arrival {

/** 绑定模式 */
enum class TargetMode {
    kNone = 0,    /**< 未绑定 */
    kMac = 1,     /**< 按 MAC 匹配 */
    kIBeacon = 2, /**< 按 iBeacon UUID（可选 major/minor）匹配 */
};

/** 信标目标描述 */
struct BeaconTarget {
    TargetMode mode = TargetMode::kNone;

    uint8_t mac[6] = {0};

    uint8_t uuid[16] = {0}; /**< iBeacon UUID，字节序与广播数据一致 */
    uint16_t major = 0;
    uint16_t minor = 0;
    bool major_valid = false;
    bool minor_valid = false;

    /** 是否已经绑定了一个有效目标 */
    bool IsValid() const;

    /** 人类可读描述（用于 Web 页面和日志） */
    std::string Describe() const;

    /** 序列化成可持久化的字符串（"mac:AA:BB:.." 或 "ibeacon:<uuid>:<major>:<minor>"） */
    std::string Serialize() const;

    /**
     * @brief 解析用户输入
     *
     * 支持的写法（大小写不敏感，允许空格）：
     *   - "AA:BB:CC:DD:EE:FF"                       -> MAC 模式
     *   - "E2C56DB5-DFFB-48D2-B060-D0F5A71096E0"    -> iBeacon UUID（不限定 major/minor）
     *   - "E2C56DB5-DFFB-48D2-B060-D0F5A71096E0:100:200" -> UUID + major + minor
     *   - "E2C56DB5DFFB48D2B060D0F5A71096E0:100:200"     -> 同上，无连字符
     *   - major/minor 支持十进制与 0x 十六进制
     */
    static bool Parse(const std::string& text, BeaconTarget* out, std::string* error);

    /** 从 NVS 读取已绑定的目标（没有绑定时返回 false，*out 保持 kNone） */
    static bool Load(BeaconTarget* out);

    /** 写入 NVS */
    static bool Save(const BeaconTarget& target);

    /** 清除 NVS 里的绑定 */
    static bool Clear();

    /** 是否匹配该广播地址（仅 MAC 模式会返回 true） */
    bool MatchesAddress(const uint8_t addr[6]) const;

    /**
     * @brief 是否匹配该条广播里的 iBeacon 内容
     *
     * @param adv      原始广播数据（AD 结构序列）
     * @param adv_len  广播数据长度
     */
    bool MatchesIBeacon(const uint8_t* adv, size_t adv_len) const;

    /**
     * @brief 综合匹配：MAC 模式下按地址匹配；iBeacon 模式下按广播内容匹配
     *
     * @note iBeacon 模式下也接受“地址匹配”作为兜底开关，调用方可通过
     *       allow_address_fallback 决定是否允许。
     */
    bool Matches(const uint8_t addr[6], const uint8_t* adv, size_t adv_len,
                 bool allow_address_fallback) const;
};

/**
 * @brief 从原始广播数据里解析 iBeacon
 *
 * @return 找到合法的 iBeacon 帧返回 true
 */
bool ParseIBeaconFromAdv(const uint8_t* adv, size_t adv_len, uint8_t uuid[16], uint16_t* major,
                         uint16_t* minor);

/** MAC 地址转 "AA:BB:CC:DD:EE:FF" */
std::string FormatMac(const uint8_t mac[6]);

/** UUID 转 "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" */
std::string FormatUuid(const uint8_t uuid[16]);

}  // namespace ble_arrival

#endif  // ARRIVAL_TARGET_H_
