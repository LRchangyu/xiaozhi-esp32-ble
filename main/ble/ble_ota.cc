#include "ble_ota.h"
#include "esp_log.h"

static const char* TAG = "BleOta";

// 静态实例指针
BleOta* BleOta::instance_ = nullptr;

BleOta& BleOta::GetInstance() {
    if (instance_ == nullptr) {
        static BleOta instance;
        instance_ = &instance;
    }
    return *instance_;
}

bool BleOta::Initialize() {
    if (initialized_) {
        ESP_LOGW(TAG, "BLE OTA already initialized");
        return true;
    }
    
    esp_err_t err = ble_ota_init(StaticProgressCallback);
    if (err == ESP_OK) {
        initialized_ = true;
        ESP_LOGI(TAG, "BLE OTA initialized successfully");
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to initialize BLE OTA: %s", esp_err_to_name(err));
        return false;
    }
}

void BleOta::Deinitialize() {
    if (!initialized_) {
        return;
    }
    
    esp_err_t err = ble_ota_deinit();
    if (err == ESP_OK) {
        initialized_ = false;
        progress_callback_ = nullptr;
        complete_callback_ = nullptr;
        ESP_LOGI(TAG, "BLE OTA deinitialized successfully");
    } else {
        ESP_LOGE(TAG, "Failed to deinitialize BLE OTA: %s", esp_err_to_name(err));
    }
}

void BleOta::SetProgressCallback(std::function<void(int)> callback) {
    progress_callback_ = callback;
}

void BleOta::SetCompleteCallback(std::function<void(bool)> callback) {
    complete_callback_ = callback;
}

ble_ota_state_t BleOta::GetState() const {
    return ble_ota_get_state();
}

void BleOta::ResetState() {
    ble_ota_reset_state();
}

void BleOta::StaticProgressCallback(int progress, const char* message) {
    if (instance_ && instance_->progress_callback_) {
        instance_->progress_callback_(progress);
    }
    
    // 检查是否完成（100%）或失败
    if (progress == 100 && instance_ && instance_->complete_callback_) {
        instance_->complete_callback_(true);
    } else if (progress < 0 && instance_ && instance_->complete_callback_) {
        // 负值表示错误
        instance_->complete_callback_(false);
    }
}
