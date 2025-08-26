#include "esp_ble.h"

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_mac.h"

#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_timer.h"

#include "modlog/modlog.h"

static const char* TAG = "esp_ble";

#ifdef USR_DEBUG_ENABLED
#define USR_ESP_BLE_LOG_LEVEL ESP_LOG_DEBUG
#else
#define USR_ESP_BLE_LOG_LEVEL ESP_LOG_INFO
#endif

#define MAX_CONN_INSTANCES (BLE_MAX_CONN+1)
#define BLE_MTU_MAX CONFIG_NIMBLE_ATT_PREFERRED_MTU
#define OWN_ADDR_TYPE BLE_OWN_ADDR_RANDOM
static bool m_ble_sync_flag = false;
static bool m_ble_scan_need_recover = false;
struct ble_hs_cfg ble_hs_cfg;
static struct ble_gap_adv_params adv_params;
static uint16_t m_mtu = 23;
static bool m_notify_en = false;

static ble_evt_callback_t g_ble_event_callback = NULL;

///Declare static functions
static int adv_start(void);
static int ble_gap_event(struct ble_gap_event *event, void *arg);

typedef struct{
    uint16_t adv_cnts;
    uint16_t rsp_cnts;
}scan_test_t;
static scan_test_t m_scan_test = {0};

#define SCAN_CB_MAX 1
static ble_scan_callback_t m_scan_callback[SCAN_CB_MAX] ;
///Declare static functions
// static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void mac_rever(uint8_t*p_tar,uint8_t*p_src)
{
    uint8_t src_cpy[6];
    
    if(p_tar ==NULL||p_src==NULL)
      return;
    memcpy(src_cpy,p_src,6);
    p_tar[0] = src_cpy[5];
    p_tar[1] = src_cpy[4];
    p_tar[2] = src_cpy[3];
    p_tar[3] = src_cpy[2];
    p_tar[4] = src_cpy[1];
    p_tar[5] = src_cpy[0];

}
//=============================================================================================================================//
/*
gatts info
*/
//=============================================================================================================================//

static const ble_uuid128_t gatt_svr_svc_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00,
    0x00, 0x80,
    0x00, 0x10,
    0x00, 0x00,
    0xD0, 0xFD, 0x00, 0x00);

/* A characteristic that can be subscribed to */
static uint16_t gatt_svr_write_chr_val_handle;
static const ble_uuid128_t gatt_svr_write_chr_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00,
    0x00, 0x80,
    0x00, 0x10,
    0x00, 0x00,
    0xD1, 0xFD, 0x00, 0x00);

uint16_t gatt_svr_notify_chr_val_handle; // 改为非static，可以被外部访问
static const ble_uuid128_t gatt_svr_notify_chr_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00,
    0x00, 0x80,
    0x00, 0x10,
    0x00, 0x00,
    0xD2, 0xFD, 0x00, 0x00);

static int gatt_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                struct ble_gatt_access_ctxt *ctxt,
                void *arg);

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        /*** Service ***/
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        { {
                /*** This characteristic can be subscribed to by writing 0x00 and 0x01 to the CCCD ***/
                .uuid = &gatt_svr_write_chr_uuid.u,
                .access_cb = gatt_svc_access,

                .flags =  BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP ,

                .val_handle = &gatt_svr_write_chr_val_handle,
                
            }, 

            {
                /*** This characteristic can be subscribed to by writing 0x00 and 0x01 to the CCCD ***/
                .uuid = &gatt_svr_notify_chr_uuid.u,
                .access_cb = gatt_svc_access,

                .flags =  BLE_GATT_CHR_F_NOTIFY ,

                .val_handle = &gatt_svr_notify_chr_val_handle,
                
            }, 

            {
                0, /* No more characteristics in this service. */
            }
        },
    },

    {
        0, /* No more services. */
    },
};

static int gatt_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // const ble_uuid_t *uuid;
    int rc = 0;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            MODLOG_DFLT(INFO, "Characteristic read; conn_handle=%d attr_handle=%d\n",
                        conn_handle, attr_handle);
        } else {
            MODLOG_DFLT(INFO, "Characteristic read by NimBLE stack; attr_handle=%d\n",
                        attr_handle);
        }
        // 对于读操作，返回空数据
        rc = BLE_ATT_ERR_READ_NOT_PERMITTED;
        break;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            MODLOG_DFLT(INFO, "Characteristic write; conn_handle=%d attr_handle=%d",
                        conn_handle, attr_handle);
        } else {
            MODLOG_DFLT(INFO, "Characteristic write by NimBLE stack; attr_handle=%d",
                        attr_handle);
        }
        
        if (attr_handle == gatt_svr_write_chr_val_handle) {
            if (ctxt->om != NULL && g_ble_event_callback != NULL) {
                // 获取数据长度
                uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);
                
                // 分配缓冲区并复制数据
                uint8_t *data_buffer = malloc(data_len);
                if (data_buffer != NULL) {
                    int ret = ble_hs_mbuf_to_flat(ctxt->om, data_buffer, data_len, NULL);
                    if (ret == 0) {
                        // 构造事件并调用回调
                        ble_evt_t evt;
                        evt.evt_id = BLE_EVT_DATA_RECEIVED;
                        evt.params.data_received.conn_id = conn_handle;
                        evt.params.data_received.handle = attr_handle;
                        evt.params.data_received.p_data = data_buffer;
                        evt.params.data_received.len = data_len;
                        
                        g_ble_event_callback(&evt);
                    }
                    free(data_buffer);
                }
                rc = 0;
            } else {
                ESP_LOGE(TAG, "conn_handle %d write data is NULL or no callback",conn_handle);
                rc = BLE_ATT_ERR_INVALID_PDU;
            }
        } else {
            rc = BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        break;

    case BLE_GATT_ACCESS_OP_READ_DSC:
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            MODLOG_DFLT(INFO, "Descriptor read; conn_handle=%d attr_handle=%d\n",
                        conn_handle, attr_handle);
        } else {
            MODLOG_DFLT(INFO, "Descriptor read by NimBLE stack; attr_handle=%d\n",
                        attr_handle);
        }
        rc = BLE_ATT_ERR_READ_NOT_PERMITTED;
        break;

    case BLE_GATT_ACCESS_OP_WRITE_DSC:
        rc = BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        break;

    default:
        rc = BLE_ATT_ERR_UNLIKELY;
        break;
    }

    return rc;
}

