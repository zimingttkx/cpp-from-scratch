#pragma once

#include "systems/cache/cache.h"
#include <limits>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cstddef>
#include <optional>

namespace cfs::cache {

template<typename Key, typename Value>
class KLfuCache;

template<typename Key, typename Value>
class FreqList {
private:
    struct Node {
        std::size_t freq_;
        Key key_;
        Value value_;
        std::weak_ptr<Node> prev_;
        std::shared_ptr<Node> next_;
        Node(const Key& key, const Value& value) : freq_(1), key_(key), value_(value), next_(nullptr) {}
        Node() : freq_(0), next_(nullptr) {}
    };

    using NodePtr = std::shared_ptr<Node>;
    std::size_t freq_;
    NodePtr dummy_head_;
    NodePtr dummy_tail_;

public:
    explicit FreqList(std::size_t freq) : freq_(freq) {
        dummy_head_ = std::make_shared<Node>();
        dummy_tail_ = std::make_shared<Node>();
        dummy_head_->next_ = dummy_tail_;
        dummy_tail_->prev_ = dummy_head_;
    }

    bool isEmpty() const {
        return dummy_head_->next_ == dummy_tail_;
    }

    void insertNode(NodePtr node) {
        if (!node || !dummy_head_ || !dummy_tail_) return;
        node->prev_ = dummy_tail_->prev_;
        node->next_ = dummy_tail_;
        dummy_tail_->prev_.lock()->next_ = node;
        dummy_tail_->prev_ = node;
    }

    void removeNode(NodePtr node) {
        if (!node || !dummy_head_ || !dummy_tail_) return;
        if (node->prev_.expired()) return;
        auto prev = node->prev_.lock();
        auto next = node->next_;
        if (prev) prev->next_ = next;
        if (next) next->prev_ = prev;
        node->next_ = nullptr;
        node->prev_.reset();
    }

    NodePtr getFirstNode() const {
        return dummy_head_->next_;
    }

    friend class KLfuCache<Key, Value>;
};

template<typename Key, typename Value>
class KLfuCache : public ICache<Key, Value> {
    using Node = typename FreqList<Key, Value>::Node;
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

private:
    std::size_t capacity_;
    std::size_t min_freq_;
    std::size_t max_average_num_;
    std::size_t cur_average_num_;
    std::size_t cur_total_num_;
    mutable std::mutex mutex_;
    NodeMap node_map_;
    std::unordered_map<std::size_t, std::shared_ptr<FreqList<Key, Value>>> freq_to_freq_list_;

public:
    explicit KLfuCache(std::size_t capacity, std::size_t max_average_num = 10)
        : capacity_(capacity)
        , min_freq_(std::numeric_limits<std::size_t>::max())
        , max_average_num_(max_average_num)
        , cur_average_num_(0)
        , cur_total_num_(0) {}

    ~KLfuCache() override { clear(); }

    void put(const Key& key, const Value& value) override {
        if (capacity_ == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) {
            it->second->value_ = value;
            update_node_freq(it->second);
            return;
        }
        if (node_map_.size() >= capacity_) {
            evict_least_freq();
        }
        NodePtr new_node = std::make_shared<Node>(key, value);
        insert_node_to_freq_list(new_node);
        node_map_[key] = new_node;
        min_freq_ = std::min(min_freq_, static_cast<std::size_t>(1));
        add_freq_num();
    }

    std::optional<Value> get(const Key& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) {
            update_node_freq(it->second);
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
        freq_to_freq_list_.clear();
        min_freq_ = std::numeric_limits<std::size_t>::max();
        cur_total_num_ = 0;
        cur_average_num_ = 0;
    }

private:
    void update_node_freq(NodePtr node) {
        std::size_t old_freq = node->freq_;
        if (!freq_to_freq_list_.count(old_freq)) return;
        freq_to_freq_list_[old_freq]->removeNode(node);
        node->freq_++;
        insert_node_to_freq_list(node);
        if (old_freq == min_freq_ && freq_to_freq_list_.count(old_freq) && freq_to_freq_list_[old_freq]->isEmpty()) {
            min_freq_++;
        }
        add_freq_num();
    }

    void insert_node_to_freq_list(NodePtr node) {
        if (!node) return;
        std::size_t freq = node->freq_;
        if (freq_to_freq_list_.find(freq) == freq_to_freq_list_.end()) {
            freq_to_freq_list_[freq] = std::make_shared<FreqList<Key, Value>>(freq);
        }
        freq_to_freq_list_[freq]->insertNode(node);
    }

    void evict_least_freq() {
        if (!freq_to_freq_list_.count(min_freq_)) return;
        NodePtr least_freq_node = freq_to_freq_list_[min_freq_]->getFirstNode();
        if (least_freq_node != freq_to_freq_list_[min_freq_]->dummy_tail_) {
            freq_to_freq_list_[min_freq_]->removeNode(least_freq_node);
            node_map_.erase(least_freq_node->key_);
            decrease_freq_num(least_freq_node->freq_);
        }
    }

    void add_freq_num() {
        cur_total_num_++;
        cur_average_num_ = node_map_.empty() ? 0 : (cur_total_num_ / node_map_.size());
        if (cur_average_num_ > max_average_num_) {
            handle_over_max_average_num();
        }
    }

    void decrease_freq_num(std::size_t num) {
        cur_total_num_ = (cur_total_num_ >= num) ? cur_total_num_ - num : 0;
        cur_average_num_ = node_map_.empty() ? 0 : (cur_total_num_ / node_map_.size());
    }

    void handle_over_max_average_num() {
        if (node_map_.empty()) return;
        for (auto& pair : node_map_) {
            NodePtr node = pair.second;
            if (!node) continue;
            if (!freq_to_freq_list_.count(node->freq_)) continue;
            freq_to_freq_list_[node->freq_]->removeNode(node);
            node->freq_ = (node->freq_ > max_average_num_ / 2) ? node->freq_ - max_average_num_ / 2 : 1;
            insert_node_to_freq_list(node);
        }
        update_min_freq();
    }

    void update_min_freq() {
        min_freq_ = std::numeric_limits<std::size_t>::max();
        for (const auto& pair : freq_to_freq_list_) {
            if (pair.second && !pair.second->isEmpty()) {
                min_freq_ = std::min(min_freq_, pair.first);
            }
        }
        if (min_freq_ == std::numeric_limits<std::size_t>::max() && !node_map_.empty()) {
            min_freq_ = 1;
        }
    }
};

} // namespace cfs::cache
