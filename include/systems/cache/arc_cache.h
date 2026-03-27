// Copyright 2025 cpp-from-scratch authors
//
// ARCCache — 自适应替换缓存 (Adaptive Replacement Cache)。
// 结合 LRU 和 LFU 的优势，通过 GhostList 追踪淘汰历史，
// 动态调整两个分区的容量比例，实现比单纯 LRU 或 LFU 更好的命中率。
//
// 核心机制：
//   1. 缓存分为 LRU 分区和 LFU 分区，初始各占一半容量。
//   2. 两个 GhostList 分别记录最近从 LRU 和 LFU 淘汰的键。
//   3. 新插入时若键在 lru_ghost 中命中 → 扩大 LRU 分区；
//      若在 lfu_ghost 中命中 → 扩大 LFU 分区。
//   4. LRU 中访问次数 >= kTransformThreshold 的热点条目自动迁移到 LFU 分区。

#pragma once

#include "systems/cache/cache.h"
#include "systems/cache/lfu_cache.h"
#include "systems/cache/lru_cache.h"

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace cfs::cache {

// 幽灵链表 (GhostList)，ARC 算法专用。
// 使用 FIFO 策略记录最近被淘汰的键。当某个键被再次请求时，
// 如果在 GhostList 中命中，说明存在"二次访问"趋势，
// ARC 据此动态调整 LRU/LFU 分区大小。
//
// 内部使用 std::list + std::unordered_map 实现 O(1) 增删查。
//
// Template Parameters:
//   Key - 键类型
template <typename Key>
class GhostList {
private:
    std::list<Key> ghost_list_;        // FIFO 链表，存储被淘汰的键
    // 键 → 链表迭代器的映射，提供 O(1) 查找与删除
    std::unordered_map<Key, typename std::list<Key>::iterator> ghost_map_;
    std::size_t capacity_;              // 幽灵链表最大容量

public:
    // 构造幽灵链表。
    //
    // Parameters:
    //   capacity - 容量，必须 > 0，否则抛出 std::invalid_argument
    explicit GhostList(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("GhostList capacity must be greater than 0");
        }
    }

    ~GhostList() { clear(); }

    // 添加键到幽灵链表尾部。若键已存在则先移除再添加；
    // 若超出容量则淘汰头部最旧的条目。
    void add(const Key& key) {
        if (contains(key)) remove(key);
        if (ghost_list_.size() >= capacity_) {
            ghost_map_.erase(ghost_list_.front());
            ghost_list_.pop_front();
        }
        ghost_list_.push_back(key);
        ghost_map_[key] = std::prev(ghost_list_.end());
    }

    // 从幽灵链表中移除指定键。键不存在时静默返回。
    void remove(const Key& key) {
        auto it = ghost_map_.find(key);
        if (it != ghost_map_.end()) {
            ghost_list_.erase(it->second);
            ghost_map_.erase(it);
        }
    }

    // 判断键是否存在于幽灵链表中。
    bool contains(const Key& key) const {
        return ghost_map_.count(key) > 0;
    }

    // 清空幽灵链表。
    void clear() {
        ghost_list_.clear();
        ghost_map_.clear();
    }

    // 返回当前元素数量。
    std::size_t size() const { return ghost_list_.size(); }

    // 返回最大容量。
    std::size_t capacity() const { return capacity_; }
};

// 自适应替换缓存 (ARC)。内部维护 LRU 分区和 LFU 分区，
// 通过 GhostList 追踪淘汰历史来动态调整分区比例。
//
// 分区调整规则：
//   - 新键命中 lru_ghost → LRU 分区 +1（优先接收最近被淘汰的键）
//   - 新键命中 lfu_ghost → LFU 分区 +1（优先接收高频键）
//   - 分区调整时会重建底层缓存实例
//
// 热点迁移：
//   LRU 分区中访问次数 >= kTransformThreshold 的条目自动迁移到 LFU 分区。
//
// Thread Safety:
//   所有公开方法均为线程安全（内部持有 std::mutex）。
//
// Template Parameters:
//   Key   - 键类型，须支持 std::hash
//   Value - 值类型
template <typename Key, typename Value>
class ARCCache : public ICache<Key, Value> {
private:
    static constexpr std::size_t kTransformThreshold = 3;  // LRU→LFU 迁移阈值

    const std::size_t total_capacity_;     // 缓存总容量（构造后不可变）
    std::size_t partition_;                // LRU 分区大小，LFU = total_capacity_ - partition_

    LRUCache<Key, Value> lru_cache_;       // LRU 分区：缓存近期访问的条目
    KLfuCache<Key, Value> lfu_cache_;      // LFU 分区：缓存高频访问的条目
    GhostList<Key> lru_ghost_;             // LRU 幽灵链表：记录最近从 LRU 淘汰的键
    GhostList<Key> lfu_ghost_;             // LFU 幽灵链表：记录最近从 LFU 淘汰的键

