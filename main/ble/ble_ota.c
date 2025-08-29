#include "ble_ota.h"
#include "ble_protocol.h"
#include "esp_ble.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_err.h"
#include "esp_crc.h"
#include "host/ble_hs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdlib.h>

static const char* TAG = "BLE_OTA";

// OTA数据消息结构
typedef struct {
    uint16_t conn_id;
    uint16_t len;
    uint8_t data[];
} ble_ota_data_msg_t;

// 任务配置
#define BLE_OTA_TASK_STACK_SIZE     4096
#define BLE_OTA_TASK_PRIORITY       3
#define BLE_OTA_QUEUE_SIZE          10

static uint32_t lr_crc_compute(uint8_t const * p_data, uint32_t size,uint32_t*p_crc)
{
    uint32_t crc;
    if (p_crc == NULL) {
        crc = 0XFFFFFFFF;
    } else {
        crc = *p_crc;
    }

    // ESP_LOGI(TAG, "CRC init: 0x%08X", crc);

    for (uint32_t i = 0; i < size; i++)
    {
        crc = crc ^ p_data[i];
        for (uint32_t j = 8; j > 0; j--)
        {
            crc = (crc >> 1) ^ (0xEDB88320U & ((crc & 1) ? 0xFFFFFFFF : 0));
        }
    }
    return crc;
}


// OTA状态管理
typedef struct {
    ble_ota_state_t state;
    uint16_t conn_id;
    
    // 文件信息
    uint8_t version[3];
    uint32_t file_size;
    uint32_t file_crc32;
    
    // 数据包信息
    uint16_t packet_length;
    uint32_t received_bytes;
    uint32_t expected_bytes;
    uint32_t packet_crc32;

    uint32_t total_written;
    uint32_t total_crc32;
    
    // OTA操作
    esp_ota_handle_t ota_handle;
    const esp_partition_t* ota_partition;
    uint8_t* ota_buffer;

    bool success_finish;

    // 回调函数
    ble_ota_progress_callback_t progress_callback;
    
    // 互斥锁
    SemaphoreHandle_t mutex;
    
    // 队列和任务
    QueueHandle_t data_queue;
    TaskHandle_t task_handle;
    bool task_running;
} ble_ota_context_t;

static ble_ota_context_t g_ota_ctx = {0};

// 函数声明
static void ble_ota_event_handler(ble_evt_t *evt);
static void ble_ota_task(void *arg);
static esp_err_t ble_ota_process_data(uint16_t conn_id, uint8_t *data, uint16_t len);
static esp_err_t ble_ota_handle_send_file_info(uint16_t conn_id, uint8_t *data, uint16_t len);
static esp_err_t ble_ota_handle_send_file_data(uint16_t conn_id, uint8_t *data, uint16_t len);
static esp_err_t ble_ota_handle_send_packet_crc(uint16_t conn_id, uint8_t *data, uint16_t len);
static bool ble_ota_check_version(const uint8_t *new_version);

