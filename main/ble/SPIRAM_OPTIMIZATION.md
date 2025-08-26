# BLE WiFi配网 - SPIRAM静态任务优化

## 优化概述

将BLE数据处理任务改为使用`xTaskCreateStatic`并将任务栈分配在SPIRAM中，这样可以：
1. 减少内部RAM的使用
2. 提供更大的栈空间
3. 避免动态内存分配的不确定性

## 技术实现

### 1. 静态任务创建

#### 原始实现
```c
// 使用动态创建，栈分配在内部RAM
xTaskCreate(
    ble_data_process_task,
    "ble_data_proc", 
    4096,
    NULL,
    2,
    &g_ble_process_task
);
```

#### 优化后实现
```c
// 使用静态创建，栈分配在SPIRAM
g_ble_process_task = xTaskCreateStatic(
    ble_data_process_task,
    "ble_data_proc",
    4096,
    NULL,
    2,
    g_ble_process_task_stack,      // SPIRAM栈指针
    &g_ble_process_task_buffer     // 静态任务控制块
);
```

### 2. SPIRAM栈分配

```c
// 分配4KB栈空间到SPIRAM
g_ble_process_task_stack = (StackType_t*)heap_caps_malloc(
    4096 * sizeof(StackType_t), 
    MALLOC_CAP_SPIRAM
);
```

### 3. 新增变量

```c
static StaticTask_t g_ble_process_task_buffer;    // 静态任务控制块
static StackType_t* g_ble_process_task_stack;     // SPIRAM栈指针
```

## 内存使用对比

### 优化前
- **内部RAM**: ~4KB (任务栈) + ~84字节 (任务控制块)
- **SPIRAM**: 0字节
- **总计**: ~4.1KB内部RAM

### 优化后  
- **内部RAM**: ~84字节 (任务控制块)
- **SPIRAM**: ~4KB (任务栈)
- **总计**: ~84字节内部RAM + 4KB SPIRAM

### 节省效果
- **节省内部RAM**: ~4KB
- **SPIRAM使用**: +4KB (通常SPIRAM容量更大)

## 优势分析

### 1. 内存优化
- **释放内部RAM**: 将大的栈空间移到SPIRAM
- **提高可用性**: 内部RAM可用于其他关键组件
- **扩展性好**: SPIRAM容量通常比内部RAM大得多

### 2. 性能考虑
- **栈访问**: SPIRAM访问稍慢，但对于BLE处理任务影响很小
- **确定性**: 静态分配避免运行时内存碎片
- **稳定性**: 减少内存不足导致的任务创建失败

### 3. 系统稳定性
- **预分配**: 启动时一次性分配，避免运行时分配失败
- **隔离性**: 任务栈独立分配，不会影响其他组件
- **可控性**: 明确的内存使用模式，便于调试

## 错误处理

### 1. SPIRAM分配失败
```c
if (g_ble_process_task_stack == NULL) {
    ESP_LOGE(TAG, "Failed to allocate SPIRAM stack for BLE task");
    // 清理已分配资源
    return -1;
}
```

### 2. 任务创建失败
```c
if (g_ble_process_task == NULL) {
    ESP_LOGE(TAG, "Failed to create BLE data process task");
    // 释放SPIRAM栈内存
    heap_caps_free(g_ble_process_task_stack);
    return -1;
}
```

### 3. 清理时的资源释放
```c
// 等待任务结束
// 释放SPIRAM栈内存
if (g_ble_process_task_stack) {
    heap_caps_free(g_ble_process_task_stack);
    g_ble_process_task_stack = NULL;
}
```

## 配置要求

### 1. Kconfig配置
确保启用SPIRAM支持：
```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
```

### 2. 内存分配策略
```
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
```

## 使用注意事项

### 1. SPIRAM可用性
- 确保硬件支持SPIRAM
- 检查SPIRAM初始化是否成功
- 监控SPIRAM使用情况

### 2. 性能影响
- SPIRAM访问比内部RAM稍慢
- 对于BLE数据处理这种非实时任务影响很小
- 可以通过监控任务执行时间来验证

### 3. 调试支持
- 可以通过`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`监控SPIRAM使用
- 任务栈使用情况可以通过`uxTaskGetStackHighWaterMark()`检查

## 验证方法

### 1. 内存使用监控
```c
ESP_LOGI(TAG, "SPIRAM free: %d bytes", 
         heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
ESP_LOGI(TAG, "Internal RAM free: %d bytes", 
         heap_caps_get_free_size(MALLOC_CAP_8BIT));
```

### 2. 栈使用检查
```c
UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(g_ble_process_task);
ESP_LOGI(TAG, "BLE task stack high water mark: %d", stack_high_water);
```

这个优化在保持功能不变的前提下，显著提高了内存使用效率，特别适合内部RAM资源紧张的ESP32应用场景。
