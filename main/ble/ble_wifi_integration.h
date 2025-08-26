#ifndef BLE_WIFI_INTEGRATION_H
#define BLE_WIFI_INTEGRATION_H

namespace BleWifiIntegration {

/**
 * @brief 启动蓝牙WiFi配网功能
 * @return true 启动成功, false 启动失败
 */
bool StartBleWifiConfig();

/**
 * @brief 停止蓝牙WiFi配网功能
 */
void StopBleWifiConfig();

/**
 * @brief 检查蓝牙配网是否活跃
 * @return true 活跃, false 不活跃
 */
bool IsBleWifiConfigActive();

} // namespace BleWifiIntegration

#endif // BLE_WIFI_INTEGRATION_H
