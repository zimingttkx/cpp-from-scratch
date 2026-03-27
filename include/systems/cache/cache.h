// Copyright 2025 cpp-from-scratch authors
//
// ICache — 缓存抽象接口，所有缓存实现（LRU、LFU、ARC）均继承此接口。
// 定义了 put/get/contains/size/capacity/clear 六个基础操作。

#pragma once

#include <cstddef>
#include <optional>

namespace cfs::cache {

// 缓存抽象接口。所有缓存实现必须继承此类并实现其纯虚函数。
//
// Template Parameters:
//   Key   - 键类型
//   Value - 值类型
template <typename Key, typename Value>
class ICache {
public:
    virtual ~ICache() = default;

    // 插入或更新键值对。若键已存在则更新其值，否则插入新条目。
    // 当缓存已满时，由具体实现决定淘汰策略。
    virtual void put(const Key& key, const Value& value) = 0;

    // 查找键对应的值。命中时可选地更新该条目的访问状态（由子类决定）。
    //
    // Returns:
    //   命中时返回对应的值，未命中返回 std::nullopt。
    virtual std::optional<Value> get(const Key& key) = 0;

    // 判断键是否存在于缓存中。不修改任何访问状态。
    virtual bool contains(const Key& key) const = 0;

    // 返回当前缓存中的元素数量。
    virtual std::size_t size() const = 0;

    // 返回缓存的最大容量。
    virtual std::size_t capacity() const = 0;

    // 清空缓存，移除所有键值对并重置内部状态。
    virtual void clear() = 0;
};

}  // namespace cfs::cache