void gatts_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        MODLOG_DFLT(INFO, "registered service %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        MODLOG_DFLT(INFO, "registering characteristic %s with "
                    "def_handle=%d val_handle=%d\n",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle,
                    ctxt->chr.val_handle);
        
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        MODLOG_DFLT(INFO, "registering descriptor %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                    ctxt->dsc.handle);
        break;

    default:
        assert(0);
        break;
    }
}
static int gatts_init(void)
{
    int rc;

    ble_svc_gap_init();
    
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    return 0;
}
//=============================================================================================================================//
/*
gattc info
*/
//=============================================================================================================================//
typedef struct{
    uint16_t start_handle;
    uint16_t end_handle;

    uint16_t write_handle;
    uint16_t notify_handle;
    uint16_t cccd_handle;

}gattc_service_t;

static gattc_service_t m_svr_info[MAX_CONN_INSTANCES];
static void gattc_service_info_rst(uint16_t conn_handle)
{
    if(conn_handle >= MAX_CONN_INSTANCES)
        return;
    gattc_service_t*p_info = &m_svr_info[conn_handle];
    p_info->start_handle = 0;
    p_info->end_handle = 0;
    p_info->write_handle = 0;
    p_info->notify_handle = 0;
    p_info->cccd_handle = 0;
}

static inline gattc_service_t* gattc_get_service_info(uint16_t conn_handle)
{
    if(conn_handle >= MAX_CONN_INSTANCES)
        return NULL;
    return &m_svr_info[conn_handle];
}
// typedef int ble_gatt_dsc_fn(uint16_t conn_handle,
//                             const struct ble_gatt_error *error,
//                             uint16_t chr_val_handle,
//                             const struct ble_gatt_dsc *dsc,
//                             void *arg);

static int gattc_desc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            uint16_t chr_val_handle,
                            const struct ble_gatt_dsc *dsc,
                            void *arg)
{
    char buf[BLE_UUID_STR_LEN+1];
    
    if(conn_handle >= MAX_CONN_INSTANCES) {
        ESP_LOGE(TAG, "gattc_desc_cb: Invalid conn_handle %d", conn_handle);
        return BLE_HS_EINVAL;
    }
    
    gattc_service_t*p_info = gattc_get_service_info(conn_handle);
    if(p_info == NULL) {
        ESP_LOGE(TAG, "gattc_desc_cb: Invalid service info for conn_handle %d", conn_handle);
        return BLE_HS_EINVAL;
    }
    switch (error->status) {
    case 0:
        ESP_LOGI(TAG, "Descriptor discovered; conn_handle=%d handle=%d uuid=%s",
                                conn_handle,dsc->handle,
                                ble_uuid_to_str((const ble_uuid_t *)&dsc->uuid,buf));
        if(chr_val_handle == p_info->notify_handle && p_info->cccd_handle == 0
          && dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == BLE_GATT_DSC_CLT_CFG_UUID16)
        {
            p_info->cccd_handle = dsc->handle;

            uint8_t cccd_en[2] = {0x01,0x00};
            int ret = esp_ble_write_data(conn_handle,p_info->cccd_handle,cccd_en,2,BLE_GATT_CHR_PROP_WRITE);
            if(ret){
                ESP_LOGE(TAG, "Conn :%d cccd %02x write failed:%d",conn_handle,p_info->cccd_handle,ret);
            }
        }
        
        break;

    case BLE_HS_EDONE:
        ESP_LOGI(TAG, "Descriptors discovery complete; conn_handle=%d status=%d",conn_handle,error->status);
        break;
        
    default:
        ESP_LOGE(TAG, "Error: Characteristic discovery failed; status=%d conn_handle=%d",error->status,conn_handle);
        break;
    }
    
    return 0;
}

// typedef int ble_gatt_chr_fn(uint16_t conn_handle,
//                             const struct ble_gatt_error *error,
//                             const struct ble_gatt_chr *chr, void *arg);

