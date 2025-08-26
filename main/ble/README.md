# 蓝牙WiFi配网功能

## 概述

本功能为xiaozhi-esp32项目实现了基于蓝牙低功耗(BLE)的WiFi配网功能，符合项目中定义的蓝牙配网协议。用户可以通过BLE连接到设备，发送指定格式的命令来配置WiFi网络参数。

## 功能特性

- **协议兼容性**：完全符合`蓝牙配网协议.md`中定义的通信协议
- **广播名称**：使用`lr_wificfg-`前缀加上AP名称作为广播名称
- **服务定义**：实现了指定的BLE服务(UUID: 0xFDD0)和特性(UUID: 0xFDD1)
- **三大功能**：
  - 获取当前WiFi配置 (命令0x00)
  - 修改WiFi配置 (命令0x01)  
  - 获取WiFi扫描列表 (命令0x02)

## 文件结构

```
main/ble/
├── ble_wifi_config.h              # 蓝牙配网功能接口定义
├── ble_wifi_config.cc             # 蓝牙配网功能实现
├── ble_wifi_config_example.cc     # 使用示例
├── ble_wifi_integration.cc        # 项目集成示例
├── esp_ble.h                      # 原有BLE接口
└── esp_ble.c                      # 原有BLE实现(已修改)
```

## 协议格式

### 通用数据包格式
```
| Header(2字节) | 命令(1字节) | 载荷(n字节) |
| 0x58 0x5A    | 0x00-0xFF  | 数据      |
```

### 支持的命令

#### 1. 获取当前WiFi配置 (0x00)

**请求：**
```
58 5A 00
```

**响应：**
```
58 5A 00 [SSID长度] [SSID] [密码长度] [密码]
```

#### 2. 修改WiFi配置 (0x01)

**请求：**
```
58 5A 01 [SSID长度] [SSID] [密码长度] [密码]
```

**响应：**
```
58 5A 01 [状态码]
```
- 状态码：0=成功，其他=失败

#### 3. 获取WiFi扫描列表 (0x02)

**请求：**
```
58 5A 02
```

**响应：**
```
58 5A 02 [AP数量] [第1个AP长度] [第1个AP名称] ...
58 5A 02 00  // 结束标记
```

## 使用方法

### 1. 基本初始化

```cpp
#include "ble_wifi_config.h"

// 获取实例
auto& ble_wifi_config = BleWifiConfig::GetInstance();

// 初始化
if (!ble_wifi_config.Initialize()) {
    ESP_LOGE(TAG, "Failed to initialize BLE WiFi config");
    return;
}

// 设置WiFi配置改变回调
ble_wifi_config.SetOnWifiConfigChanged([](const std::string& ssid, const std::string& password) {
    ESP_LOGI(TAG, "New WiFi config: %s", ssid.c_str());
    // 处理新的WiFi配置
});

// 启动广播
std::string ap_ssid = "my-device";
if (!ble_wifi_config.StartAdvertising(ap_ssid)) {
    ESP_LOGE(TAG, "Failed to start advertising");
    return;
}
```

### 2. 与现有项目集成

参考`ble_wifi_integration.cc`中的集成示例：

```cpp
// 在Application::Start()中添加
#include "ble_wifi_integration.cc"

void Application::Start() {
    // ... 现有初始化代码 ...
    
    // 启动蓝牙WiFi配网
    if (!BleWifiIntegration::StartBleWifiConfig()) {
        ESP_LOGE(TAG, "Failed to start BLE WiFi config");
    }
    
    // ... 其他初始化代码 ...
}
```

### 3. C接口使用

```c
#include "ble_wifi_config.h"

// 初始化
if (ble_wifi_config_init() != 0) {
    ESP_LOGE(TAG, "Failed to init BLE WiFi config");
    return;
}

// 启动广播
const char* ap_ssid = "my-device";
if (ble_wifi_config_start_advertising(ap_ssid) != 0) {
    ESP_LOGE(TAG, "Failed to start advertising");
    return;
}
```

## 技术实现细节

### 1. BLE服务架构

- **服务UUID**: 0xFDD0 (16位) / 0000FDD0-0000-1000-8000-00805f9b34fb (128位)
- **特性UUID**: 0xFDD1 (16位) / 0000FDD1-0000-1000-8000-00805f9b34fb (128位)
- **特性属性**: Write + Notify

### 2. 数据流程

1. 客户端连接到BLE设备
2. 客户端向Write特性写入命令数据
3. 设备解析协议数据包
4. 设备处理命令（获取/设置WiFi配置，扫描WiFi）
5. 设备通过Notify特性发送响应数据

### 3. WiFi功能集成

- **SSID管理**: 使用`SsidManager`类管理保存的WiFi配置
- **扫描功能**: 调用ESP-IDF的`esp_wifi_scan_start()`进行WiFi扫描
- **连接功能**: 与`WifiConfigurationAp`类集成，支持WiFi连接

### 4. 广播名称生成

广播名称格式：`lr_wificfg-{AP_SSID}`

其中AP_SSID通过`WifiConfigurationAp::GetSsid()`获取，该函数基于设备MAC地址生成唯一标识。

## 编译配置

### 1. 修改CMakeLists.txt

在`main/CMakeLists.txt`的SOURCES中添加：
```cmake
"ble/ble_wifi_config.cc"
```

### 2. 依赖组件

确保以下组件已启用：
- `bt` (蓝牙功能)
- `esp_wifi` (WiFi功能)
- `cjson` (JSON解析)
- `78__esp-wifi-connect` (WiFi管理)

### 3. Kconfig配置

建议在`main/Kconfig.projbuild`中添加：
```kconfig
config ENABLE_BLE_WIFI_CONFIG
    bool "Enable BLE WiFi Configuration"
    default y
    help
      Enable Bluetooth Low Energy WiFi configuration service.
```

## 错误处理

### 常见错误及解决方案

1. **初始化失败**
   - 检查蓝牙是否已启用
   - 确认NimBLE栈正常工作

2. **广播启动失败**
   - 检查广播名称长度是否超限
   - 确认BLE控制器状态正常

3. **数据接收失败**
   - 验证协议格式是否正确
   - 检查数据长度是否超过MTU限制

4. **WiFi扫描失败**
   - 确认WiFi初始化正常
   - 检查WiFi模式设置

## 安全考虑

1. **连接限制**: 当前实现允许任何BLE客户端连接，建议添加配对或PIN码验证
2. **数据加密**: 当前WiFi密码以明文传输，建议添加加密机制
3. **访问控制**: 建议添加操作权限验证机制

## 性能优化

1. **MTU优化**: 自动协商更大的MTU以提高传输效率
2. **连接参数**: 优化连接间隔以平衡功耗和响应速度
3. **扫描优化**: 实现增量扫描避免重复扫描

## 测试验证

### 1. 功能测试

使用BLE调试工具（如nRF Connect）进行测试：

1. 搜索并连接到设备（广播名以`lr_wificfg-`开头）
2. 发现服务0xFDD0和特性0xFDD1
3. 发送测试命令验证各功能

### 2. 集成测试

1. 验证与现有WiFi配网功能的兼容性
2. 测试WiFi配置保存和加载
3. 验证网络连接功能

## 版本历史

- **v1.0.0**: 初始实现，支持基本的BLE WiFi配网功能
- 完全符合项目蓝牙配网协议规范
- 与现有WiFi管理组件无缝集成

## 贡献

欢迎提交Issue和Pull Request来改进这个功能。

## 许可证

遵循项目的开源许可证。
