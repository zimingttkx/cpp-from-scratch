# HighSpeedCacheSystem

> 基于 C++20 的轻量级 Header-only 缓存库。

小而精，零依赖，线程安全。

## 特性

- **Header-only** — 引入头文件即可使用，无需链接
- **泛型模板** — 支持任意键值类型
- **线程安全** — `std::mutex` 保护，可直接用于多线程场景
- **O(1) 操作** — 哈希表 + 双向链表实现

## 快速上手

```cpp
#include "hs_cache/lru_cache.h"
#include <iostream>

int main() {
    hs_cache::LRUCache<std::string, std::string> cache(1000);

    cache.put("key", "value");
    if (auto val = cache.get("key")) {
        std::cout << val.value() << std::endl;  // "value"
    }

    return 0;
}
```

## 构建

需要 C++20 编译器及 CMake 3.31+。

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

## 开发进度

| 模块 | 状态 |
|---|---|
| `ICache` 接口 | 已完成 |
| `LRUCache` | 已完成 |
| `LFUCache` | 已完成 |
| `LFUCache` | 已完成 |
| `ARCCache` | 已完成 |
| 分片缓存 | 计划中 |
| 布隆过滤器 | 计划中 |
| 单元测试 | 计划中 |

## 许可证

[MIT](LICENSE)
