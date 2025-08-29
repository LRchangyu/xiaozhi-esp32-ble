#ifndef BLE_WIFI_CONFIG_H
#define BLE_WIFI_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "ble_protocol.h"

#ifdef __cplusplus
#include <string>
#include <functional>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// WiFi配置协议使用公共协议定义
#define BLE_WIFI_CONFIG_HEADER_BYTE1    BLE_PROTOCOL_HEADER_0
#define BLE_WIFI_CONFIG_HEADER_BYTE2    BLE_PROTOCOL_HEADER_1

// 命令定义
#define BLE_WIFI_CONFIG_CMD_GET_WIFI    BLE_PROTOCOL_CMD_GET_WIFI_CONFIG
#define BLE_WIFI_CONFIG_CMD_SET_WIFI    BLE_PROTOCOL_CMD_SET_WIFI_CONFIG
#define BLE_WIFI_CONFIG_CMD_GET_SCAN    BLE_PROTOCOL_CMD_GET_WIFI_SCAN

// 响应状态
#define BLE_WIFI_CONFIG_RESP_SUCCESS    BLE_PROTOCOL_ACK_SUCCESS
#define BLE_WIFI_CONFIG_RESP_ERROR      BLE_PROTOCOL_ACK_ERROR

// 协议相关常量
#define BLE_WIFI_CONFIG_TIMEOUT_MS      BLE_PROTOCOL_TIMEOUT_MS
#define BLE_WIFI_CONFIG_MAX_CONN_INTERVAL_MS BLE_PROTOCOL_MAX_CONN_INTERVAL_MS

// BLE 服务定义
#define BLE_WIFI_CONFIG_SERVICE_UUID_16     BLE_PROTOCOL_SERVICE_UUID_16
#define BLE_WIFI_CONFIG_CHAR_UUID_16        BLE_PROTOCOL_WRITE_CHAR_UUID_16

// 广播名称前缀
#define BLE_WIFI_CONFIG_ADV_NAME_PREFIX     BLE_PROTOCOL_ADV_NAME_PREFIX

// 函数声明
int ble_wifi_config_init(void);
int ble_wifi_config_start_advertising(const char* ap_ssid);
int ble_wifi_config_stop_advertising(void);
void ble_wifi_config_deinit(void);
void ble_wifi_config_disconnect(uint16_t conn_handle);

#ifdef __cplusplus
}

// C++ 接口
class BleWifiConfig {
public:
    static BleWifiConfig& GetInstance();
    
    bool Initialize();
    bool StartAdvertising(const std::string& ap_ssid);
    bool StopAdvertising();
    void Deinitialize();
    void Disconnect();

    // 设置回调函数
    void SetOnWifiConfigChanged(std::function<void(const std::string&, const std::string&)> callback);
    
private:
    BleWifiConfig() = default;
    ~BleWifiConfig() = default;
    BleWifiConfig(const BleWifiConfig&) = delete;
    BleWifiConfig& operator=(const BleWifiConfig&) = delete;
    
    bool initialized_;
    std::function<void(const std::string&, const std::string&)> wifi_config_callback_;
};

#endif

#endif // BLE_WIFI_CONFIG_H
