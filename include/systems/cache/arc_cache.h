#pragma once

#include "systems/cache/cache.h"
#include "systems/cache/lru_cache.h"
#include "systems/cache/lfu_cache.h"
#include <list>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <cstddef>

namespace cfs::cache {

// GhostList：FIFO淘汰策略，固定容量，O(1)增删/查找，ARC专用
template<typename Key>
class GhostList {
private:
    std::list<Key> ghost_list_;
    std::unordered_map<Key, typename std::list<Key>::iterator> ghost_map_;
    std::size_t capacity_;

public:
    explicit GhostList(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("GhostList capacity must be greater than 0");
        }
    }

    ~GhostList() { clear(); }

    void add(const Key& key) {
        if (contains(key)) remove(key);
        if (ghost_list_.size() >= capacity_) {
            ghost_map_.erase(ghost_list_.front());
            ghost_list_.pop_front();
        }
        ghost_list_.push_back(key);
        ghost_map_[key] = std::prev(ghost_list_.end());
    }

    void remove(const Key& key) {
        auto it = ghost_map_.find(key);
        if (it != ghost_map_.end()) {
            ghost_list_.erase(it->second);
            ghost_map_.erase(it);
        }
    }

    bool contains(const Key& key) const {
        return ghost_map_.count(key) > 0;
    }

    void clear() {
        ghost_list_.clear();
        ghost_map_.clear();
    }

    std::size_t size() const { return ghost_list_.size(); }
    std::size_t capacity() const { return capacity_; }
};

// ARCCache：自适应替换缓存，LRU/LFU双分区 + GhostList动态调整
template<typename Key, typename Value>
class ARCCache : public ICache<Key, Value> {
private:
    static constexpr std::size_t kTransformThreshold = 3;
    const std::size_t total_capacity_;
    std::size_t partition_;

    LRUCache<Key, Value> lru_cache_;
    KLfuCache<Key, Value> lfu_cache_;
    GhostList<Key> lru_ghost_;
    GhostList<Key> lfu_ghost_;

    mutable std::mutex mutex_;

public:
    explicit ARCCache(std::size_t total_capacity)
        : total_capacity_(total_capacity)
        , partition_(total_capacity / 2)
        , lru_cache_(partition_)
        , lfu_cache_(total_capacity - partition_)
        , lru_ghost_(partition_)
        , lfu_ghost_(total_capacity - partition_)
    {
        if (total_capacity < 2) {
            throw std::invalid_argument("ARCCache capacity must be at least 2");
        }
    }

    ~ARCCache() override { clear(); }

    void put(const Key& key, const Value& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        arc_core_put(key, value);
    }

    std::optional<Value> get(const Key& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return arc_core_get(key);
    }

    bool contains(const Key& key) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return lru_cache_.contains(key) || lfu_cache_.contains(key);
    }

    std::size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return lru_cache_.size() + lfu_cache_.size();
    }

    std::size_t capacity() const override { return total_capacity_; }

    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        lru_cache_.clear();
        lfu_cache_.clear();
        lru_ghost_.clear();
        lfu_ghost_.clear();
    }

private:
    void arc_core_put(const Key& key, const Value& value) {
        if (lru_cache_.contains(key)) {
            lru_cache_.put(key, value);
            if (auto cnt = lru_cache_.getAccessCount(key); cnt && *cnt >= kTransformThreshold) {
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

    std::optional<Value> arc_core_get(const Key& key) {
        if (auto res = lru_cache_.get(key); res) {
            if (auto cnt = lru_cache_.getAccessCount(key); cnt && *cnt >= kTransformThreshold) {
                lru_to_lfu_move(key, *res);
            }
            return res;
        }
        if (auto res = lfu_cache_.get(key); res) {
            return res;
        }
        return std::nullopt;
    }

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

    void lru_to_lfu_move(const Key& key, const Value& value) {
        lru_cache_.remove(key);
        lfu_cache_.put(key, value);
    }

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

} // namespace cfs::cache
