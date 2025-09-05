#include "ble_wifi_config.h"
#include "ble_protocol.h"
#include "esp_ble.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nimble/ble.h"
#include "host/ble_hs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <cJSON.h>
#include <functional>
#include <vector>
#include <algorithm>
#include "wifi_configuration_ap.h"
#include "ble_protocol.h"
// 包含WiFi配置相关头文件 - 使用相对路径
extern "C" {
#include "ssid_manager.h"
}

#define TAG "BleWifiConfig"

// 全局变量
static bool g_ble_initialized = false;
static bool g_ble_advertising = false;
static uint16_t g_conn_handle = 0xFFFF;
static std::function<void(const std::string&, const std::string&)> g_wifi_config_callback;

// 获取当前WiFi配置
static int handle_get_wifi_config_cmd(uint16_t conn_id ) {
    ESP_LOGI(TAG, "Handling get WiFi config command");
    
    // 从SSID管理器获取当前WiFi配置
    auto& ssid_manager = SsidManager::GetInstance();
    const auto& ssid_list = ssid_manager.GetSsidList();
    
    if (ssid_list.empty()) {
        ESP_LOGW(TAG, "No saved WiFi configurations");
        // 返回空配置
        uint8_t empty_payload[] = {0, 0}; // ssid_len=0, password_len=0
        return ble_protocol_send_response(conn_id, BLE_WIFI_CONFIG_CMD_GET_WIFI, empty_payload, sizeof(empty_payload));
    }
    
    // 取第一个（默认）配置
    const auto& default_ssid = ssid_list[0];
    const std::string& ssid = default_ssid.ssid;
    const std::string& password = default_ssid.password;
    
    // 构建响应载荷：ssid_len + ssid + password_len + password
    size_t payload_size = 1 + ssid.length() + 1 + password.length();
    uint8_t *payload = new uint8_t[payload_size];
    
    size_t offset = 0;
    payload[offset++] = ssid.length();
    memcpy(&payload[offset], ssid.c_str(), ssid.length());
    offset += ssid.length();
    payload[offset++] = password.length();
    memcpy(&payload[offset], password.c_str(), password.length());

    size_t result = ble_protocol_send_response(conn_id, BLE_WIFI_CONFIG_CMD_GET_WIFI, payload, payload_size);
    delete[] payload;
    
    ESP_LOGI(TAG, "WiFi config response: ssid=%s, password_len=%d", ssid.c_str(), password.length());
    return result;
}

// 设置WiFi配置
static int handle_set_wifi_config_cmd(uint16_t conn_id, const uint8_t *payload, size_t payload_len) {
    ESP_LOGI(TAG, "Handling set WiFi config command, payload_len=%d", payload_len);
    
    if (payload_len < 2) {
        ESP_LOGE(TAG, "Invalid payload length for set WiFi config");
        uint8_t error_resp = BLE_WIFI_CONFIG_RESP_ERROR;
        return ble_protocol_send_response(conn_id, BLE_WIFI_CONFIG_CMD_SET_WIFI, &error_resp, 1);
    }
    
    // 解析载荷：ssid_len + ssid + password_len + password
    size_t offset = 0;
    uint8_t ssid_len = payload[offset++];
    
    if (offset + ssid_len >= payload_len) {
        ESP_LOGE(TAG, "Invalid SSID length");
        uint8_t error_resp = BLE_WIFI_CONFIG_RESP_ERROR;
        return ble_protocol_send_response(conn_id, BLE_WIFI_CONFIG_CMD_SET_WIFI, &error_resp, 1);
    }
    
    std::string ssid((char*)&payload[offset], ssid_len);
    offset += ssid_len;
    
    if (offset >= payload_len) {
        ESP_LOGE(TAG, "Missing password length");
        uint8_t error_resp = BLE_WIFI_CONFIG_RESP_ERROR;
        return ble_protocol_send_response(conn_id, BLE_WIFI_CONFIG_CMD_SET_WIFI, &error_resp, 1);
    }
    
    uint8_t password_len = payload[offset++];
    
    if (offset + password_len > payload_len) {
        ESP_LOGE(TAG, "Invalid password length");
        uint8_t error_resp = BLE_WIFI_CONFIG_RESP_ERROR;
        return ble_protocol_send_response(conn_id, BLE_WIFI_CONFIG_CMD_SET_WIFI, &error_resp, 1);
    }
    
    std::string password((char*)&payload[offset], password_len);
    
    ESP_LOGI(TAG, "Setting WiFi config: ssid=%s, password_len=%d", ssid.c_str(), password.length());
    
    // 保存到SSID管理器
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
    
    // 如果有回调函数，通知WiFi配置改变
    if (g_wifi_config_callback) {
        g_wifi_config_callback(ssid, password);
    }
    
    // 返回成功响应
    uint8_t success_resp = BLE_WIFI_CONFIG_RESP_SUCCESS;
    return ble_protocol_send_response(conn_id, BLE_WIFI_CONFIG_CMD_SET_WIFI, &success_resp, 1);
}

