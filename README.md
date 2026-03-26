# cpp-from-scratch

> 从零手写 C++ 底层组件的开源仓库。

用 C++20 复现各类系统级数据结构与组件，包括缓存策略、容器、垃圾回收器、内存池等。Header-only，零依赖，线程安全。

## 项目结构

```
include/
├── containers/        # 基础容器：红黑树、B树、跳表、哈希表...
├── algorithms/        # 算法：排序、搜索...
└── systems/           # 系统级组件
    └── cache/         # 缓存策略（LRU / LFU / ARC）
```

命名空间统一为 `cfs::<模块>`，各模块互不耦合。

## 已实现

### `systems/cache` — 缓存策略

| 组件 | 说明 |
|---|---|
| `ICache<K,V>` | 统一缓存抽象接口 |
| `LRUCache<K,V>` | 最近最少使用，哈希表 + 双向链表，O(1) |
| `KLfuCache<K,V>` | 最低频次淘汰，支持频次衰减（K-LFU），解决缓存老化 |
| `ARCCache<K,V>` | 自适应替换缓存，LRU/LFU 双分区 + GhostList 动态调整 |

```cpp
#include "systems/cache/lru_cache.h"

cfs::cache::LRUCache<std::string, std::string> cache(1000);
cache.put("key", "value");
if (auto val = cache.get("key")) {
    std::cout << val.value() << std::endl;
}
```

## 路线图

| 模块 | 类别 | 状态 |
|---|---|---|
| LRU / LFU / ARC 缓存 | `systems/cache` | 已完成 |
| 红黑树 | `containers` | 计划中 |
| B 树 | `containers` | 计划中 |
| 跳表 | `containers` | 计划中 |
| 内存池 | `systems` | 计划中 |
| 线程池 | `systems` | 计划中 |
| 垃圾回收器 | `systems` | 计划中 |
| 布隆过滤器 | `systems` | 计划中 |
| 协程调度器 | `systems` | 计划中 |
| Mini Redis | `systems` | 计划中 |

## 构建

需要 C++20 编译器及 CMake 3.31+。

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

## 许可证

[MIT](LICENSE)
