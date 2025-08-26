#ifndef BLE_WIFI_CONFIG_H
#define BLE_WIFI_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
#include <string>
#include <functional>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 协议定义
#define BLE_WIFI_CONFIG_HEADER_BYTE1    0x58
#define BLE_WIFI_CONFIG_HEADER_BYTE2    0x5A

// 命令定义
#define BLE_WIFI_CONFIG_CMD_GET_WIFI    0x00    // 获取当前WiFi配置
#define BLE_WIFI_CONFIG_CMD_SET_WIFI    0x01    // 修改WiFi配置
#define BLE_WIFI_CONFIG_CMD_GET_SCAN    0x02    // 获取WiFi扫描列表

// 响应状态
#define BLE_WIFI_CONFIG_RESP_SUCCESS    0x00
#define BLE_WIFI_CONFIG_RESP_ERROR      0x01

// 协议相关常量
#define BLE_WIFI_CONFIG_TIMEOUT_MS      10000   // 10秒超时
#define BLE_WIFI_CONFIG_MAX_CONN_INTERVAL_MS 150 // 最大连接间隔150ms

// BLE 服务定义
#define BLE_WIFI_CONFIG_SERVICE_UUID_16     0xFDD0
#define BLE_WIFI_CONFIG_CHAR_UUID_16        0xFDD1

// 广播名称前缀
#define BLE_WIFI_CONFIG_ADV_NAME_PREFIX     "lr_wificfg-"

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
