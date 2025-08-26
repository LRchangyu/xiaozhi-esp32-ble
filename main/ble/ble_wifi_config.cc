#include "ble_wifi_config.h"
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

// 包含WiFi配置相关头文件 - 使用相对路径
extern "C" {
#include "ssid_manager.h"
}
// C++头文件不能在extern "C"块中包含
#include "wifi_configuration_ap.h"

// 从esp_ble.c中导入notify特性handle
extern "C" {
    extern uint16_t gatt_svr_notify_chr_val_handle;
}

#define TAG "BleWifiConfig"

// 队列数据结构
typedef struct {
    uint16_t conn_id;
    uint16_t handle;
    uint8_t data[256];  // 最大数据长度
    size_t data_len;
} ble_data_queue_item_t;

// 全局变量
static bool g_ble_initialized = false;
static bool g_ble_advertising = false;
static uint16_t g_conn_handle = 0xFFFF;
static std::function<void(const std::string&, const std::string&)> g_wifi_config_callback;

// 队列和线程相关变量
static QueueHandle_t g_ble_data_queue = NULL;
static TaskHandle_t g_ble_process_task = NULL;
static bool g_process_task_running = false;

// 协议处理相关
static uint8_t g_response_buffer[512];

// BLE事件处理函数声明
static void ble_wifi_config_event_handler(ble_evt_t *evt);
static int handle_get_wifi_config_cmd(uint8_t *response, size_t max_len);
static int handle_set_wifi_config_cmd(const uint8_t *payload, size_t payload_len, uint8_t *response, size_t max_len);
static int handle_get_scan_list_cmd(uint8_t *response, size_t max_len);

static bool parse_protocol_packet(const uint8_t *data, size_t len, uint8_t *cmd, const uint8_t **payload, size_t *payload_len);
// 数据处理线程函数
static void ble_data_process_task(void* pvParameters) {
    ble_data_queue_item_t queue_item;
    
    ESP_LOGI(TAG, "BLE data process task started");
    
    while (g_process_task_running) {
        // 从队列中获取数据，等待最多100ms
        if (xQueueReceive(g_ble_data_queue, &queue_item, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "Processing BLE data: conn_id=%d, handle=%d, len=%d", 
                     queue_item.conn_id, queue_item.handle, queue_item.data_len);
            
            // 解析协议数据包
            uint8_t cmd;
            const uint8_t *payload;
            size_t payload_len;
            
            if (!parse_protocol_packet(queue_item.data, queue_item.data_len, &cmd, &payload, &payload_len)) {
                ESP_LOGE(TAG, "Failed to parse protocol packet");
                continue;
            }
            
            // 处理命令
            size_t response_len = 0;
            switch (cmd) {
                case BLE_WIFI_CONFIG_CMD_GET_WIFI:
                    response_len = handle_get_wifi_config_cmd(g_response_buffer, sizeof(g_response_buffer));
                    break;
                    
                case BLE_WIFI_CONFIG_CMD_SET_WIFI:
                    response_len = handle_set_wifi_config_cmd(payload, payload_len, g_response_buffer, sizeof(g_response_buffer));
                    break;
                    
                case BLE_WIFI_CONFIG_CMD_GET_SCAN:
                    response_len = handle_get_scan_list_cmd(g_response_buffer, sizeof(g_response_buffer));
                    break;
                    
                default:
                    ESP_LOGW(TAG, "Unknown command: 0x%02X", cmd);
                    continue;
            }
            
            // 发送响应
            if (response_len > 0 && queue_item.conn_id != 0xFFFF) {
                esp_ble_notify_data(queue_item.conn_id, 
                                   gatt_svr_notify_chr_val_handle, 
                                   g_response_buffer, response_len);
            }
        }
    }
    
    ESP_LOGI(TAG, "BLE data process task ended");
    vTaskDelete(NULL);
}

// 协议数据包解析
static bool parse_protocol_packet(const uint8_t *data, size_t len, uint8_t *cmd, const uint8_t **payload, size_t *payload_len) {
    if (len < 3) {
        ESP_LOGE(TAG, "Packet too short: %d bytes", len);
        return false;
    }
    
    if (data[0] != BLE_WIFI_CONFIG_HEADER_BYTE1 || data[1] != BLE_WIFI_CONFIG_HEADER_BYTE2) {
        ESP_LOGE(TAG, "Invalid header: 0x%02X 0x%02X", data[0], data[1]);
        return false;
    }
    
    *cmd = data[2];
    *payload = (len > 3) ? &data[3] : nullptr;
    *payload_len = (len > 3) ? len - 3 : 0;
    
    ESP_LOGI(TAG, "Parsed packet: cmd=0x%02X, payload_len=%d", *cmd, *payload_len);
    return true;
}