static int char_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{

    char buf[BLE_UUID_STR_LEN+1];
    
    if(conn_handle >= MAX_CONN_INSTANCES) {
        ESP_LOGE(TAG, "char_disc_cb: Invalid conn_handle %d", conn_handle);
        return BLE_HS_EINVAL;
    }
    
    gattc_service_t*p_info = gattc_get_service_info(conn_handle);
    if(p_info == NULL) {
        ESP_LOGE(TAG, "char_disc_cb: Invalid service info for conn_handle %d", conn_handle);
        return BLE_HS_EINVAL;
    }

    switch (error->status) {
    case 0:
        ESP_LOGI(TAG, "Characteristic discovered; conn_handle=%d def_handle=%d val_handle=%d prop:%x,uuid=%s",
                                conn_handle,chr->def_handle,
                                chr->val_handle,
                                chr->properties,
                                ble_uuid_to_str((const ble_uuid_t *)&chr->uuid,buf));
        if(chr->uuid.u.type == gatt_svr_write_chr_uuid.u.type
        && ((chr->uuid.u.type == BLE_UUID_TYPE_16 && chr->uuid.u16.value == BLE_UUID16(&gatt_svr_write_chr_uuid)->value)
            || (chr->uuid.u.type == BLE_UUID_TYPE_128 && memcmp(chr->uuid.u128.value,BLE_UUID128(&gatt_svr_write_chr_uuid)->value,16) == 0))
        ){
            if((chr->properties &(BLE_GATT_CHR_PROP_WRITE_NO_RSP | BLE_GATT_CHR_PROP_WRITE))
            && p_info->write_handle == 0){
                p_info->write_handle = chr->val_handle;
                ESP_LOGI(TAG, "write_handle=%d",p_info->write_handle);
                
            }
        }else if(chr->uuid.u.type == gatt_svr_notify_chr_uuid.u.type
        && ((chr->uuid.u.type == BLE_UUID_TYPE_16 && chr->uuid.u16.value == BLE_UUID16(&gatt_svr_notify_chr_uuid)->value)
            || (chr->uuid.u.type == BLE_UUID_TYPE_128 && memcmp(chr->uuid.u128.value,BLE_UUID128(&gatt_svr_notify_chr_uuid)->value,16) == 0))
        ){
            if(chr->properties & BLE_GATT_CHR_PROP_NOTIFY && p_info->notify_handle == 0){
                p_info->notify_handle = chr->val_handle;
                ESP_LOGI(TAG, "notify_handle=%d",p_info->notify_handle);

            }
            
        }
        
        break;

    case BLE_HS_EDONE:
        ESP_LOGI(TAG, "Characteristic discovery complete; conn_handle=%d status=%d",conn_handle,error->status);
        if(p_info->write_handle == 0 && p_info->notify_handle == 0){
            ESP_LOGE(TAG, "Characteristic not found");
            esp_ble_disconnect(conn_handle);
        }else if(p_info->notify_handle != 0 && p_info->notify_handle > p_info->start_handle && p_info->notify_handle < p_info->end_handle){
            ble_gattc_disc_all_dscs(conn_handle,p_info->notify_handle,p_info->end_handle,gattc_desc_cb,NULL);
        }else{
        }
        break;
        
    default:
        ESP_LOGE(TAG, "Error: Characteristic discovery failed; status=%d conn_handle=%d",error->status,conn_handle);
        break;
    }
    
    return 0;
}

static int gattc_find_char(uint16_t conn_handle)
{
    int ret;
    if(conn_handle >= MAX_CONN_INSTANCES)
        return -1;
    gattc_service_t*p_info = gattc_get_service_info(conn_handle);
    if(p_info == NULL || p_info->start_handle == 0)
        return -1;
    ret = ble_gattc_disc_all_chrs(conn_handle,p_info->start_handle,p_info->end_handle,char_disc_cb,NULL);
    if(ret != 0) {
        ESP_LOGE(TAG, "Failed to discover characteristics; rc=%d\n", ret);
    }
    return ret;
}

// typedef int ble_gatt_disc_svc_fn(uint16_t conn_handle,
//                                  const struct ble_gatt_error *error,
//                                  const struct ble_gatt_svc *service,
//                                  void *arg);
static int svr_svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *arg)
{
    char buf[BLE_UUID_STR_LEN+1];
    
    if(conn_handle >= MAX_CONN_INSTANCES) {
        ESP_LOGE(TAG, "svr_svc_disc_cb: Invalid conn_handle %d", conn_handle);
        return BLE_HS_EINVAL;
    }
    
    gattc_service_t*p_info = gattc_get_service_info(conn_handle);
    if(p_info == NULL) {
        ESP_LOGE(TAG, "char_disc_cb: Invalid service info for conn_handle %d", conn_handle);
        return BLE_HS_EINVAL;
    }
    switch(error->status){
        case 0:
            ESP_LOGI(TAG, "Service discovered; conn_handle=%d start_handle=%d end_handle=%d uuid=%s",
                                conn_handle,service->start_handle,
                                service->end_handle,
                                ble_uuid_to_str((const ble_uuid_t *)&service->uuid,buf));
            p_info->start_handle = service->start_handle;
            p_info->end_handle = service->end_handle;
            break;

        case BLE_HS_EDONE:
            ESP_LOGI(TAG, "Service discovery complete; conn_handle=%d status=%d",conn_handle,error->status);
            if(p_info->start_handle == 0){
                ESP_LOGE(TAG, "Service not found");
                esp_ble_disconnect(conn_handle);
            }else{
                gattc_find_char(conn_handle);
                
            }
            break;

        default:
            ESP_LOGE(TAG, "Service discovery failed; status=%d conn_handle=%d",error->status,conn_handle);
            esp_ble_disconnect(conn_handle);
            break;
    }
    return 0;
}