    mutable std::mutex mutex_;             // 互斥锁，保证线程安全

public:
    // 构造 ARC 缓存。
    //
    // Parameters:
    //   total_capacity - 总容量，必须 >= 2（LRU 和 LFU 各至少占 1），
    //                    否则抛出 std::invalid_argument
    explicit ARCCache(std::size_t total_capacity)
        : total_capacity_(total_capacity)
        , partition_(total_capacity / 2)
        , lru_cache_(partition_)
        , lfu_cache_(total_capacity - partition_)
        , lru_ghost_(partition_)
        , lfu_ghost_(total_capacity - partition_) {
        if (total_capacity < 2) {
            throw std::invalid_argument("ARCCache capacity must be at least 2");
        }
    }

    ~ARCCache() override { clear(); }

    // 插入或更新键值对。按 ARC 策略处理 GhostList 命中和分区调整。
    void put(const Key& key, const Value& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        arc_core_put(key, value);
    }

    // 查找键对应的值。命中时按 ARC 策略处理 LRU→LFU 热点迁移。
    //
    // Returns:
    //   命中返回对应的值，未命中返回 std::nullopt。
    std::optional<Value> get(const Key& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return arc_core_get(key);
    }

    // 判断键是否存在于任意分区中（LRU 或 LFU）。
    bool contains(const Key& key) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return lru_cache_.contains(key) || lfu_cache_.contains(key);
    }

    // 返回两个分区的元素总数。
    std::size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return lru_cache_.size() + lfu_cache_.size();
    }

    // 返回缓存总容量。
    std::size_t capacity() const override { return total_capacity_; }

    // 清空所有分区和幽灵链表。
    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        lru_cache_.clear();
        lfu_cache_.clear();
        lru_ghost_.clear();
        lfu_ghost_.clear();
    }

private:
    // ARC 插入核心逻辑，优先级依次为：
    //   1. 键在 LRU 中 → 更新值，检查是否满足迁移条件
    //   2. 键在 LFU 中 → 更新值
    //   3. 键在 lru_ghost 中 → 扩大 LRU 分区，移除幽灵条目，插入 LRU
    //   4. 键在 lfu_ghost 中 → 扩大 LFU 分区，移除幽灵条目，插入 LRU
    //   5. 否则 → 直接插入 LRU
    void arc_core_put(const Key& key, const Value& value) {
        if (lru_cache_.contains(key)) {
            lru_cache_.put(key, value);
            if (auto cnt = lru_cache_.getAccessCount(key);
                cnt && *cnt >= kTransformThreshold) {
                lru_to_lfu_move(key, value);
            }
            return;
        }
        if (lfu_cache_.contains(key)) {
            lfu_cache_.put(key, value);
            return;
        }
        if (lru_ghost_.contains(key)) {
            adjust_partition(true);
            lru_ghost_.remove(key);
            add_to_lru_safe(key, value);
            return;
        }
        if (lfu_ghost_.contains(key)) {
            adjust_partition(false);
            lfu_ghost_.remove(key);
            add_to_lru_safe(key, value);
            return;
        }
        add_to_lru_safe(key, value);
    }

    // ARC 查找核心逻辑：
    //   1. 键在 LRU 中 → 返回值并检查热点迁移
    //   2. 键在 LFU 中 → 返回值
    //   3. 未命中 → 返回 std::nullopt
    std::optional<Value> arc_core_get(const Key& key) {
        if (auto res = lru_cache_.get(key); res) {
            if (auto cnt = lru_cache_.getAccessCount(key);
                cnt && *cnt >= kTransformThreshold) {
                lru_to_lfu_move(key, *res);
            }
            return res;
        }
        if (auto res = lfu_cache_.get(key); res) {
            return res;
        }
        return std::nullopt;
    }

    // 安全地向 LRU 分区添加键值对。若 LRU 已满则循环淘汰最久节点，
    // 将被淘汰的键加入 lru_ghost_，直到有空间为止。
    void add_to_lru_safe(const Key& key, const Value& value) {
        while (lru_cache_.size() >= lru_cache_.capacity()) {
            if (auto evict_key = lru_cache_.evictAndGetKey(); evict_key) {
                lru_ghost_.add(*evict_key);
            } else {
                break;
            }
        }
        lru_cache_.put(key, value);
    }

    // 将键从 LRU 分区迁移到 LFU 分区（热点提升）。
    void lru_to_lfu_move(const Key& key, const Value& value) {
        lru_cache_.remove(key);
        lfu_cache_.put(key, value);
    }

    // 调整 LRU/LFU 分区大小。调整后会重建所有底层缓存和幽灵链表。
    //
    // Parameters:
    //   is_expand_lru - true 时 LRU 分区 +1，false 时 LRU 分区 -1
    void adjust_partition(bool is_expand_lru) {
        if (is_expand_lru) {
            if (partition_ >= total_capacity_ - 1) return;
            partition_++;
        } else {
            if (partition_ <= 1) return;
            partition_--;
        }
        lru_cache_ = LRUCache<Key, Value>(partition_);
        lfu_cache_ = KLfuCache<Key, Value>(total_capacity_ - partition_);
        lru_ghost_ = GhostList<Key>(partition_);
        lfu_ghost_ = GhostList<Key>(total_capacity_ - partition_);
    }
};

}  // namespace cfs::cache
