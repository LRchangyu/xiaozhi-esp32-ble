

#ifndef BLE_OTA_H
#define BLE_OTA_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_ble.h"
#include "ble_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// BLE OTA 协议使用公共协议定义
#define BLE_OTA_HEADER_0        BLE_PROTOCOL_HEADER_0
#define BLE_OTA_HEADER_1        BLE_PROTOCOL_HEADER_1

// BLE OTA 命令定义
#define BLE_OTA_CMD_SEND_FILE_INFO      BLE_PROTOCOL_CMD_SEND_FILE_INFO
#define BLE_OTA_CMD_SEND_FILE_DATA      BLE_PROTOCOL_CMD_SEND_FILE_DATA
#define BLE_OTA_CMD_SEND_PACKET_CRC     BLE_PROTOCOL_CMD_SEND_PACKET_CRC

// ACK 响应定义
#define BLE_OTA_ACK_SUCCESS             BLE_PROTOCOL_ACK_SUCCESS
#define BLE_OTA_ACK_ERROR               BLE_PROTOCOL_ACK_ERROR
#define BLE_OTA_ACK_VERSION_NOT_ALLOW   BLE_PROTOCOL_ACK_VERSION_NOT_ALLOW

// 数据包长度范围
#define BLE_OTA_PACKET_LEN_MIN          64
#define BLE_OTA_PACKET_LEN_MAX          4096

// OTA状态定义
typedef enum {
    BLE_OTA_STATE_IDLE = 0,
    BLE_OTA_STATE_WAIT_FILE_INFO,
    BLE_OTA_STATE_WAIT_FILE_DATA,
    BLE_OTA_STATE_WAIT_PACKET_CRC,
    BLE_OTA_STATE_UPGRADING,
    BLE_OTA_STATE_ERROR
} ble_ota_state_t;

// OTA进度回调函数类型
typedef void (*ble_ota_progress_callback_t)(int progress, const char* message);

// OTA模块初始化
esp_err_t ble_ota_init(ble_ota_progress_callback_t progress_cb);

// OTA模块去初始化
esp_err_t ble_ota_deinit(void);

// 获取当前OTA状态
ble_ota_state_t ble_ota_get_state(void);

// 重置OTA状态
void ble_ota_reset_state(void);

#ifdef __cplusplus
}

// C++ 接口
#include <functional>

class BleOta {
public:
    static BleOta& GetInstance();
    
    bool Initialize();
    void Deinitialize();
    bool IsInitialized() const { return initialized_; }
    
    // 设置进度回调函数
    void SetProgressCallback(std::function<void(int)> callback);
    
    // 设置完成回调函数  
    void SetCompleteCallback(std::function<void(bool)> callback);
    
    // 获取OTA状态
    ble_ota_state_t GetState() const;
    
    // 重置状态
    void ResetState();
    
private:
    BleOta() : initialized_(false) {}
    ~BleOta() = default;
    BleOta(const BleOta&) = delete;
    BleOta& operator=(const BleOta&) = delete;
    
    bool initialized_;
    std::function<void(int)> progress_callback_;
    std::function<void(bool)> complete_callback_;
    
    // 静态回调适配器
    static void StaticProgressCallback(int progress, const char* message);
    static BleOta* instance_;
};

#endif

#endif // BLE_OTA_H