static int gattc_find_service(uint16_t conn_handle)
{
    int ret;
    if(conn_handle >= MAX_CONN_INSTANCES)
        return -1;
    gattc_service_info_rst(conn_handle);
    ret = ble_gattc_disc_svc_by_uuid(conn_handle,(const ble_uuid_t *)&gatt_svr_svc_uuid,svr_svc_disc_cb,NULL);
    if(ret != 0) {
        ESP_LOGE(TAG, "Failed to discover services; rc=%d\n", ret);

    }
    return ret;
}

int esp_ble_connect(uint8_t* remote_bda, uint8_t remote_addr_type)
{
    ble_addr_t addr;
    const struct ble_gap_conn_params conn_params = {
        .scan_itvl = BLE_GAP_SCAN_ITVL_MS(80),
        .scan_window = BLE_GAP_SCAN_WIN_MS(80),
        .itvl_min = BLE_GAP_CONN_ITVL_MS(7.5),
        .itvl_max = BLE_GAP_CONN_ITVL_MS(30),
        .latency = 0,
        .supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(4000),
    };
    memset(&addr,0,sizeof(addr));
    mac_rever(addr.val,remote_bda);
    addr.type = remote_addr_type;

    m_ble_scan_need_recover = ble_gap_disc_active()? true : false;
    if(m_ble_scan_need_recover)
        esp_ble_scan_stop();

    ble_gap_conn_cancel();
    // int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr,
    //                 int32_t duration_ms,
    //                 const struct ble_gap_conn_params *params,
    //                 ble_gap_event_fn *cb, void *cb_arg);

    return ble_gap_connect(OWN_ADDR_TYPE,&addr,BLE_HS_FOREVER, &conn_params,ble_gap_event,NULL);
}
//=============================================================================================================================//
/*
send data
*/
//=============================================================================================================================//

// typedef int ble_gatt_attr_fn(uint16_t conn_handle,
//                              const struct ble_gatt_error *error,
//                              struct ble_gatt_attr *attr,
//                              void *arg);

static int gattc_write_cb(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr,
                             void *arg)
{
    ESP_LOGI(TAG, "gattc_write_cb; status=%d conn_handle=%d "
                      "attr_handle=%d\n",
                error->status, conn_handle, attr->handle);
    //tx finish
    return 0;
}

int esp_ble_write_data(uint16_t conn_id, uint16_t handle, uint8_t *p_data, uint16_t len, uint8_t write_type){
    int ret;
    if(handle == 0 || handle == 0xffff|| p_data == NULL || len == 0 || conn_id >= MAX_CONN_INSTANCES
        // || (write_type != ESP_GATT_WRITE_TYPE_NO_RSP && write_type != ESP_GATT_WRITE_TYPE_RSP)
       
    ){
         return -1;
    }

    if(len > m_mtu - 3){
        ESP_LOGE(TAG,"esp_ble_write_data:%d > m_mtu:%d - 3",len,m_mtu);
        return -1;
    }

    if(write_type == BLE_GATT_CHR_PROP_WRITE_NO_RSP)
        ret = ble_gattc_write_no_rsp_flat(conn_id, handle, p_data, len);
    else if(write_type == BLE_GATT_CHR_PROP_WRITE){
        ret = ble_gattc_write_flat(conn_id, handle, p_data, len, gattc_write_cb, NULL);
    }else{
        return -1;
    }
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_ble_notify_data failed: %d", ret);
        if(ret == BLE_HS_ENOMEM){
            return ESP_ERR_NO_MEM;
        }
    }
    return ret;
}

uint16_t esp_ble_get_mtu(uint16_t conn_id){
    if(conn_id >= MAX_CONN_INSTANCES)
        return 0;
    return m_mtu;
}

int esp_ble_notify_data(uint16_t conn_id, uint16_t handle, uint8_t *p_data, uint16_t len){
    int ret;
    if(handle == 0 || p_data == NULL || len == 0 || conn_id >= MAX_CONN_INSTANCES)
    {
        ESP_LOGE(TAG,"esp_ble_notify_data:invalid param");
        return -1;
    }

    if(len > m_mtu-3){
        ESP_LOGE(TAG,"esp_ble_write_data:len > p_dev->mtu_size-3");
        return -1;
    }

    if(!m_notify_en){
        ESP_LOGE(TAG,"data_ntf_en is 0");
        return -1;
    }

    struct os_mbuf *om;
    om = ble_hs_mbuf_from_flat(p_data, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "esp_ble_notify_data om alloc Error");
        return ESP_ERR_NO_MEM;
    }

    ret = ble_gatts_notify_custom(conn_id, handle, om);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_ble_notify_data failed: %d", ret);
        if(ret == BLE_HS_ENOMEM){
            return ESP_ERR_NO_MEM;
        }
    }else{
        ESP_LOG_BUFFER_HEX(TAG, p_data, len);
    }
    return ret;
}