esp_err_t ble_ota_init(ble_ota_progress_callback_t progress_cb)
{
    ESP_LOGI(TAG, "Initializing BLE OTA module");
    
    memset(&g_ota_ctx, 0, sizeof(g_ota_ctx));
    g_ota_ctx.state = BLE_OTA_STATE_IDLE;
    g_ota_ctx.progress_callback = progress_cb;
    g_ota_ctx.mutex = xSemaphoreCreateMutex();
    
    if (g_ota_ctx.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // 创建数据队列
    g_ota_ctx.data_queue = xQueueCreate(BLE_OTA_QUEUE_SIZE, sizeof(ble_ota_data_msg_t*));
    if (g_ota_ctx.data_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create data queue");
        vSemaphoreDelete(g_ota_ctx.mutex);
        return ESP_ERR_NO_MEM;
    }
    
    // 创建OTA处理任务
    BaseType_t ret = xTaskCreate(
        ble_ota_task,
        "ble_ota_task",
        BLE_OTA_TASK_STACK_SIZE,
        NULL,
        BLE_OTA_TASK_PRIORITY,
        &g_ota_ctx.task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        vQueueDelete(g_ota_ctx.data_queue);
        vSemaphoreDelete(g_ota_ctx.mutex);
        return ESP_ERR_NO_MEM;
    }

    // 注册BLE事件回调
    esp_err_t esp_ret = esp_ble_register_evt_callback(ble_ota_event_handler);
    if (esp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register BLE callback: %s", esp_err_to_name(esp_ret));
        g_ota_ctx.task_running = false;
        vTaskDelete(g_ota_ctx.task_handle);
        vQueueDelete(g_ota_ctx.data_queue);
        vSemaphoreDelete(g_ota_ctx.mutex);
        return esp_ret;
    }
    
    ESP_LOGI(TAG, "BLE OTA module initialized successfully");
    g_ota_ctx.task_running = true;
    return ESP_OK;
}

esp_err_t ble_ota_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing BLE OTA module");
    
    // 取消注册BLE事件回调
    esp_ble_unregister_evt_callback(ble_ota_event_handler);
    
    // 停止任务
    if (g_ota_ctx.task_running) {
        g_ota_ctx.task_running = false;
        
        // 发送空消息通知任务退出
        ble_ota_data_msg_t* exit_msg = NULL;
        xQueueSend(g_ota_ctx.data_queue, &exit_msg, 0);
        
        // 等待任务退出
        vTaskDelay(pdMS_TO_TICKS(100));
        
        if (g_ota_ctx.task_handle) {
            vTaskDelete(g_ota_ctx.task_handle);
            g_ota_ctx.task_handle = NULL;
        }
    }
    
    // 清理队列
    if (g_ota_ctx.data_queue) {
        // 清理队列中剩余的消息
        ble_ota_data_msg_t* msg;
        while (xQueueReceive(g_ota_ctx.data_queue, &msg, 0) == pdTRUE) {
            if (msg) {
                free(msg);
            }
        }
        vQueueDelete(g_ota_ctx.data_queue);
        g_ota_ctx.data_queue = NULL;
    }
    
    // 如果正在进行OTA操作，终止它
    if (g_ota_ctx.state != BLE_OTA_STATE_IDLE && g_ota_ctx.ota_handle != 0) {
        esp_ota_abort(g_ota_ctx.ota_handle);
    }
    
    if (g_ota_ctx.mutex) {
        vSemaphoreDelete(g_ota_ctx.mutex);
    }
    
    memset(&g_ota_ctx, 0, sizeof(g_ota_ctx));
    
    return ESP_OK;
}

ble_ota_state_t ble_ota_get_state(void)
{
    return g_ota_ctx.state;
}

