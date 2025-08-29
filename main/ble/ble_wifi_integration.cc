/*
 * 蓝牙WiFi配网集成示例
 * 
 * 本文件展示了如何将蓝牙配网功能集成到xiaozhi-esp32项目中
 * 将此代码集成到application.cc中，可以在设备启动时自动启用蓝牙配网
 */

#include "ble_wifi_integration.h"
#include "ble_wifi_config.h"
#include "ble_ota.h"
#include "esp_log.h"
#include "wifi_configuration_ap.h"

static const char* TAG = "BLE_WIFI_INTEGRATION";

/*
 * 建议的集成方案：
 * 
 * 1. 在Application类中添加蓝牙配网相关的成员函数和变量
 * 2. 在Application::Start()中启动蓝牙配网功能
 * 3. 当WiFi配置改变时，触发WiFi连接
 */

namespace BleWifiIntegration {

// 静态变量，用于跟踪蓝牙配网状态
static bool ble_wifi_config_active = false;

// 前向声明
void StopBleWifiConfig();

// WiFi配置改变回调函数
static void OnWifiConfigChanged(const std::string& ssid, const std::string& password) {
    ESP_LOGI(TAG, "BLE WiFi config changed - SSID: %s", ssid.c_str());
    
    // 尝试连接到新的WiFi网络
    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    bool connected = wifi_ap.ConnectToWifi(ssid, password);
    
    if (connected) {
        ESP_LOGI(TAG, "Successfully connected to WiFi: %s", ssid.c_str());
        
        // 连接成功后，可以选择停止蓝牙配网以节省资源
        StopBleWifiConfig();
        
        // 同时清理BLE OTA服务
        auto& ble_ota = BleOta::GetInstance();
        ble_ota.Deinitialize();
        ESP_LOGI(TAG, "BLE OTA service deinitialized");
        
        ESP_LOGI(TAG, "Restarting in 1 second");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGW(TAG, "Failed to connect to WiFi: %s", ssid.c_str());
    }
}

// 启动蓝牙配网功能
bool StartBleWifiConfig() {
    if (ble_wifi_config_active) {
        ESP_LOGW(TAG, "BLE WiFi config already active");
        return true;
    }
    
    ESP_LOGI(TAG, "Starting BLE WiFi configuration service");
    
    // 获取BLE WiFi配置实例
    auto& ble_wifi_config = BleWifiConfig::GetInstance();
    
    // 初始化蓝牙配网功能
    if (!ble_wifi_config.Initialize()) {
        ESP_LOGE(TAG, "Failed to initialize BLE WiFi config");
        return false;
    }
    
    // 设置WiFi配置改变回调
    ble_wifi_config.SetOnWifiConfigChanged(OnWifiConfigChanged);
    
    // 获取当前AP的SSID用于广播名称
    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    std::string ap_ssid = "Xiaozhi" + wifi_ap.GetSsid();
    
    // 启动蓝牙广播
    if (!ble_wifi_config.StartAdvertising(ap_ssid)) {
        ESP_LOGE(TAG, "Failed to start BLE advertising");
        ble_wifi_config.Deinitialize();
        return false;
    }
    
    ble_wifi_config_active = true;
    ESP_LOGI(TAG, "BLE WiFi configuration started successfully");
    ESP_LOGI(TAG, "Advertising name: lr_wificfg-%s", ap_ssid.c_str());
    
    return true;
}

// 停止蓝牙配网功能
void StopBleWifiConfig() {
    if (!ble_wifi_config_active) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping BLE WiFi configuration service");
    
    auto& ble_wifi_config = BleWifiConfig::GetInstance();
    ble_wifi_config.Disconnect();
    ble_wifi_config.StopAdvertising();
    ble_wifi_config.Deinitialize();
    
    // 同时清理BLE OTA服务
    auto& ble_ota = BleOta::GetInstance();
    if (ble_ota.IsInitialized()) {
        ble_ota.Deinitialize();
        ESP_LOGI(TAG, "BLE OTA service deinitialized");
    }
    
    ble_wifi_config_active = false;
    ESP_LOGI(TAG, "BLE WiFi configuration stopped");
}

// 检查蓝牙配网是否活跃
bool IsBleWifiConfigActive() {
    return ble_wifi_config_active;
}

} // namespace BleWifiIntegration

