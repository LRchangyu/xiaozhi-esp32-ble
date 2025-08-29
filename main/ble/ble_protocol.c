#include "ble_protocol.h"
#include "esp_ble.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "BLE_PROTOCOL";

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

bool ble_protocol_validate_packet(const uint8_t *data, size_t len)
{
    if (data == NULL || len < BLE_PROTOCOL_MIN_PACKET_LEN) {
        return false;
    }
    
    return (data[0] == BLE_PROTOCOL_HEADER_0 && data[1] == BLE_PROTOCOL_HEADER_1);
}

bool ble_protocol_is_wifi_cmd(uint8_t cmd)
{
    return (cmd >= BLE_PROTOCOL_CMD_GET_WIFI_CONFIG && cmd <= BLE_PROTOCOL_CMD_GET_WIFI_SCAN);
}

bool ble_protocol_is_ota_cmd(uint8_t cmd)
{
    return (cmd >= BLE_PROTOCOL_CMD_SEND_FILE_INFO && cmd <= BLE_PROTOCOL_CMD_SEND_PACKET_CRC);
}
