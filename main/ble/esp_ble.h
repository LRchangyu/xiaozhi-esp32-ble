#ifndef ESP_BLE_H
#define ESP_BLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_MAX_CONN  CONFIG_BT_CTRL_BLE_MAX_ACT

#define ADV_DATA_MAX_LEN 31
typedef struct
{
    uint8_t adv_len;// len
    uint8_t rsp_len;
    int8_t rssi;
    uint8_t mac[6];
    uint8_t addr_type;
    uint8_t data[ADV_DATA_MAX_LEN*2];
    // char name[31];
}adv_pk_t;

int esp_ble_adv_set_data(uint8_t *adv_data, uint8_t adv_data_len, uint8_t *scan_rsp_data, uint8_t scan_rsp_data_len);
int esp_ble_adv_stop(void);
int esp_ble_adv_start(uint16_t adv_interval_ms);

typedef void (*ble_scan_callback_t)(adv_pk_t *adv);
int esp_ble_scan_cb_register(ble_scan_callback_t callback);
int esp_ble_scan_cb_unregister(ble_scan_callback_t callback);
int esp_ble_scan_start(uint16_t scan_interval_ms, uint16_t scan_window_ms, uint16_t duration_s,bool active_scan);
int esp_ble_scan_stop(void);

int esp_ble_gap_set_advname(char* p_name);
int esp_ble_gap_get_mac(uint8_t *p_mac);

int esp_ble_connect(uint8_t* remote_bda, uint8_t remote_addr_type);
int esp_ble_disconnect(uint16_t conn_id);

uint16_t esp_ble_get_mtu(uint16_t conn_id);

int esp_ble_write_data( uint16_t conn_id, uint16_t handle, uint8_t *p_data, uint16_t len, uint8_t write_type);
int esp_ble_notify_data( uint16_t conn_id, uint16_t handle, uint8_t *p_data, uint16_t len);

uint16_t esp_ble_get_notify_handle(void);

typedef enum{
    BLE_EVT_CONNECTED,
    BLE_EVT_DISCONNECTED,
    BLE_EVT_DATA_RECEIVED,
    BLE_EVT_DATA_SENT,
}ble_evt_id_e;

typedef struct{
    uint16_t conn_id;
    uint8_t remote_bda[6];
    uint8_t remote_addr_type;
    uint8_t role;
}ble_evt_connected_t;

typedef struct{
    uint16_t conn_id;
    uint8_t remote_bda[6];
    uint8_t remote_addr_type;
}ble_evt_disconnected_t;

typedef struct{
    uint16_t conn_id;
    uint16_t handle;
    uint8_t *p_data;
    uint16_t len;
}ble_evt_data_received_t;

typedef struct{
    uint16_t conn_id;
    uint16_t handle;
    uint8_t *p_data;
    uint16_t len;
}ble_evt_data_sent_t;

typedef struct{
    ble_evt_id_e evt_id;
    union{
        ble_evt_connected_t connected;
        ble_evt_disconnected_t disconnected;
        ble_evt_data_received_t data_received;
        ble_evt_data_sent_t data_sent;
    }params;
}ble_evt_t;

typedef void (*ble_evt_callback_t)(ble_evt_t *evt);

#define BLE_EVT_CALLBACK_MAX 5

int esp_ble_register_evt_callback(ble_evt_callback_t callback);
int esp_ble_unregister_evt_callback(ble_evt_callback_t callback);

int esp_ble_init(void);

#ifdef __cplusplus
}
#endif

#endif // ESP_BLE_H