void ble_ota_reset_state(void)
{
    if (xSemaphoreTake(g_ota_ctx.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (g_ota_ctx.ota_handle != 0) {
            esp_ota_abort(g_ota_ctx.ota_handle);
            g_ota_ctx.ota_handle = 0;
        }
        
        g_ota_ctx.state = BLE_OTA_STATE_IDLE;
        g_ota_ctx.conn_id = 0;
        g_ota_ctx.file_size = 0;
        g_ota_ctx.file_crc32 = 0;
        g_ota_ctx.packet_length = 0;
        g_ota_ctx.received_bytes = 0;
        g_ota_ctx.expected_bytes = 0;
        g_ota_ctx.packet_crc32 = 0;
        g_ota_ctx.ota_partition = NULL;
        g_ota_ctx.total_crc32 = 0;
        g_ota_ctx.success_finish = false;

        if (g_ota_ctx.ota_buffer) {
            free(g_ota_ctx.ota_buffer);
            g_ota_ctx.ota_buffer = NULL;
        }

        xSemaphoreGive(g_ota_ctx.mutex);
    }
    
    ESP_LOGI(TAG, "OTA state reset to IDLE");
}

static void ble_ota_task(void *arg)
{
    ble_ota_data_msg_t* msg;
    
    ESP_LOGI(TAG, "BLE OTA task started");
    while(!g_ota_ctx.task_running){
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    while (g_ota_ctx.task_running) {
        // 等待队列消息
        if (xQueueReceive(g_ota_ctx.data_queue, &msg, pdMS_TO_TICKS(10000)) == pdTRUE) {
            if (msg == NULL) {
                // 收到退出信号
                ESP_LOGI(TAG, "BLE OTA task received exit signal");
                break;
            }
            
            // 处理数据
            ble_ota_process_data(msg->conn_id, msg->data, msg->len);
            
            // 释放消息内存
            free(msg);

            if(g_ota_ctx.success_finish) {
                // 处理成功完成的情况
                ESP_LOGI(TAG, "BLE OTA task completed successfully");
                g_ota_ctx.progress_callback(100,"OTA finished successfully.");
                break;
            }
        }
    }
    
    ESP_LOGI(TAG, "BLE OTA task exited");
    vTaskDelete(NULL);
}

static esp_err_t ble_ota_process_data(uint16_t conn_id, uint8_t *data, uint16_t len)
{
    // 检查最小包长度
    if (len < 3) {
        ESP_LOGE(TAG, "Received data too short: %d", len);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 解析协议包
    uint8_t cmd;
    const uint8_t *payload;
    size_t payload_len;
    
    if (!ble_protocol_parse_packet(data, len, &cmd, &payload, &payload_len)) {
        ESP_LOGD(TAG, "Not OTA protocol data, ignoring");
        return ESP_OK;
    }
    
    // 只处理OTA相关的命令
    if (!ble_protocol_is_ota_cmd(cmd)) {
        ESP_LOGD(TAG, "Not an OTA command: 0x%02X", cmd);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Processing OTA command: 0x%02X, payload_len: %d", cmd, payload_len);
    
    switch (cmd) {
        case BLE_OTA_CMD_SEND_FILE_INFO:
            return ble_ota_handle_send_file_info(conn_id, (uint8_t*)payload, payload_len);
            
        case BLE_OTA_CMD_SEND_FILE_DATA:
            return ble_ota_handle_send_file_data(conn_id, (uint8_t*)payload, payload_len);
            
        case BLE_OTA_CMD_SEND_PACKET_CRC:
            return ble_ota_handle_send_packet_crc(conn_id, (uint8_t*)payload, payload_len);
            
        default:
            ESP_LOGE(TAG, "Unknown OTA command: 0x%02X", cmd);
            return ESP_ERR_NOT_SUPPORTED;
    }
}

static void ble_ota_event_handler(ble_evt_t *evt)
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
            // 如果正在进行OTA且连接断开，重置状态
            if (g_ota_ctx.conn_id == evt->params.disconnected.conn_id) {
                ble_ota_reset_state();
            }
            break;
            
        case BLE_EVT_DATA_RECEIVED:
            {
                uint8_t *data = evt->params.data_received.p_data;
                uint16_t len = evt->params.data_received.len;
                uint16_t conn_id = evt->params.data_received.conn_id;
                
                // 检查最小包长度
                if (len < 3) {
                    ESP_LOGE(TAG, "Received data too short: %d", len);
                    return;
                }
                
                // 快速检查是否为OTA协议包
                if (data[0] != BLE_OTA_HEADER_0 || data[1] != BLE_OTA_HEADER_1) {
                    ESP_LOGD(TAG, "Not OTA protocol header, ignoring");
                    return;
                }
                
                // 检查是否为OTA命令
                uint8_t cmd = data[2];
                if (!ble_protocol_is_ota_cmd(cmd)) {
                    ESP_LOGD(TAG, "Not an OTA command: 0x%02X", cmd);
                    return;
                }
                
                // 分配消息内存
                ble_ota_data_msg_t* msg = (ble_ota_data_msg_t*)malloc(sizeof(ble_ota_data_msg_t) + len);
                if (msg == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate memory for OTA message");
                    return;
                }
                
                // 填充消息
                msg->conn_id = conn_id;
                msg->len = len;
                memcpy(msg->data, data, len);
                
                // 发送到队列
                if (xQueueSend(g_ota_ctx.data_queue, &msg, 0) != pdTRUE) {
                    ESP_LOGE(TAG, "Failed to send message to OTA queue");
                    free(msg);
                    return;
                }
                
                ESP_LOGD(TAG, "OTA data queued for processing");
            }
            break;
            
        default:
            break;
    }
    
}

static void check_expected_bytes(void)
{
    g_ota_ctx.expected_bytes = g_ota_ctx.file_size - g_ota_ctx.total_written >= g_ota_ctx.packet_length ? g_ota_ctx.packet_length : g_ota_ctx.file_size - g_ota_ctx.total_written;
}

static esp_err_t ble_ota_handle_send_file_info(uint16_t conn_id, uint8_t *data, uint16_t len)
{
    ESP_LOGI(TAG, "Handle send file info");
    
    if (data == NULL || len != 11) { // 3 + 4 + 4 = 11 bytes
        ESP_LOGE(TAG, "Invalid file info data length: %d", len);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_INFO, &ack, 1);
    }
    
    if (xSemaphoreTake(g_ota_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_INFO, &ack, 1);
    }
    
    // 解析文件信息
    memcpy(g_ota_ctx.version, data, 3);
    g_ota_ctx.file_size = (data[3] << 0) | (data[4] << 8) | (data[5] << 16) | (data[6] << 24);
    g_ota_ctx.file_crc32 = (data[7] << 0) | (data[8] << 8) | (data[9] << 16) | (data[10] << 24);
    g_ota_ctx.conn_id = conn_id;
    g_ota_ctx.received_bytes = 0;
    
    ESP_LOGI(TAG, "File info - Version: %d.%d.%d, Size: %lu, CRC32: 0x%08lX", 
             g_ota_ctx.version[0], g_ota_ctx.version[1], g_ota_ctx.version[2],
             g_ota_ctx.file_size, g_ota_ctx.file_crc32);
    
    // 检查版本是否允许升级
    if (!ble_ota_check_version(g_ota_ctx.version)) {
        ESP_LOGE(TAG, "Version not allowed for upgrade");
        g_ota_ctx.state = BLE_OTA_STATE_ERROR;
        xSemaphoreGive(g_ota_ctx.mutex);
        uint8_t ack = BLE_OTA_ACK_VERSION_NOT_ALLOW;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_INFO, &ack, 1);
    }
    
    // 获取OTA分区
    g_ota_ctx.ota_partition = esp_ota_get_next_update_partition(NULL);
    if (g_ota_ctx.ota_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get OTA partition");
        g_ota_ctx.state = BLE_OTA_STATE_ERROR;
        xSemaphoreGive(g_ota_ctx.mutex);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_INFO, &ack, 1);
    }

    ESP_LOGI(TAG, "Starting partition %s", g_ota_ctx.ota_partition->label);

    // 开始OTA
    esp_err_t ret = esp_ota_begin(g_ota_ctx.ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &g_ota_ctx.ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(ret));
        g_ota_ctx.state = BLE_OTA_STATE_ERROR;
        xSemaphoreGive(g_ota_ctx.mutex);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_INFO, &ack, 1);
    }

    ESP_LOGI(TAG, "esp_ota_begin %s", g_ota_ctx.ota_partition->label);
    
    // 设置数据包长度 (64-4096字节范围内)
    g_ota_ctx.packet_length = 4096; // 默认1KB

    g_ota_ctx.ota_buffer = (uint8_t *)malloc(g_ota_ctx.packet_length);
    if (g_ota_ctx.ota_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        g_ota_ctx.state = BLE_OTA_STATE_ERROR;
        xSemaphoreGive(g_ota_ctx.mutex);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_INFO, &ack, 1);
    }
    memset(g_ota_ctx.ota_buffer, 0xFF, g_ota_ctx.packet_length);
    g_ota_ctx.state = BLE_OTA_STATE_WAIT_FILE_DATA;
    check_expected_bytes();
    g_ota_ctx.packet_crc32 = 0;
    xSemaphoreGive(g_ota_ctx.mutex);
    
    // 发送响应：ack + packet_length
    uint8_t response[3];
    response[0] = BLE_OTA_ACK_SUCCESS;
    response[1] = g_ota_ctx.packet_length & 0xFF;
    response[2] = (g_ota_ctx.packet_length >> 8) & 0xFF;
    
    ble_gap_set_prefered_le_phy(conn_id,BLE_GAP_LE_PHY_2M_MASK,BLE_GAP_LE_PHY_2M_MASK,0);
    return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_INFO, response, 3);
}

