#include "ble_protocol.h"
#include "esp_ble.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

static const char* TAG = "BLE_PROTOCOL";

// 数据队列结构
typedef struct {
    uint16_t conn_id;
    uint16_t handle;
    uint16_t len;
    uint8_t data[256];  // 最大数据长度
} ble_protocol_data_msg_t;

// 全局变量
static ble_protocol_cmd_handler_t g_handlers[BLE_PROTOCOL_MAX_HANDLERS];
static SemaphoreHandle_t g_handlers_mutex = NULL;
static QueueHandle_t g_data_queue = NULL;
static TaskHandle_t g_process_task = NULL;
static bool g_task_running = false;

// 任务配置
#define BLE_PROTOCOL_TASK_STACK_SIZE    4096
#define BLE_PROTOCOL_TASK_PRIORITY      3
#define BLE_PROTOCOL_QUEUE_SIZE         10

// 内部函数声明
static void ble_protocol_event_handler(ble_evt_t *evt);
static void ble_protocol_process_task(void *arg);
static esp_err_t ble_protocol_process_data(uint16_t conn_id, uint8_t *data, uint16_t len);

esp_err_t ble_protocol_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE protocol module");
    
    // 初始化处理器数组
    memset(g_handlers, 0, sizeof(g_handlers));
    
    // 创建互斥锁
    g_handlers_mutex = xSemaphoreCreateMutex();
    if (g_handlers_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create handlers mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // 创建数据队列
    g_data_queue = xQueueCreate(BLE_PROTOCOL_QUEUE_SIZE, sizeof(ble_protocol_data_msg_t));
    if (g_data_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create data queue");
        vSemaphoreDelete(g_handlers_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    
    
    // 注册BLE事件回调
    esp_err_t esp_ret = esp_ble_register_evt_callback(ble_protocol_event_handler);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register BLE callback: %s", esp_err_to_name(esp_ret));
        vQueueDelete(g_data_queue);
        vSemaphoreDelete(g_handlers_mutex);
        return esp_ret;
    }

    g_task_running = true;
    // 创建数据处理任务
    BaseType_t ret = xTaskCreate(
        ble_protocol_process_task,
        "ble_protocol_task",
        BLE_PROTOCOL_TASK_STACK_SIZE,
        NULL,
        BLE_PROTOCOL_TASK_PRIORITY,
        &g_process_task
    );
    
    if (ret != pdPASS) {
        g_task_running = false;
        ESP_LOGE(TAG, "Failed to create protocol task");
        esp_ble_unregister_evt_callback(ble_protocol_event_handler);
        vQueueDelete(g_data_queue);
        vSemaphoreDelete(g_handlers_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    
    ESP_LOGI(TAG, "BLE protocol module initialized successfully");
    return ESP_OK;
}

esp_err_t ble_protocol_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing BLE protocol module");
    
    // 取消注册BLE事件回调
    esp_ble_unregister_evt_callback(ble_protocol_event_handler);
    
    // 停止任务
    if (g_task_running) {
        g_task_running = false;
        
        // 等待任务退出
        vTaskDelay(pdMS_TO_TICKS(100));
        
        if (g_process_task) {
            vTaskDelete(g_process_task);
            g_process_task = NULL;
        }
    }
    
    // 清理队列
    if (g_data_queue) {
        vQueueDelete(g_data_queue);
        g_data_queue = NULL;
    }
    
    // 清理互斥锁
    if (g_handlers_mutex) {
        vSemaphoreDelete(g_handlers_mutex);
        g_handlers_mutex = NULL;
    }
    
    // 清理处理器数组
    memset(g_handlers, 0, sizeof(g_handlers));
    
    ESP_LOGI(TAG, "BLE protocol module deinitialized");
    return ESP_OK;
}

esp_err_t ble_protocol_register_handler(uint8_t cmd, ble_protocol_handler_t handler, const char* name)
{
    if (handler == NULL || name == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for handler registration");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(g_handlers_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take handlers mutex");
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t ret = ESP_ERR_NO_MEM;
    
    // 查找空闲槽位
    for (int i = 0; i < BLE_PROTOCOL_MAX_HANDLERS; i++) {
        if (g_handlers[i].handler == NULL) {
            g_handlers[i].cmd = cmd;
            g_handlers[i].handler = handler;
            g_handlers[i].name = name;
            ESP_LOGI(TAG, "Registered handler for cmd 0x%02X: %s", cmd, name);
            ret = ESP_OK;
            break;
        }
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No more handler slots available");
    }
    
    xSemaphoreGive(g_handlers_mutex);
    return ret;
}

esp_err_t ble_protocol_unregister_handler(uint8_t cmd)
{
    if (xSemaphoreTake(g_handlers_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take handlers mutex");
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    
    // 查找并移除处理器
    for (int i = 0; i < BLE_PROTOCOL_MAX_HANDLERS; i++) {
        if (g_handlers[i].cmd == cmd && g_handlers[i].handler != NULL) {
            ESP_LOGI(TAG, "Unregistered handler for cmd 0x%02X: %s", cmd, g_handlers[i].name);
            memset(&g_handlers[i], 0, sizeof(ble_protocol_cmd_handler_t));
            ret = ESP_OK;
            break;
        }
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Handler for cmd 0x%02X not found", cmd);
    }
    
    xSemaphoreGive(g_handlers_mutex);
    return ret;
}

static void ble_protocol_event_handler(ble_evt_t *evt)
{
    if (evt == NULL) {
        return;
    }
    
    switch (evt->evt_id) {
        case BLE_EVT_CONNECTED:
            ESP_LOGI(TAG, "BLE connected, conn_id: %d", evt->params.connected.conn_id);
            break;
            
        case BLE_EVT_DISCONNECTED:
            ESP_LOGI(TAG, "BLE disconnected, conn_id: %d", evt->params.disconnected.conn_id);
            break;
            
        case BLE_EVT_DATA_RECEIVED:
            {
                // 将数据放入队列中异步处理
                ble_protocol_data_msg_t msg;
                msg.conn_id = evt->params.data_received.conn_id;
                msg.handle = evt->params.data_received.handle;
                msg.len = evt->params.data_received.len;
                
                if (msg.len > sizeof(msg.data)) {
                    ESP_LOGE(TAG, "Data too large: %d bytes", msg.len);
                    break;
                }
                
                memcpy(msg.data, evt->params.data_received.p_data, msg.len);
                
                if (xQueueSend(g_data_queue, &msg, 0) != pdTRUE) {
                    ESP_LOGE(TAG, "Failed to send data to queue");
                }
            }
            break;
            
        default:
            break;
    }
}

static void ble_protocol_process_task(void *arg)
{
    ble_protocol_data_msg_t msg;
    
    ESP_LOGI(TAG, "BLE protocol process task started");
    
    while (g_task_running) {
        // 等待队列消息
        if (xQueueReceive(g_data_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // 处理数据
            ble_protocol_process_data(msg.conn_id, msg.data, msg.len);
        }
    }
    
    ESP_LOGI(TAG, "BLE protocol process task exited");
    vTaskDelete(NULL);
}

static esp_err_t ble_protocol_process_data(uint16_t conn_id, uint8_t *data, uint16_t len)
{
    // 检查最小包长度
    if (len < BLE_PROTOCOL_MIN_PACKET_LEN) {
        ESP_LOGE(TAG, "Received data too short: %d", len);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 解析协议包
    uint8_t cmd;
    const uint8_t *payload;
    size_t payload_len;
    
    if (!ble_protocol_parse_packet(data, len, &cmd, &payload, &payload_len)) {
        ESP_LOGD(TAG, "Not a valid protocol packet, ignoring");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Processing protocol command: 0x%02X, payload_len: %d", cmd, payload_len);
    
    // 查找并调用对应的处理器
    if (xSemaphoreTake(g_handlers_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
        
        for (int i = 0; i < BLE_PROTOCOL_MAX_HANDLERS; i++) {
            if (g_handlers[i].cmd == cmd && g_handlers[i].handler != NULL) {
                ESP_LOGI(TAG, "Calling handler: %s", g_handlers[i].name);
                ret = g_handlers[i].handler(conn_id, payload, payload_len);
                break;
            }
        }
        
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(TAG, "No handler found for command: 0x%02X", cmd);
        }
        
        xSemaphoreGive(g_handlers_mutex);
        return ret;
    } else {
        ESP_LOGE(TAG, "Failed to take handlers mutex");
        return ESP_ERR_TIMEOUT;
    }
}

bool ble_protocol_parse_packet(const uint8_t *data, size_t len, uint8_t *cmd, const uint8_t **payload, size_t *payload_len)
{
    if (data == NULL || cmd == NULL || payload == NULL || payload_len == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }
    
    if (len < BLE_PROTOCOL_MIN_PACKET_LEN) {
        ESP_LOGE(TAG, "Packet too short: %d bytes", len);
        return false;
    }
    
    if (data[0] != BLE_PROTOCOL_HEADER_0 || data[1] != BLE_PROTOCOL_HEADER_1) {
        ESP_LOGD(TAG, "Invalid header: 0x%02X 0x%02X", data[0], data[1]);
        return false;
    }
    
    *cmd = data[2];
    *payload = (len > 3) ? &data[3] : NULL;
    *payload_len = (len > 3) ? len - 3 : 0;
    
    ESP_LOGD(TAG, "Parsed packet: cmd=0x%02X, payload_len=%d", *cmd, *payload_len);
    return true;
}

size_t ble_protocol_build_packet(uint8_t cmd, const uint8_t *payload, size_t payload_len, uint8_t *packet, size_t max_len)
{
    if (packet == NULL) {
        ESP_LOGE(TAG, "Packet buffer is NULL");
        return 0;
    }
    
    size_t total_len = BLE_PROTOCOL_MIN_PACKET_LEN + payload_len;
    if (total_len > max_len) {
        ESP_LOGE(TAG, "Packet buffer too small: need %d, have %d", total_len, max_len);
        return 0;
    }
    
    if (payload_len > BLE_PROTOCOL_MAX_PAYLOAD_LEN) {
        ESP_LOGE(TAG, "Payload too large: %d bytes", payload_len);
        return 0;
    }
    
    packet[0] = BLE_PROTOCOL_HEADER_0;
    packet[1] = BLE_PROTOCOL_HEADER_1;
    packet[2] = cmd;
    
    if (payload && payload_len > 0) {
        memcpy(&packet[3], payload, payload_len);
    }
    
    ESP_LOGD(TAG, "Built packet: cmd=0x%02X, total_len=%d", cmd, total_len);
    return total_len;
}

esp_err_t ble_protocol_send_response(uint16_t conn_id, uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t packet_buffer[BLE_PROTOCOL_MIN_PACKET_LEN + BLE_PROTOCOL_MAX_PAYLOAD_LEN];
    
    size_t packet_len = ble_protocol_build_packet(cmd, payload, payload_len, packet_buffer, sizeof(packet_buffer));
    if (packet_len == 0) {
        ESP_LOGE(TAG, "Failed to build response packet");
        return ESP_ERR_INVALID_ARG;
    }
    
    uint16_t notify_handle = esp_ble_get_notify_handle();
    if (notify_handle == 0) {
        ESP_LOGE(TAG, "Invalid notify handle");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = esp_ble_notify_data(conn_id, notify_handle, packet_buffer, packet_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Response sent: cmd=0x%02X, len=%d", cmd, packet_len);
    }
    
    return ret;
}