// 构建响应数据包
static size_t build_response_packet(uint8_t cmd, const uint8_t *payload, size_t payload_len, uint8_t *response, size_t max_len) {
    if (max_len < 3 + payload_len) {
        ESP_LOGE(TAG, "Response buffer too small");
        return 0;
    }
    
    response[0] = BLE_WIFI_CONFIG_HEADER_BYTE1;
    response[1] = BLE_WIFI_CONFIG_HEADER_BYTE2;
    response[2] = cmd;
    
    if (payload && payload_len > 0) {
        memcpy(&response[3], payload, payload_len);
    }
    
    return 3 + payload_len;
}

// 获取当前WiFi配置
static int handle_get_wifi_config_cmd(uint8_t *response, size_t max_len) {
    ESP_LOGI(TAG, "Handling get WiFi config command");
    
    // 从SSID管理器获取当前WiFi配置
    auto& ssid_manager = SsidManager::GetInstance();
    const auto& ssid_list = ssid_manager.GetSsidList();
    
    if (ssid_list.empty()) {
        ESP_LOGW(TAG, "No saved WiFi configurations");
        // 返回空配置
        uint8_t empty_payload[] = {0, 0}; // ssid_len=0, password_len=0
        return build_response_packet(BLE_WIFI_CONFIG_CMD_GET_WIFI, empty_payload, sizeof(empty_payload), response, max_len);
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
    
    size_t result = build_response_packet(BLE_WIFI_CONFIG_CMD_GET_WIFI, payload, payload_size, response, max_len);
    delete[] payload;
    
    ESP_LOGI(TAG, "WiFi config response: ssid=%s, password_len=%d", ssid.c_str(), password.length());
    return result;
}

// 设置WiFi配置
static int handle_set_wifi_config_cmd(const uint8_t *payload, size_t payload_len, uint8_t *response, size_t max_len) {
    ESP_LOGI(TAG, "Handling set WiFi config command, payload_len=%d", payload_len);
    
    if (payload_len < 2) {
        ESP_LOGE(TAG, "Invalid payload length for set WiFi config");
        uint8_t error_resp = BLE_WIFI_CONFIG_RESP_ERROR;
        return build_response_packet(BLE_WIFI_CONFIG_CMD_SET_WIFI, &error_resp, 1, response, max_len);
    }
    
    // 解析载荷：ssid_len + ssid + password_len + password
    size_t offset = 0;
    uint8_t ssid_len = payload[offset++];
    
    if (offset + ssid_len >= payload_len) {
        ESP_LOGE(TAG, "Invalid SSID length");
        uint8_t error_resp = BLE_WIFI_CONFIG_RESP_ERROR;
        return build_response_packet(BLE_WIFI_CONFIG_CMD_SET_WIFI, &error_resp, 1, response, max_len);
    }
    
    std::string ssid((char*)&payload[offset], ssid_len);
    offset += ssid_len;
    
    if (offset >= payload_len) {
        ESP_LOGE(TAG, "Missing password length");
        uint8_t error_resp = BLE_WIFI_CONFIG_RESP_ERROR;
        return build_response_packet(BLE_WIFI_CONFIG_CMD_SET_WIFI, &error_resp, 1, response, max_len);
    }
    
    uint8_t password_len = payload[offset++];
    
    if (offset + password_len > payload_len) {
        ESP_LOGE(TAG, "Invalid password length");
        uint8_t error_resp = BLE_WIFI_CONFIG_RESP_ERROR;
        return build_response_packet(BLE_WIFI_CONFIG_CMD_SET_WIFI, &error_resp, 1, response, max_len);
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
    return build_response_packet(BLE_WIFI_CONFIG_CMD_SET_WIFI, &success_resp, 1, response, max_len);
}

// WiFi扫描事件处理
// 获取WiFi扫描列表 - 兼容现有WiFi配置AP扫描
static int handle_get_scan_list_cmd(uint8_t *response, size_t max_len) {
    ESP_LOGI(TAG, "Handling get scan list command");

    // 获取当前扫描结果
    std::vector<wifi_ap_record_t> local_scan_results = WifiConfigurationAp::GetInstance().GetAccessPoints();

    // 构建响应载荷
    uint16_t len_limit = 200; // 设置一个安全的MTU限制，避免超过BLE MTU
    uint8_t arr[len_limit];
    uint16_t offset = 0;
    size_t response_len = 0;
    
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
            response_len = build_response_packet(BLE_WIFI_CONFIG_CMD_GET_SCAN, 
                                               arr, offset, 
                                               response, max_len);
            if (response_len > 0 && g_conn_handle != 0xFFFF) {
                esp_ble_notify_data(g_conn_handle, gatt_svr_notify_chr_val_handle, response, response_len);
            }

            vTaskDelay(pdMS_TO_TICKS(10)); // 小延迟避免发送过快
        } else {
            break;
        }
        
    } while (i < local_scan_results.size());

    // 发送结束标记
    uint8_t end_marker[] = {0x00};
    response_len = build_response_packet(BLE_WIFI_CONFIG_CMD_GET_SCAN, end_marker, sizeof(end_marker), response, max_len);
    
    ESP_LOGI(TAG, "Scan list response sent, found %d APs", (int)local_scan_results.size());
    return response_len;
}