// WiFi扫描事件处理
// 获取WiFi扫描列表 - 兼容现有WiFi配置AP扫描
static int handle_get_scan_list_cmd(uint16_t conn_id) {
    ESP_LOGI(TAG, "Handling get scan list command");

    // 获取当前扫描结果
    std::vector<wifi_ap_record_t> local_scan_results = WifiConfigurationAp::GetInstance().GetAccessPoints();
    int ret;
    // 构建响应载荷
    uint16_t len_limit = 200; // 设置一个安全的MTU限制，避免超过BLE MTU
    uint8_t arr[len_limit];
    uint16_t offset = 0;
    
    int i = 0;
    do {
        memset(arr, 0, sizeof(arr));
        arr[0] = 0;
        offset = 1;
        
        while (i < local_scan_results.size()) {
            const char* ssid_str = (const char*)local_scan_results[i].ssid;
            uint8_t ssid_len = strlen(ssid_str);
            
            // 检查是否会超出缓冲区
            if (offset + ssid_len + 1 > len_limit) {
                break;
            }
            
            arr[0]++;
            arr[offset++] = ssid_len;
            memcpy(&arr[offset], ssid_str, ssid_len);
            offset += ssid_len;
            i++;
        }

        if (arr[0] > 0) {
            ret = ble_protocol_send_response(conn_id, BLE_WIFI_CONFIG_CMD_GET_SCAN,
                                               arr, offset);
            
            vTaskDelay(pdMS_TO_TICKS(10)); // 小延迟避免发送过快
        } else {
            break;
        }
        
    } while (i < local_scan_results.size());

    // 发送结束标记
    uint8_t end_marker[] = {0x00};
    ret = ble_protocol_send_response(conn_id, BLE_WIFI_CONFIG_CMD_GET_SCAN, end_marker, sizeof(end_marker));

    ESP_LOGI(TAG, "Scan list response sent, found %d APs", (int)local_scan_results.size());
    return ret;
}

// C接口实现
extern "C" {

int ble_wifi_config_init(void) {
    if (g_ble_initialized) {
        ESP_LOGW(TAG, "BLE WiFi config already initialized");
        return 0;
    }

    int ret = esp_ble_init();
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to initialize BLE: %d", ret);
        return ret;
    }

    ble_protocol_init();

    ble_wifi_config_register_handlers();

    g_ble_initialized = true;
    ESP_LOGI(TAG, "BLE WiFi config initialized");
    return 0;
}

int ble_wifi_config_start_advertising(const char* ap_ssid) {
    if (!g_ble_initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        return -1;
    }
    
    if (g_ble_advertising) {
        ESP_LOGW(TAG, "Already advertising");
        return 0;
    }
    
    // 构建广播名称
    std::string adv_name = BLE_WIFI_CONFIG_ADV_NAME_PREFIX;
    if (ap_ssid) {
        adv_name += ap_ssid;
    } else {
        adv_name += "device";
    }
    
    // 设置广播名称
    int ret = esp_ble_gap_set_advname(const_cast<char*>(adv_name.c_str()));
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set advertising name: %d", ret);
        return ret;
    }
    
    // 构建广播数据
    uint8_t adv_data[31];
    size_t adv_len = 0;
    
    // Flags
    adv_data[adv_len++] = 2;  // Length
    adv_data[adv_len++] = 0x01;  // Flags
    adv_data[adv_len++] = 0x06;  // LE General Discoverable + BR/EDR Not Supported
    
    // Complete Local Name
    size_t name_len = adv_name.length();
    if (adv_len + 2 + name_len <= 31) {
        adv_data[adv_len++] = 1 + name_len;  // Length
        adv_data[adv_len++] = 0x09;  // Complete Local Name
        memcpy(&adv_data[adv_len], adv_name.c_str(), name_len);
        adv_len += name_len;
    }
    
    // 16-bit Service UUID
    if (adv_len + 4 <= 31) {
        adv_data[adv_len++] = 3;  // Length
        adv_data[adv_len++] = 0x03;  // Complete List of 16-bit Service UUIDs
        adv_data[adv_len++] = (BLE_WIFI_CONFIG_SERVICE_UUID_16 & 0xFF);
        adv_data[adv_len++] = (BLE_WIFI_CONFIG_SERVICE_UUID_16 >> 8) & 0xFF;
    }
    
    ret = esp_ble_adv_set_data(adv_data, adv_len, nullptr, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set advertising data: %d", ret);
        return ret;
    }
    
    // 开始广播
    ret = esp_ble_adv_start(100); // 100ms间隔
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to start advertising: %d", ret);
        return ret;
    }
    
    g_ble_advertising = true;
    ESP_LOGI(TAG, "Started BLE advertising with name: %s", adv_name.c_str());
    return 0;
}

int ble_wifi_config_stop_advertising(void) {
    if (!g_ble_advertising) {
        return 0;
    }
    
    int ret = esp_ble_adv_stop();
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to stop advertising: %d", ret);
        return ret;
    }
    
    g_ble_advertising = false;
    ESP_LOGI(TAG, "Stopped BLE advertising");
    return 0;
}

