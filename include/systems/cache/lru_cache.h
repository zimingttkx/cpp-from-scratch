// Copyright 2025 cpp-from-scratch authors
//
// LRUCache — 最近最少使用 (Least Recently Used) 缓存。
// 基于双向链表 + 哈希表实现，put/get/evict 均为 O(1)。
// 额外维护 access_count 用于 ARC 场景下 LRU→LFU 热点迁移判定。
// 内部使用 dummy_head / dummy_tail 哨兵节点简化链表操作。
// 所有公开方法通过 mutex 保证线程安全。

#pragma once

#include "systems/cache/cache.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace cfs::cache {

// 最近最少使用缓存。淘汰顺序由访问时间决定，最近被访问的条目
// 位于链表尾部，最久未被访问的位于链表头部。
//
// Thread Safety:
//   所有公开方法均为线程安全（内部持有 std::mutex）。
//
// Template Parameters:
//   Key   - 键类型，须支持 std::hash
//   Value - 值类型
template <typename Key, typename Value>
class LRUCache : public ICache<Key, Value> {
private:
    // 双向链表节点。使用 shared_ptr/weak_ptr 组合避免循环引用。
    struct Node {
        Key key_;                        // 节点键
        Value value_;                    // 节点值
        size_t access_count_;            // 累计访问次数，用于 ARC 热点判定
        std::shared_ptr<Node> next_;     // 后继节点（强引用）
        std::weak_ptr<Node> prev_;       // 前驱节点（弱引用）

        Node(const Key& key, const Value& value)
            : key_(key), value_(value), access_count_(1) {}
        Node() : access_count_(0) {}
    };

    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    std::size_t capacity_;               // 缓存最大容量
    NodeMap node_map_;                   // 键 → 节点的哈希映射，提供 O(1) 查找
    mutable std::mutex mutex_;           // 互斥锁，保护所有公开操作
    NodePtr dummy_head_;                 // 头哨兵，dummy_head_->next_ 为最久未使用节点
    NodePtr dummy_tail_;                 // 尾哨兵，dummy_tail_->prev_ 为最近使用节点

public:
    // 构造 LRU 缓存。
    //
    // Parameters:
    //   capacity - 最大容量，设为 0 时所有 put 操作将被忽略
    explicit LRUCache(std::size_t capacity) : capacity_(capacity) {
        dummy_head_ = std::make_shared<Node>();
        dummy_tail_ = std::make_shared<Node>();
        dummy_head_->next_ = dummy_tail_;
        dummy_tail_->prev_ = dummy_head_;
    }

    ~LRUCache() override { clear(); }

    // 插入或更新键值对。已存在的键会更新值、递增访问计数并移至最近端；
    // 新键在缓存已满时先淘汰最久未使用的节点。
    void put(const Key& key, const Value& value) override {
        if (capacity_ == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) {
            it->second->value_ = value;
            it->second->access_count_++;
            moveToMostRecent(it->second);
            return;
        }
        if (node_map_.size() >= capacity_) {
            evictLeastRecent();
        }
        NodePtr new_node = std::make_shared<Node>(key, value);
        insertNode(new_node);
        node_map_[key] = new_node;
    }

    // 查找键对应的值。命中时递增访问计数并将节点移至最近端。
    //
    // Returns:
    //   命中返回对应的值，未命中返回 std::nullopt。
    std::optional<Value> get(const Key& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) {
            it->second->access_count_++;
            moveToMostRecent(it->second);
            return it->second->value_;
        }
        return std::nullopt;
    }

    // 判断键是否存在于缓存中（不影响访问计数和顺序）。
    bool contains(const Key& key) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return node_map_.find(key) != node_map_.end();
    }

    // 返回当前缓存中的元素数量。
    std::size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return node_map_.size();
    }

    // 返回缓存最大容量。
    std::size_t capacity() const override { return capacity_; }

    // 清空缓存中所有键值对，重置链表为空（仅保留哨兵）。
    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        node_map_.clear();
        dummy_head_->next_ = dummy_tail_;
        dummy_tail_->prev_ = dummy_head_;
    }

    // ===== ARC 扩展接口 =====
    // 以下方法仅供 ARCCache 内部调用，不属于通用 ICache 接口。

    // 获取指定键的累计访问次数。
    //
    // Returns:
    //   访问次数，键不存在时返回 std::nullopt。
    std::optional<size_t> getAccessCount(const Key& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) return it->second->access_count_;
        return std::nullopt;
    }

    // 淘汰最久未使用的节点并返回其键，但不加入任何 GhostList。
    // 用于 ARCCache 手动将淘汰键转入幽灵链表。
    //
    // Returns:
    //   被淘汰的键，缓存为空时返回 std::nullopt。
    std::optional<Key> evictAndGetKey() {
        std::lock_guard<std::mutex> lock(mutex_);
        NodePtr least_recent = dummy_head_->next_;
        if (least_recent == dummy_tail_) return std::nullopt;
        Key evict_key = least_recent->key_;
        removeNode(least_recent);
        node_map_.erase(evict_key);
        return evict_key;
    }

    // 直接移除指定键（不触发幽灵链表操作）。
    // 用于 ARCCache 将节点从 LRU 分区迁移到 LFU 分区。
    void remove(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) {
            removeNode(it->second);
            node_map_.erase(it);
        }
    }

private:
    // 将节点从当前位置摘除并重新插入到链表尾部（最近使用端）。
    void moveToMostRecent(NodePtr node) {
        removeNode(node);
        insertNode(node);
    }

    // 从双向链表中摘除节点，不释放内存也不删除哈希映射。
    void removeNode(NodePtr node) {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            auto next = node->next_;
            prev->next_ = next;
            next->prev_ = prev;
            node->next_ = nullptr;
            node->prev_.reset();
        }
    }

    // 将节点插入到链表尾部（dummy_tail_ 之前），使其成为最近使用节点。
    void insertNode(NodePtr node) {
        auto tail_prev = dummy_tail_->prev_.lock();
        node->next_ = dummy_tail_;
        node->prev_ = tail_prev;
        tail_prev->next_ = node;
        dummy_tail_->prev_ = node;
    }

    // 淘汰链表头部的节点（最久未使用端，即 dummy_head_->next_）。
    void evictLeastRecent() {
        NodePtr least_recent = dummy_head_->next_;
        if (least_recent != dummy_tail_) {
            removeNode(least_recent);
            node_map_.erase(least_recent->key_);
        }
    }
};

}  // namespace cfs::cache