static esp_err_t ble_ota_handle_send_file_data(uint16_t conn_id, uint8_t *data, uint16_t len)
{
    if (g_ota_ctx.state != BLE_OTA_STATE_WAIT_FILE_DATA && g_ota_ctx.state != BLE_OTA_STATE_WAIT_PACKET_CRC) {
        ESP_LOGE(TAG, "Not in correct state for file data: %d", g_ota_ctx.state);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        ble_ota_reset_state();
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_DATA, &ack, 1);
    }
    
    if (data == NULL || len == 0) {
        ESP_LOGE(TAG, "Invalid file data");
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_DATA, &ack, 1);
    }
    
    if (xSemaphoreTake(g_ota_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_DATA, &ack, 1);
    }
    
    // 检查是否开始新的数据包
    
    // 检查数据长度
    if (g_ota_ctx.received_bytes + len > g_ota_ctx.expected_bytes) {
        ESP_LOGE(TAG, "Received more data than expected: %d + %d > %lu", 
                 g_ota_ctx.received_bytes, len, g_ota_ctx.expected_bytes);
        g_ota_ctx.state = BLE_OTA_STATE_ERROR;
        xSemaphoreGive(g_ota_ctx.mutex);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        ble_ota_reset_state();
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_DATA, &ack, 1);
    }

    if (g_ota_ctx.ota_buffer) {
        memcpy(g_ota_ctx.ota_buffer + g_ota_ctx.received_bytes, data, len);
    }

    // 更新CRC32
    g_ota_ctx.packet_crc32 = lr_crc_compute(data, len,&g_ota_ctx.packet_crc32);
    g_ota_ctx.total_crc32 = lr_crc_compute(data, len,&g_ota_ctx.total_crc32);
    g_ota_ctx.received_bytes += len;
    
    ESP_LOGD(TAG, "Received %d bytes, total: %lu/%lu", len, g_ota_ctx.received_bytes, g_ota_ctx.expected_bytes);
    
    // 检查是否接收完一个数据包
    if (g_ota_ctx.received_bytes >= g_ota_ctx.expected_bytes) {

        // 写入OTA数据
        esp_err_t ret = esp_ota_write(g_ota_ctx.ota_handle, g_ota_ctx.ota_buffer , g_ota_ctx.received_bytes);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(ret));
            g_ota_ctx.state = BLE_OTA_STATE_ERROR;
            xSemaphoreGive(g_ota_ctx.mutex);
            uint8_t ack = BLE_OTA_ACK_ERROR;
            ble_ota_reset_state();
            return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_DATA, &ack, 1);
        }
        memset(g_ota_ctx.ota_buffer, 0xFF, g_ota_ctx.packet_length);
        ESP_LOGI(TAG, "Packet complete, waiting for CRC");
        g_ota_ctx.state = BLE_OTA_STATE_WAIT_PACKET_CRC;

        // 发送ACK
        uint8_t ack = BLE_OTA_ACK_SUCCESS;
        xSemaphoreGive(g_ota_ctx.mutex);
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_DATA, &ack, 1);
    }
    
    xSemaphoreGive(g_ota_ctx.mutex);
    return ESP_OK; // 不发送ACK，等待更多数据
}

