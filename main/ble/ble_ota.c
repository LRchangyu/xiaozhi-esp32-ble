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
    
} ble_ota_context_t;

static ble_ota_context_t g_ota_ctx = {0};

// 函数声明
static esp_err_t ble_ota_handle_send_file_info(uint16_t conn_id, const uint8_t *payload, uint16_t payload_len);
static esp_err_t ble_ota_handle_send_file_data(uint16_t conn_id, const uint8_t *payload, uint16_t payload_len);
static esp_err_t ble_ota_handle_send_packet_crc(uint16_t conn_id, const uint8_t *payload, uint16_t payload_len);
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

    ble_ota_register_handlers();

    ESP_LOGI(TAG, "BLE OTA module initialized successfully");
    return ESP_OK;
}

esp_err_t ble_ota_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing BLE OTA module");

    xSemaphoreTake(g_ota_ctx.mutex, portMAX_DELAY);

    ble_ota_unregister_handlers();

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

esp_err_t ble_ota_register_handlers(void)
{
    ESP_LOGI(TAG, "Registering BLE OTA protocol handlers");
    
    esp_err_t ret;
    
    // 注册文件信息处理器
    ret = ble_protocol_register_handler(BLE_OTA_CMD_SEND_FILE_INFO, 
                                        ble_ota_handle_send_file_info, 
                                        "ota_send_file_info");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register file info handler: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 注册文件数据处理器
    ret = ble_protocol_register_handler(BLE_OTA_CMD_SEND_FILE_DATA, 
                                        ble_ota_handle_send_file_data, 
                                        "ota_send_file_data");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register file data handler: %s", esp_err_to_name(ret));
        ble_protocol_unregister_handler(BLE_OTA_CMD_SEND_FILE_INFO);
        return ret;
    }
    
    // 注册数据包CRC处理器
    ret = ble_protocol_register_handler(BLE_OTA_CMD_SEND_PACKET_CRC, 
                                        ble_ota_handle_send_packet_crc, 
                                        "ota_send_packet_crc");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register packet CRC handler: %s", esp_err_to_name(ret));
        ble_protocol_unregister_handler(BLE_OTA_CMD_SEND_FILE_INFO);
        ble_protocol_unregister_handler(BLE_OTA_CMD_SEND_FILE_DATA);
        return ret;
    }
    
    ESP_LOGI(TAG, "BLE OTA protocol handlers registered successfully");
    return ESP_OK;
}

esp_err_t ble_ota_unregister_handlers(void)
{
    ESP_LOGI(TAG, "Unregistering BLE OTA protocol handlers");
    
    ble_protocol_unregister_handler(BLE_OTA_CMD_SEND_FILE_INFO);
    ble_protocol_unregister_handler(BLE_OTA_CMD_SEND_FILE_DATA);
    ble_protocol_unregister_handler(BLE_OTA_CMD_SEND_PACKET_CRC);
    
    ESP_LOGI(TAG, "BLE OTA protocol handlers unregistered");
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

static void check_expected_bytes(void)
{
    g_ota_ctx.expected_bytes = g_ota_ctx.file_size - g_ota_ctx.total_written >= g_ota_ctx.packet_length ? g_ota_ctx.packet_length : g_ota_ctx.file_size - g_ota_ctx.total_written;
}

static esp_err_t ble_ota_handle_send_file_info(uint16_t conn_id, const uint8_t *payload, uint16_t payload_len)
{
    ESP_LOGI(TAG, "Handle send file info");
    
    if (payload == NULL || payload_len != 11) { // 3 + 4 + 4 = 11 bytes
        ESP_LOGE(TAG, "Invalid file info data length: %d", payload_len);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_INFO, &ack, 1);
    }
    
    if (xSemaphoreTake(g_ota_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_INFO, &ack, 1);
    }
    
    // 解析文件信息
    memcpy(g_ota_ctx.version, payload, 3);
    g_ota_ctx.file_size = (payload[3] << 0) | (payload[4] << 8) | (payload[5] << 16) | (payload[6] << 24);
    g_ota_ctx.file_crc32 = (payload[7] << 0) | (payload[8] << 8) | (payload[9] << 16) | (payload[10] << 24);
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
    g_ota_ctx.packet_length = 964; // 244- 3 =241， 241*4 = 964

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

static esp_err_t ble_ota_handle_send_file_data(uint16_t conn_id, const uint8_t *payload, uint16_t payload_len)
{
    if (g_ota_ctx.state != BLE_OTA_STATE_WAIT_FILE_DATA && g_ota_ctx.state != BLE_OTA_STATE_WAIT_PACKET_CRC) {
        ESP_LOGE(TAG, "Not in correct state for file data: %d", g_ota_ctx.state);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        ble_ota_reset_state();
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_DATA, &ack, 1);
    }
    
    if (payload == NULL || payload_len == 0) {
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
    if (g_ota_ctx.received_bytes + payload_len > g_ota_ctx.expected_bytes) {
        ESP_LOGE(TAG, "Received more data than expected: %d + %d > %lu", 
                 g_ota_ctx.received_bytes, payload_len, g_ota_ctx.expected_bytes);
        g_ota_ctx.state = BLE_OTA_STATE_ERROR;
        xSemaphoreGive(g_ota_ctx.mutex);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        ble_ota_reset_state();
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_FILE_DATA, &ack, 1);
    }

    if (g_ota_ctx.ota_buffer) {
        memcpy(g_ota_ctx.ota_buffer + g_ota_ctx.received_bytes, payload, payload_len);
    }

    // 更新CRC32
    g_ota_ctx.packet_crc32 = lr_crc_compute(payload, payload_len,&g_ota_ctx.packet_crc32);
    g_ota_ctx.total_crc32 = lr_crc_compute(payload, payload_len,&g_ota_ctx.total_crc32);
    g_ota_ctx.received_bytes += payload_len;
    
    ESP_LOGD(TAG, "Received %d bytes, total: %lu/%lu", payload_len, g_ota_ctx.received_bytes, g_ota_ctx.expected_bytes);
    
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

static esp_err_t ble_ota_handle_send_packet_crc(uint16_t conn_id, const uint8_t *payload, uint16_t payload_len)
{
    ESP_LOGI(TAG, "Handle send packet CRC");
    
    if (g_ota_ctx.state != BLE_OTA_STATE_WAIT_PACKET_CRC) {
        ESP_LOGE(TAG, "Not waiting for packet CRC");
        uint8_t ack = BLE_OTA_ACK_ERROR;
        ble_ota_reset_state();
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_PACKET_CRC, &ack, 1);
    }
    
    if (payload == NULL || payload_len != 4) {
        ESP_LOGE(TAG, "Invalid CRC data length: %d", payload_len);
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_PACKET_CRC, &ack, 1);
    }
    
    if (xSemaphoreTake(g_ota_ctx.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        uint8_t ack = BLE_OTA_ACK_ERROR;
        return ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_PACKET_CRC, &ack, 1);
    }
    
    uint32_t received_crc = (payload[0] << 0) | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
    
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

    int ret = ble_protocol_send_response(conn_id, BLE_OTA_CMD_SEND_PACKET_CRC, ack, 5);
    if(g_ota_ctx.success_finish){
        if(g_ota_ctx.progress_callback){
            g_ota_ctx.progress_callback(100,"OTA升级成功，设备即将重启");
        }
    }
    return ret;
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


