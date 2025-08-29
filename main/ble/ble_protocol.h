#ifndef BLE_PROTOCOL_H
#define BLE_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// BLE协议公共定义
#define BLE_PROTOCOL_HEADER_0           0x58
#define BLE_PROTOCOL_HEADER_1           0x5A

// WiFi配置协议命令 (0x00-0x02)
#define BLE_PROTOCOL_CMD_GET_WIFI_CONFIG     0x00
#define BLE_PROTOCOL_CMD_SET_WIFI_CONFIG     0x01
#define BLE_PROTOCOL_CMD_GET_WIFI_SCAN       0x02

// OTA协议命令 (0x03-0x05)
#define BLE_PROTOCOL_CMD_SEND_FILE_INFO      0x03
#define BLE_PROTOCOL_CMD_SEND_FILE_DATA      0x04
#define BLE_PROTOCOL_CMD_SEND_PACKET_CRC     0x05

// 公共响应状态
#define BLE_PROTOCOL_ACK_SUCCESS             0x00
#define BLE_PROTOCOL_ACK_ERROR               0x01
#define BLE_PROTOCOL_ACK_VERSION_NOT_ALLOW   0x02

// 协议相关常量
#define BLE_PROTOCOL_TIMEOUT_MS              10000   // 10秒超时
#define BLE_PROTOCOL_MAX_CONN_INTERVAL_MS    150     // 最大连接间隔150ms

// BLE 服务定义（小智AI自定义协议）
#define BLE_PROTOCOL_SERVICE_UUID_16         0xFDD0
#define BLE_PROTOCOL_WRITE_CHAR_UUID_16      0xFDD1
#define BLE_PROTOCOL_NOTIFY_CHAR_UUID_16     0xFDD2

// 广播名称前缀
#define BLE_PROTOCOL_ADV_NAME_PREFIX         "lr_wificfg-"

// 数据包长度限制
#define BLE_PROTOCOL_MIN_PACKET_LEN          3       // header(2) + cmd(1)
#define BLE_PROTOCOL_MAX_PAYLOAD_LEN         251     // 根据BLE MTU限制

// 协议数据包结构
typedef struct {
    uint8_t header[2];      // 0x58 0x5A
    uint8_t cmd;            // 命令字节
    uint8_t payload[0];     // 载荷数据（柔性数组成员）
} ble_protocol_packet_t;

// 协议处理函数类型定义
typedef int (*ble_protocol_handler_t)(uint16_t conn_id, const uint8_t *payload, uint16_t payload_len);

// 协议解析函数
bool ble_protocol_parse_packet(const uint8_t *data, size_t len, uint8_t *cmd, const uint8_t **payload, size_t *payload_len);

// 协议构建函数
size_t ble_protocol_build_packet(uint8_t cmd, const uint8_t *payload, size_t payload_len, uint8_t *packet, size_t max_len);

// 协议发送函数
esp_err_t ble_protocol_send_response(uint16_t conn_id, uint8_t cmd, const uint8_t *payload, uint16_t payload_len);

// 协议验证函数
bool ble_protocol_validate_packet(const uint8_t *data, size_t len);

// 协议命令类型判断
bool ble_protocol_is_wifi_cmd(uint8_t cmd);
bool ble_protocol_is_ota_cmd(uint8_t cmd);

#ifdef __cplusplus
}
#endif

#endif // BLE_PROTOCOL_H