//=============================================================================================================================//
/*
softdevice cb
*/
//=============================================================================================================================//

// static int gatt_mtu_cb(uint16_t conn_handle,
//                             const struct ble_gatt_error *error,
//                             uint16_t mtu, void *arg)
// {
//     ESP_LOGI(TAG, "ble_gatt_mtu_fn:conn_handle=%d,mtu=%d",conn_handle,mtu);
//     return 0;
// }

static adv_pk_t m_adv;
static inline void scan_info_rst(void){
    memset(&m_adv,0,sizeof(m_adv));
}

static inline void send_scan_data(adv_pk_t*p_adv){
    for(int i=0;i<SCAN_CB_MAX;i++){
        if(m_scan_callback[i] != NULL){
            m_scan_callback[i](p_adv);
        }
    }
}
static struct ble_gap_conn_desc dev_desc;
static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    int ret;
    

    // Safety check for event pointer
    if (event == NULL) {
        ESP_LOGE(TAG, "ble_gap_event: event is NULL");
        return -1;
    }

    memset(&dev_desc,0,sizeof(dev_desc));
    if(BLE_GAP_EVENT_DISC != event->type && BLE_GAP_EVENT_NOTIFY_TX != event->type){
        ESP_LOGI(TAG, "gap event id:%d",event->type);
    }
    
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        // ESP_LOGI(TAG, "BLE_GAP_EVENT_DISC:%d",event->disc.event_type);
        if(event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP){
            m_scan_test.rsp_cnts++;
            if(m_adv.addr_type == event->disc.addr.type
                && memcmp(m_adv.mac,event->disc.addr.val,6) == 0){
                // Check buffer overflow for scan response data
                if(m_adv.adv_len + event->disc.length_data <= sizeof(m_adv.data)) {
                    m_adv.rsp_len = event->disc.length_data;
                    memcpy(&m_adv.data[m_adv.adv_len],event->disc.data,event->disc.length_data);
                } else {
                    ESP_LOGE(TAG, "Scan response data overflow: adv_len=%d + rsp_len=%d > max=%d", 
                             m_adv.adv_len, event->disc.length_data, sizeof(m_adv.data));
                    m_adv.rsp_len = 0; // Discard scan response data if overflow
                }
            }
            if(m_adv.adv_len > 0){
                send_scan_data(&m_adv);
            }
            memset(&m_adv,0,sizeof(m_adv));
        }else if(event->disc.event_type < BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP){
            m_scan_test.adv_cnts++;
            if(m_adv.adv_len > 0){
                send_scan_data(&m_adv);
                memset(&m_adv,0,sizeof(m_adv));
            }
            m_adv.addr_type = event->disc.addr.type;
            mac_rever(m_adv.mac,event->disc.addr.val);
            // memcpy(m_adv.mac,event->disc.addr.val,6);

            // Check buffer overflow for advertisement data
            if(event->disc.length_data <= sizeof(m_adv.data)) {
                m_adv.adv_len = event->disc.length_data;
                memcpy(m_adv.data,event->disc.data,event->disc.length_data);
            } else {
                ESP_LOGE(TAG, "Advertisement data overflow: length=%d > max=%d", 
                         event->disc.length_data, sizeof(m_adv.data));
                m_adv.adv_len = 0; // Discard advertisement data if overflow
            }

            m_adv.rssi = event->disc.rssi;
        }else{
            ESP_LOGE(TAG,"invalid event type:%d",event->disc.event_type);
        }      
    break;

    case BLE_GAP_EVENT_DISC_COMPLETE:

        ESP_LOGI(TAG,"BLE_GAP_EVENT_DISC_COMPLETE:%d,%d,%d",event->disc_complete.reason,m_scan_test.adv_cnts,m_scan_test.rsp_cnts);
        memset(&m_scan_test,0,sizeof(m_scan_test));
        // if(m_adv.adv_len > 0){
        //     send_scan_data(&m_adv);
        // }
        memset(&m_adv,0,sizeof(m_adv));
        send_scan_data(NULL);
    break;

    //=====================================================================================================
    case BLE_GAP_EVENT_CONNECT:
        ret = ble_gap_conn_find(event->connect.conn_handle,&dev_desc);
        if(ret){
            ESP_LOGE(TAG,"ble_gap_conn_find:%d",ret);
        }
        ESP_LOGI(TAG,"BLE_GAP_EVENT_CONNECT:%d,%d",event->connect.status,event->connect.conn_handle);
        
        ret = ble_gap_set_data_len(event->connect.conn_handle,251,2120);
        if(ret){
            ESP_LOGE(TAG,"ble_gap_set_data_len:%d,%d",event->connect.conn_handle,ret);
        }

        ret = ble_att_set_preferred_mtu(BLE_MTU_MAX);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to set preferred MTU; rc = %d", ret);
        }
        
        // 通知应用层连接事件
        if (g_ble_event_callback != NULL) {
            ble_evt_t evt;
            evt.evt_id = BLE_EVT_CONNECTED;
            evt.params.connected.conn_id = event->connect.conn_handle;
            evt.params.connected.role = dev_desc.role;
            // 这里需要获取远程设备地址，暂时置0
            memset(evt.params.connected.remote_bda, 0, 6);
            evt.params.connected.remote_addr_type = 0;
            g_ble_event_callback(&evt);
        }
    break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG,"BLE_GAP_EVENT_DISCONNECT:%x,%d",event->disconnect.reason,event->disconnect.conn.conn_handle);
        
        // 通知应用层断开连接事件
        if (g_ble_event_callback != NULL) {
            ble_evt_t evt;
            evt.evt_id = BLE_EVT_DISCONNECTED;
            evt.params.disconnected.conn_id = event->disconnect.conn.conn_handle;
            memset(evt.params.disconnected.remote_bda, 0, 6);
            evt.params.disconnected.remote_addr_type = 0;
            g_ble_event_callback(&evt);
        }
        
        if(event->disconnect.conn.role == BLE_GAP_ROLE_SLAVE){
            adv_start();
        }
    break;

    case BLE_GAP_EVENT_LINK_ESTAB:
        ret = ble_gap_conn_find(event->link_estab.conn_handle,&dev_desc);
        if(ret){
            ESP_LOGE(TAG,"ble_gap_conn_find:%d",ret);
        }

        if(event->link_estab.status != 0){
            ESP_LOGE(TAG,"BLE_GAP_EVENT_LINK_ESTAB:%d",event->link_estab.status);
            // return 0;
        }

        ESP_LOGI(TAG,"BLE_GAP_EVENT_LINK_ESTAB:%d,%d, dev is %s",
            event->link_estab.status,
            event->link_estab.conn_handle,
            dev_desc.role == BLE_GAP_ROLE_SLAVE ? "peripheral" : "central");

        ret = ble_gattc_exchange_mtu(event->link_estab.conn_handle, NULL, NULL);
        if (ret != 0) {
            ESP_LOGE(TAG, "ble_gattc_exchange_mtu failed: %d", ret);
        }

        if(dev_desc.role == BLE_GAP_ROLE_MASTER){
            gattc_find_service(event->link_estab.conn_handle);
        }
    break;
    
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG,"BLE_GAP_EVENT_MTU:%d,%d,%d",event->mtu.conn_handle,event->mtu.value,event->mtu.channel_id);
        ret = ble_gap_conn_find(event->mtu.conn_handle,&dev_desc);
        if(ret){
            ESP_LOGE(TAG,"ble_gap_conn_find:%d",ret);
        }else{
            m_mtu = event->mtu.value;
        }

    break;
    
    case BLE_GAP_EVENT_NOTIFY_TX:
        //ble_mngt_dev_tx_fininh_evt(event->notify_tx.conn_handle,event->notify_tx.status==0?true:false,false);
    break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        ESP_LOGI(TAG,"BLE_GAP_EVENT_NOTIFY_RX");
        if (event->notify_rx.om != NULL) {
            
        } else {
            ESP_LOGE(TAG, "conn_handle %d notify rx data is NULL",event->notify_rx.conn_handle);
        }
    break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe event; conn_handle=%d attr_handle=%d "
                "reason=%d prevn=%d curn=%d previ=%d curi=%d\n",
                event->subscribe.conn_handle,
                event->subscribe.attr_handle,
                event->subscribe.reason,
                event->subscribe.prev_notify,
                event->subscribe.cur_notify,
                event->subscribe.prev_indicate,
                event->subscribe.cur_indicate);
        if(event->subscribe.reason != BLE_GAP_SUBSCRIBE_REASON_TERM && gatt_svr_notify_chr_val_handle == event->subscribe.attr_handle){
            m_notify_en = event->subscribe.cur_notify;
        }else if(event->subscribe.reason == BLE_GAP_SUBSCRIBE_REASON_TERM){
            
        }
    break;

    case BLE_GAP_EVENT_DATA_LEN_CHG:
    
        ESP_LOGI(TAG,"BLE_GAP_EVENT_DATA_LEN_CHG:%d,%d,%d,%d,%d",
            event->data_len_chg.conn_handle,
            event->data_len_chg.max_tx_octets,
            event->data_len_chg.max_tx_time,
            event->data_len_chg.max_rx_octets,
            event->data_len_chg.max_rx_time
            );
    break;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        if (event->conn_update_req.peer_params != NULL) {
            ESP_LOGI(TAG,"BLE_GAP_EVENT_CONN_UPDATE_REQ:%d,%d,%d,%d,%d,%d,%d",
                event->conn_update_req.conn_handle,
                event->conn_update_req.peer_params->itvl_min,
                event->conn_update_req.peer_params->itvl_max,
                event->conn_update_req.peer_params->latency,
                event->conn_update_req.peer_params->supervision_timeout,
                event->conn_update_req.peer_params->min_ce_len,
                event->conn_update_req.peer_params->max_ce_len);
            
            // Accept the peer's connection parameters by copying them to self_params
            if (event->conn_update_req.self_params != NULL) {
                *event->conn_update_req.self_params = *event->conn_update_req.peer_params;
            }
        } else {
            ESP_LOGI(TAG,"BLE_GAP_EVENT_CONN_UPDATE_REQ:%d,peer_params=NULL",
                event->conn_update_req.conn_handle);
        }
        return 0; // Accept the connection parameter update request

    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
        if (event->conn_update_req.peer_params != NULL) {
            ESP_LOGI(TAG,"BLE_GAP_EVENT_L2CAP_UPDATE_REQ:%d,%d,%d,%d,%d,%d,%d",
                event->conn_update_req.conn_handle,
                event->conn_update_req.peer_params->itvl_min,
                event->conn_update_req.peer_params->itvl_max,
                event->conn_update_req.peer_params->latency,
                event->conn_update_req.peer_params->supervision_timeout,
                event->conn_update_req.peer_params->min_ce_len,
                event->conn_update_req.peer_params->max_ce_len);
            
            // Accept the peer's connection parameters by copying them to self_params
            if (event->conn_update_req.self_params != NULL) {
                *event->conn_update_req.self_params = *event->conn_update_req.peer_params;
            }
        } else {
            ESP_LOGI(TAG,"BLE_GAP_EVENT_L2CAP_UPDATE_REQ:%d,peer_params=NULL",
                event->conn_update_req.conn_handle);
        }
        return 0; // Accept the L2CAP connection parameter update request

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG,"BLE_GAP_EVENT_CONN_UPDATE:%d,%d",event->conn_update.status,event->conn_update.conn_handle);
    break;
    
    default:
        break;
    }

    return 0;
}