// BLE事件处理
static void ble_wifi_config_event_handler(ble_evt_t *evt) {
    if (!evt) return;
    
    if(g_conn_handle != BLE_HS_CONN_HANDLE_NONE
        && g_conn_handle != evt->params.data_received.conn_id){
        return;
    }
    switch (evt->evt_id) {
        case BLE_EVT_CONNECTED:
            if (evt->params.connected.role == BLE_GAP_ROLE_MASTER) {
                ESP_LOGI(TAG, "BLE connected as central, conn_id=%d", evt->params.connected.conn_id);
                break;
            } 
            ESP_LOGI(TAG, "BLE connected as peripheral, conn_id=%d", evt->params.connected.conn_id);
            
            g_conn_handle = evt->params.connected.conn_id;

            g_ble_advertising = false;
            break;
            
        case BLE_EVT_DISCONNECTED:
            ESP_LOGI(TAG, "BLE disconnected, conn_id=%d", evt->params.disconnected.conn_id);
            if (g_conn_handle == evt->params.disconnected.conn_id) {
                g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            }

            g_ble_advertising = true;
            break;
            
        case BLE_EVT_DATA_RECEIVED: {
            ESP_LOGI(TAG, "BLE data received, conn_id=%d, handle=%d, len=%d", 
                     evt->params.data_received.conn_id,
                     evt->params.data_received.handle,
                     evt->params.data_received.len);

            // 检查数据长度是否超过队列项的最大大小
            if (evt->params.data_received.len > sizeof(((ble_data_queue_item_t*)0)->data)) {
                ESP_LOGE(TAG, "Received data too large: %d bytes", evt->params.data_received.len);
                break;
            }

            // 创建队列项
            ble_data_queue_item_t queue_item;
            queue_item.conn_id = evt->params.data_received.conn_id;
            queue_item.handle = evt->params.data_received.handle;
            queue_item.data_len = evt->params.data_received.len;
            memcpy(queue_item.data, evt->params.data_received.p_data, evt->params.data_received.len);

            // 将数据放入队列
            if (g_ble_data_queue && xQueueSend(g_ble_data_queue, &queue_item, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGW(TAG, "Failed to queue BLE data, queue might be full");
            } else {
                ESP_LOGD(TAG, "BLE data queued successfully");
            }
            
            break;
        }
        
        case BLE_EVT_DATA_SENT:
            ESP_LOGD(TAG, "BLE data sent, conn_id=%d, handle=%d", 
                     evt->params.data_sent.conn_id,
                     evt->params.data_sent.handle);
            
            break;
            
        default:
            break;
    }
}

// C接口实现
extern "C" {

int ble_wifi_config_init(void) {
    if (g_ble_initialized) {
        ESP_LOGW(TAG, "BLE WiFi config already initialized");
        return 0;
    }
    
    // 创建数据处理队列
    g_ble_data_queue = xQueueCreate(4, sizeof(ble_data_queue_item_t));
    if (g_ble_data_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create BLE data queue");
        return -1;
    }
    
    // 启动数据处理线程
    g_process_task_running = true;
    BaseType_t task_ret = xTaskCreate(
        ble_data_process_task,
        "ble_data_proc",
        4096,  // 栈大小
        NULL,
        2,     // 优先级
        &g_ble_process_task
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create BLE data process task");
        vQueueDelete(g_ble_data_queue);
        g_ble_data_queue = NULL;
        g_process_task_running = false;
        return -1;
    }
    
    int ret = esp_ble_init(ble_wifi_config_event_handler);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to initialize BLE: %d", ret);
        
        // 清理资源
        g_process_task_running = false;
        if (g_ble_process_task) {
            vTaskDelete(g_ble_process_task);
            g_ble_process_task = NULL;
        }
        if (g_ble_data_queue) {
            vQueueDelete(g_ble_data_queue);
            g_ble_data_queue = NULL;
        }
        return ret;
    }
    
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
    
    // 停止数据处理线程
    g_process_task_running = false;
    
    // 等待任务结束，最多等待1秒
    if (g_ble_process_task) {
        int wait_count = 0;
        while (eTaskGetState(g_ble_process_task) != eDeleted && wait_count < 10) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_count++;
        }
        g_ble_process_task = NULL;
    }
    
    // 清理队列
    if (g_ble_data_queue) {
        vQueueDelete(g_ble_data_queue);
        g_ble_data_queue = NULL;
    }
    
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
