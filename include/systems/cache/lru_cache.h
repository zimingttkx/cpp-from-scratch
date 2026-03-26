#pragma once

#include "systems/cache/cache.h"
#include <memory>
#include <unordered_map>
#include <mutex>

namespace cfs::cache {

template<typename Key, typename Value>
class LRUCache : public ICache<Key, Value> {
private:
    struct Node {
        Key key_;
        Value value_;
        size_t accessCount_;
        std::shared_ptr<Node> next_;
        std::weak_ptr<Node> prev_;
        Node(const Key& key, const Value& value) : key_(key), value_(value), accessCount_(1) {}
        Node() : accessCount_(0) {}
    };
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    std::size_t capacity_;
    NodeMap node_map_;
    mutable std::mutex mutex_;
    NodePtr dummy_head_;
    NodePtr dummy_tail_;

public:
    explicit LRUCache(std::size_t capacity) : capacity_(capacity) {
        dummy_head_ = std::make_shared<Node>();
        dummy_tail_ = std::make_shared<Node>();
        dummy_head_->next_ = dummy_tail_;
        dummy_tail_->prev_ = dummy_head_;
    }

    ~LRUCache() override { clear(); }

    void put(const Key& key, const Value& value) override {
        if (capacity_ == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) {
            it->second->value_ = value;
            it->second->accessCount_++;
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

    std::optional<Value> get(const Key& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) {
            it->second->accessCount_++;
            moveToMostRecent(it->second);
            return it->second->value_;
        }
        return std::nullopt;
    }

    bool contains(const Key& key) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return node_map_.find(key) != node_map_.end();
    }

    std::size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return node_map_.size();
    }

    std::size_t capacity() const override { return capacity_; }

    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        node_map_.clear();
        dummy_head_->next_ = dummy_tail_;
        dummy_tail_->prev_ = dummy_head_;
    }

    // ARC 扩展接口
    std::optional<size_t> getAccessCount(const Key& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) return it->second->accessCount_;
        return std::nullopt;
    }

    std::optional<Key> evictAndGetKey() {
        std::lock_guard<std::mutex> lock(mutex_);
        NodePtr least_recent = dummy_head_->next_;
        if (least_recent == dummy_tail_) return std::nullopt;
        Key evict_key = least_recent->key_;
        removeNode(least_recent);
        node_map_.erase(evict_key);
        return evict_key;
    }

    void remove(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) {
            removeNode(it->second);
            node_map_.erase(it);
        }
    }

private:
    void moveToMostRecent(NodePtr node) {
        removeNode(node);
        insertNode(node);
    }

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

    void insertNode(NodePtr node) {
        auto tail_prev = dummy_tail_->prev_.lock();
        node->next_ = dummy_tail_;
        node->prev_ = tail_prev;
        tail_prev->next_ = node;
        dummy_tail_->prev_ = node;
    }

    void evictLeastRecent() {
        NodePtr least_recent = dummy_head_->next_;
        if (least_recent != dummy_tail_) {
            removeNode(least_recent);
            node_map_.erase(least_recent->key_);
        }
    }
};

} // namespace cfs::cache