void ble_wifi_config_deinit(void) {
    if (!g_ble_initialized) {
        return;
    }
    
    ble_wifi_config_stop_advertising();
    
    ble_wifi_config_unregister_handlers();
    
    g_ble_initialized = false;
    ESP_LOGI(TAG, "BLE WiFi config deinitialized");
}

void ble_wifi_config_disconnect(uint16_t conn_handle) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    
    int ret = esp_ble_disconnect(conn_handle);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to disconnect BLE connection: %d", ret);
    } else {
        ESP_LOGI(TAG, "Disconnected BLE connection, conn_id=%d", conn_handle);
    }
}

} // extern "C"

// C++接口实现
BleWifiConfig& BleWifiConfig::GetInstance() {
    static BleWifiConfig instance;
    return instance;
}

bool BleWifiConfig::Initialize() {
    return ble_wifi_config_init() == 0;
}

bool BleWifiConfig::StartAdvertising(const std::string& ap_ssid) {
    return ble_wifi_config_start_advertising(ap_ssid.c_str()) == 0;
}

bool BleWifiConfig::StopAdvertising() {
    return ble_wifi_config_stop_advertising() == 0;
}

void BleWifiConfig::Disconnect() {
    if(g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_wifi_config_disconnect(g_conn_handle);

        while(g_conn_handle != BLE_HS_CONN_HANDLE_NONE){
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void BleWifiConfig::Deinitialize() {
    ble_wifi_config_deinit();
}

void BleWifiConfig::SetOnWifiConfigChanged(std::function<void(const std::string&, const std::string&)> callback) {
    g_wifi_config_callback = callback;
}

// 协议处理器包装函数
static esp_err_t ble_wifi_get_config_handler(uint16_t conn_id, const uint8_t *payload, uint16_t payload_len)
{
    ESP_LOGI(TAG, "Handling get WiFi config command");
    g_conn_handle = conn_id;
    int ret = handle_get_wifi_config_cmd(conn_id);

    if(ret){
        ESP_LOGE(TAG, "Failed to get WiFi config: %d", ret);
    }
    return ret;
}

static esp_err_t ble_wifi_set_config_handler(uint16_t conn_id, const uint8_t *payload, uint16_t payload_len)
{
    ESP_LOGI(TAG, "Handling set WiFi config command");
    g_conn_handle = conn_id;

    int ret = handle_set_wifi_config_cmd(conn_id,payload, payload_len);

    if (ret) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %d", ret);
    }
   return ret;
}

static esp_err_t ble_wifi_get_scan_handler(uint16_t conn_id, const uint8_t *payload, uint16_t payload_len)
{
    ESP_LOGI(TAG, "Handling get WiFi scan command");
    g_conn_handle = conn_id;

    int ret = handle_get_scan_list_cmd(conn_id);
    
    if (ret) {
        ESP_LOGE(TAG, "Failed to get WiFi scan list: %d", ret);
    }
   return ret;
}

extern "C" {

esp_err_t ble_wifi_config_register_handlers(void)
{
    ESP_LOGI(TAG, "Registering BLE WiFi config protocol handlers");
    
    esp_err_t ret;
    
    // 注册获取WiFi配置处理器
    ret = ble_protocol_register_handler(BLE_WIFI_CONFIG_CMD_GET_WIFI, 
                                        ble_wifi_get_config_handler, 
                                        "wifi_get_config");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register get WiFi config handler: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 注册设置WiFi配置处理器
    ret = ble_protocol_register_handler(BLE_WIFI_CONFIG_CMD_SET_WIFI, 
                                        ble_wifi_set_config_handler, 
                                        "wifi_set_config");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register set WiFi config handler: %s", esp_err_to_name(ret));
        ble_protocol_unregister_handler(BLE_WIFI_CONFIG_CMD_GET_WIFI);
        return ret;
    }
    
    // 注册WiFi扫描处理器
    ret = ble_protocol_register_handler(BLE_WIFI_CONFIG_CMD_GET_SCAN, 
                                        ble_wifi_get_scan_handler, 
                                        "wifi_get_scan");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi scan handler: %s", esp_err_to_name(ret));
        ble_protocol_unregister_handler(BLE_WIFI_CONFIG_CMD_GET_WIFI);
        ble_protocol_unregister_handler(BLE_WIFI_CONFIG_CMD_SET_WIFI);
        return ret;
    }
    
    ESP_LOGI(TAG, "BLE WiFi config protocol handlers registered successfully");
    return ESP_OK;
}

esp_err_t ble_wifi_config_unregister_handlers(void)
{
    ESP_LOGI(TAG, "Unregistering BLE WiFi config protocol handlers");
    
    ble_protocol_unregister_handler(BLE_WIFI_CONFIG_CMD_GET_WIFI);
    ble_protocol_unregister_handler(BLE_WIFI_CONFIG_CMD_SET_WIFI);
    ble_protocol_unregister_handler(BLE_WIFI_CONFIG_CMD_GET_SCAN);
    
    ESP_LOGI(TAG, "BLE WiFi config protocol handlers unregistered");
    return ESP_OK;
}

}