int esp_ble_gap_set_advname(char* p_name){
    if(p_name == NULL){
        ESP_LOGE(TAG,"esp_ble_gap_set_advname:p_name is NULL");
        return -1;
    }
    int rc = ble_svc_gap_device_name_set(p_name);
    ESP_LOGI(TAG, "ble_svc_gap_device_name_set:%d",rc);
    return rc;
}



int esp_ble_gap_get_mac(uint8_t *p_mac){
    if(p_mac == NULL){
        ESP_LOGE(TAG,"esp_ble_gap_get_mac:p_mac is NULL");
        return -1;
    }
    int out_is_nrpa=0;
    int ret = ble_hs_id_copy_addr(BLE_ADDR_RANDOM,p_mac, &out_is_nrpa);
    if(ret){
        return ret;
    }
    mac_rever(p_mac,p_mac);
    ESP_LOGI(TAG, "get mac:%02x%02x%02x%02x%02x%02x,%d",
    p_mac[0],p_mac[1],p_mac[2],p_mac[3],p_mac[4],p_mac[5],out_is_nrpa);
    return 0;
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "Resetting state; reason=%d\n", reason);
}

static void ble_on_sync(void)
{
    uint8_t mac[6];
    int ret = esp_read_mac(mac, ESP_MAC_BT);

     /*
     * To improve both throughput and stability, it is recommended to set the connection interval
     * as an integer multiple of the `MINIMUM_CONN_INTERVAL`. This `MINIMUM_CONN_INTERVAL` should
     * be calculated based on the total number of connections and the Transmitter/Receiver phy.
     *
     * Note that the `MINIMUM_CONN_INTERVAL` value should meet the condition that:
     *      MINIMUM_CONN_INTERVAL > ((MAX_TIME_OF_PDU * 2) + 150us) * CONN_NUM.
     *
     * For example, if we have 10 connections, maxmum TX/RX length is 251 and the phy is 1M, then
     * the `MINIMUM_CONN_INTERVAL` should be greater than ((261 * 8us) * 2 + 150us) * 10 = 43260us.
     *
     */
    // ret = ble_gap_common_factor_set(true, (BLE_PREF_CONN_ITVL_MS * 1000) / 625);
    // assert(ret == 0);

    mac[5] |= 0xc0;
    ret = ble_hs_id_set_rnd(mac);
    if(ret){
        ESP_LOGE(TAG, "ble_hs_id_set_rnd failed: %d,%02x%02x%02x%02x%02x%02x\n", ret,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    /* Make sure we have proper identity address set (public preferred) */
    // rc = ble_hs_util_ensure_addr(1);
    // assert(rc == 0);


    m_ble_sync_flag = true;

    ESP_LOGI(TAG, "ble_on_sync");
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

int esp_ble_disconnect(uint16_t conn_id){
    return ble_gap_terminate(conn_id, BLE_ERR_REM_USER_CONN_TERM);
}

int esp_ble_init(ble_evt_callback_t callback)
{
    esp_err_t ret;

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init nimble %d ", ret);
        return ret;
    }

    g_ble_event_callback = callback;

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = gatts_svr_register_cb;
    
    gatts_init();

    nimble_port_freertos_init(ble_host_task);
    esp_log_level_set(TAG, USR_ESP_BLE_LOG_LEVEL);
    return 0;
}
//=============================================================================================================================//
/*
adv and scan
*/
//=============================================================================================================================//
int esp_ble_adv_set_data(uint8_t *adv_data, uint8_t adv_data_len, uint8_t *scan_rsp_data, uint8_t scan_rsp_data_len)
{
    esp_err_t status;

    if (adv_data == NULL || adv_data_len == 0) {
        ESP_LOGE(TAG, "Invalid advertising data");
        return ESP_ERR_INVALID_ARG;
    }
    
    status = ble_gap_adv_set_data(adv_data, adv_data_len);
    if (status != ESP_OK) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data failed: %s", esp_err_to_name(status));
        return status;
    }

    if (scan_rsp_data && scan_rsp_data_len > 0) {
        status = ble_gap_adv_rsp_set_data(scan_rsp_data, scan_rsp_data_len);
        if (status != ESP_OK) {
            ESP_LOGE(TAG, "ble_gap_adv_rsp_set_data failed: %s", esp_err_to_name(status));
            return status;
        }
    }

    return 0;
}   

int esp_ble_adv_stop(void)
{
    return ble_gap_adv_stop();
}

static int adv_start(void){
    int ret = ble_gap_adv_start(OWN_ADDR_TYPE, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (ret != 0) {
        MODLOG_DFLT(ERROR, "error enabling advertisement; ret=%d\n", ret);
    }
    
    return ret;
}

int esp_ble_adv_start(uint16_t adv_interval_ms)
{
    // int ret;
    if(adv_interval_ms < 20 || adv_interval_ms > 10240) {
        ESP_LOGE(TAG, "Invalid advertising interval: %d ms", adv_interval_ms);
        return ESP_ERR_INVALID_ARG;
    }

    
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(adv_interval_ms); // Convert ms to 0.625ms units
    adv_params.itvl_max = adv_params.itvl_min; // Set min and max to the same value for fixed interval
    
    return adv_start();
}

int esp_ble_scan_cb_register(ble_scan_callback_t callback)
{
    if(callback == NULL){
        return ESP_ERR_INVALID_ARG; 
    }
    for(int i=0;i<SCAN_CB_MAX;i++){
        if(m_scan_callback[i] == NULL){
            m_scan_callback[i] = callback;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}


int esp_ble_scan_cb_unregister(ble_scan_callback_t callback)
{
    if(callback == NULL){
        return ESP_ERR_INVALID_ARG; 
    }
    for(int i=0;i<SCAN_CB_MAX;i++){
        if(m_scan_callback[i] == callback){
            m_scan_callback[i] = NULL;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

int esp_ble_scan_start(uint16_t scan_interval_ms, uint16_t scan_window_ms, uint16_t duration_s,bool active_scan){

    if(scan_interval_ms < 20 || scan_interval_ms > 10240) {
        ESP_LOGE(TAG, "Invalid scan interval: %d ms", scan_interval_ms);
        return ESP_ERR_INVALID_ARG;
    }
    if(scan_window_ms < 20 || scan_window_ms > 10240) {
        ESP_LOGE(TAG, "Invalid scan window: %d ms", scan_window_ms);
        return ESP_ERR_INVALID_ARG;
    }
    if(scan_window_ms > scan_interval_ms){
        ESP_LOGE(TAG, "scan window %d ms > scan interval %d ms", scan_window_ms, scan_interval_ms);
        return ESP_ERR_INVALID_ARG;
    }

    if(duration_s > 180) {
        ESP_LOGE(TAG, "Invalid scan duration: %d s", duration_s);
        return ESP_ERR_INVALID_ARG;
    }

    struct ble_gap_disc_params disc_params;
    int ret;
    /* Tell the controller to filter duplicates; we don't want to process
     * repeated advertisements from the same device.
     */
    disc_params.filter_duplicates = 0;

    /**
     * Perform a passive scan.  I.e., don't send follow-up scan requests to
     * each advertiser.
     */
    disc_params.passive = active_scan?0:1;

    /* Use defaults for the rest of the parameters. */
    disc_params.itvl = BLE_GAP_SCAN_ITVL_MS(scan_interval_ms);
    disc_params.window = BLE_GAP_SCAN_WIN_MS(scan_window_ms);
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    ret = ble_gap_disc(OWN_ADDR_TYPE, duration_s==0?BLE_HS_FOREVER:duration_s*1000, &disc_params,
                      ble_gap_event, NULL);
    if (ret != 0) {
        MODLOG_DFLT(ERROR, "Error initiating GAP discovery procedure; rc=%d\n",
                    ret);
    }
    return ret;
}

int esp_ble_scan_stop(void)
{
    //esp_err_t status;

    // status = esp_ble_gap_stop_scanning();
    // if (status != ESP_OK) {
    //     ESP_LOGE(TAG, "esp_ble_gap_stop_scanning failed: %s", esp_err_to_name(status));
    //     return status;
    // }
    int ret = ble_gap_disc_cancel();
    if(ret != 0) {
        ESP_LOGE(TAG, "ble_gap_disc_cancel failed: %d", ret);
        return ret;
    }
    scan_info_rst();

    return 0;
}