static esp_err_t ble_ota_handle_send_packet_crc(uint16_t conn_id, uint8_t *data, uint16_t len)
{
    ESP_LOGI(TAG, "Handle send packet CRC");
    
    if (g_ota_ctx.state != BLE_OTA_STATE_WAIT_PACKET_CRC) {
        ESP_LOGE(TAG, "Not waiting for packet CRC");
        uint8_t ack = BLE_OTA_ACK_ERROR;
        ble_ota_reset_state();
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_PACKET_CRC, &ack, 1);
    }
    
    if (data == NULL || len != 4) {
        ESP_LOGE(TAG, "Invalid CRC data length: %d", len);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_PACKET_CRC, &ack, 1);
    }
    
    if (xSemaphoreTake(g_ota_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_PACKET_CRC, &ack, 1);
    }
    
    uint32_t received_crc = (data[0] << 0) | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
    
    ESP_LOGI(TAG, "Packet CRC check - Calculated: 0x%08lX, Received: 0x%08lX", 
             g_ota_ctx.packet_crc32, received_crc);
    
    uint8_t ack[5];

    ack[1] = g_ota_ctx.packet_crc32>>0;
    ack[2] = g_ota_ctx.packet_crc32>>8;
    ack[3] = g_ota_ctx.packet_crc32>>16;
    ack[4] = g_ota_ctx.packet_crc32>>24;

    if (g_ota_ctx.packet_crc32 == received_crc) {
        g_ota_ctx.packet_crc32 = 0;
        ESP_LOGI(TAG, "Packet CRC check passed");

        ack[0] = BLE_OTA_ACK_SUCCESS;

        // 检查是否完成整个文件的传输
        // 这里需要使用其他方法检查写入的总字节数
        g_ota_ctx.total_written += g_ota_ctx.received_bytes;

        if (g_ota_ctx.total_written >= g_ota_ctx.file_size) {
            ESP_LOGI(TAG, "File transfer complete, finalizing OTA");
            g_ota_ctx.state = BLE_OTA_STATE_UPGRADING;
            
            // 重置静态变量
            g_ota_ctx.total_written = 0;
            g_ota_ctx.state = BLE_OTA_STATE_UPGRADING;
            
            // 完成OTA
            esp_err_t ret = esp_ota_end(g_ota_ctx.ota_handle);
            if (ret == ESP_OK) {
                ret = esp_ota_set_boot_partition(g_ota_ctx.ota_partition);
                if (ret == ESP_OK) {
                    g_ota_ctx.success_finish = true;
                    ack[0] = BLE_OTA_ACK_SUCCESS;
                } else {
                    ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(ret));
                    ack[0] = BLE_OTA_ACK_ERROR;
                }
            } else {
                ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(ret));
                ack[0] = BLE_OTA_ACK_ERROR;
            }
            g_ota_ctx.ota_handle = 0;
        } else {
            // 重置数据包状态，准备接收下一个数据包或完成升级
            g_ota_ctx.received_bytes = 0;
            check_expected_bytes();
            g_ota_ctx.state = BLE_OTA_STATE_WAIT_FILE_DATA;
        }
    } else {
        ESP_LOGE(TAG, "Packet CRC check failed");
        ack[0] = BLE_OTA_ACK_ERROR;
        g_ota_ctx.state = BLE_OTA_STATE_ERROR;
    }
    if(ack[0] == BLE_OTA_ACK_ERROR){
        ble_ota_reset_state();
    }
    xSemaphoreGive(g_ota_ctx.mutex);

    return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_PACKET_CRC, ack, 5);
}

static bool ble_ota_check_version(const uint8_t *new_version)
{
    // 获取当前版本
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Current version: %s, New version: %d.%d.%d", 
             app_desc->version, new_version[0], new_version[1], new_version[2]);
    
    // 这里可以实现版本比较逻辑
    // 目前简单返回true，允许所有版本升级
    return true;
}


