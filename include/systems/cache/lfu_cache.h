// Copyright 2025 cpp-from-scratch authors
//
// KLfuCache — 带频次衰减的最低频次淘汰 (K-LFU) 缓存。
// 经典 LFU 的扩展变体：维护 min_freq 实现O(1)淘汰，并引入全局频次
// 衰减机制防止高频条目长期占用缓存空间。
//
// 频次衰减机制：
//   每次 put/get 操作递增全局频率总和。当平均频次超过阈值 max_average_num
//   时，对所有节点执行频率减半（不低于 1），使其重新参与公平竞争。

#pragma once

#include "systems/cache/cache.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace cfs::cache {

template <typename Key, typename Value>
class KLfuCache;

// 频率桶链表。将相同访问频率的节点组织在同一条双向链表中。
// 每个频率值对应一个 FreqList 实例，内部使用 dummy_head/dummy_tail
// 哨兵节点保持 O(1) 的插入和删除操作。
//
// Template Parameters:
//   Key   - 键类型
//   Value - 值类型
template <typename Key, typename Value>
class FreqList {
private:
    // 频率桶内的链表节点。
    struct Node {
        std::size_t freq_;              // 该节点当前访问频率
        Key key_;                       // 键
        Value value_;                   // 值
        std::weak_ptr<Node> prev_;      // 前驱节点（弱引用，避免循环引用）
        std::shared_ptr<Node> next_;    // 后继节点（强引用）

        Node(const Key& key, const Value& value)
            : freq_(1), key_(key), value_(value), next_(nullptr) {}
        Node() : freq_(0), next_(nullptr) {}
    };

    using NodePtr = std::shared_ptr<Node>;

    std::size_t freq_;                  // 此桶对应的频率值
    NodePtr dummy_head_;                // 头哨兵，dummy_head_->next_ 为桶中第一个有效节点
    NodePtr dummy_tail_;                // 尾哨兵

public:
    // 构造指定频率值的空桶。
    explicit FreqList(std::size_t freq) : freq_(freq) {
        dummy_head_ = std::make_shared<Node>();
        dummy_tail_ = std::make_shared<Node>();
        dummy_head_->next_ = dummy_tail_;
        dummy_tail_->prev_ = dummy_head_;
    }

    // 判断此频率桶是否为空（无有效节点）。
    bool isEmpty() const {
        return dummy_head_->next_ == dummy_tail_;
    }

    // 在桶尾部插入节点（尾插法，新节点成为此桶中"最旧"的位置）。
    void insertNode(NodePtr node) {
        if (!node || !dummy_head_ || !dummy_tail_) return;
        node->prev_ = dummy_tail_->prev_;
        node->next_ = dummy_tail_;
        dummy_tail_->prev_.lock()->next_ = node;
        dummy_tail_->prev_ = node;
    }

    // 从桶中摘除指定节点，不释放内存。
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

    // 获取桶中第一个有效节点（用于淘汰最低频率的最旧条目）。
    // 空桶时返回 dummy_tail_，调用方应进行判等检查。
    NodePtr getFirstNode() const {
        return dummy_head_->next_;
    }

    friend class KLfuCache<Key, Value>;
};

// 带频次衰减的 K-LFU 缓存。在经典 LFU 基础上增加频次衰减机制：
//   - 维护 min_freq_ 追踪当前最低频率，保证 O(1) 淘汰。
//   - 当全局平均频次超过 max_average_num_ 时，对所有节点频率减半，
//     防止历史高频条目长期垄断缓存空间（解决"缓存老化"问题）。
//
// Thread Safety:
//   所有公开方法均为线程安全（内部持有 std::mutex）。
//
// Template Parameters:
//   Key   - 键类型，须支持 std::hash
//   Value - 值类型
template <typename Key, typename Value>
class KLfuCache : public ICache<Key, Value> {
    using Node = typename FreqList<Key, Value>::Node;
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

private:
    std::size_t capacity_;               // 缓存最大容量
    std::size_t min_freq_;               // 当前缓存中最低访问频率，用于 O(1) 淘汰
    std::size_t max_average_num_;        // 频次衰减阈值（平均频次超过此值时触发减半）

    std::size_t cur_average_num_;        // 当前全局平均频次 = cur_total_num_ / node_map_.size()
    std::size_t cur_total_num_;          // 所有节点频率之和，用于计算平均频次

    mutable std::mutex mutex_;           // 互斥锁，保证线程安全
    NodeMap node_map_;                   // 键 → 节点的哈希映射，提供 O(1) 查找
    // 频率 → 对应频率桶链表的映射。每个频率值对应一个 FreqList 实例。
    std::unordered_map<std::size_t, std::shared_ptr<FreqList<Key, Value>>> freq_to_freq_list_;

public:
    // 构造 K-LFU 缓存。
    //
    // Parameters:
    //   capacity        - 最大容量
    //   max_average_num - 频次衰减阈值，默认 10。
    //                      当平均频次超过此值时对所有节点执行频率减半
    explicit KLfuCache(std::size_t capacity, std::size_t max_average_num = 10)
        : capacity_(capacity)
        , min_freq_(std::numeric_limits<std::size_t>::max())
        , max_average_num_(max_average_num)
        , cur_average_num_(0)
        , cur_total_num_(0) {}

    ~KLfuCache() override { clear(); }

    // 插入或更新键值对。已存在的键会更新值并递增频率；新键在缓存已满时
    // 淘汰 min_freq_ 桶中最旧的节点。
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

    // 查找键对应的值。命中时递增节点频率并更新全局频次统计。
    //
    // Returns:
    //   命中返回对应的值，未命中返回 std::nullopt。
    std::optional<Value> get(const Key& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if (it != node_map_.end()) {
            update_node_freq(it->second);
            return it->second->value_;
        }
        return std::nullopt;
    }

    // 判断键是否存在于缓存中（不影响频率和访问状态）。
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

    // 清空缓存，移除所有键值对并重置频率桶、min_freq 和频次统计。
    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        node_map_.clear();
        freq_to_freq_list_.clear();
        min_freq_ = std::numeric_limits<std::size_t>::max();
        cur_total_num_ = 0;
        cur_average_num_ = 0;
    }

private:
    // 将节点从旧频率桶移出，频率 +1 后插入新频率桶。
    // 若旧桶变空且旧频率等于 min_freq_，则 min_freq_ 递增以保持正确性。
    void update_node_freq(NodePtr node) {
        std::size_t old_freq = node->freq_;
        if (!freq_to_freq_list_.count(old_freq)) return;
        freq_to_freq_list_[old_freq]->removeNode(node);
        node->freq_++;
        insert_node_to_freq_list(node);
        if (old_freq == min_freq_ && freq_to_freq_list_.count(old_freq) &&
            freq_to_freq_list_[old_freq]->isEmpty()) {
            min_freq_++;
        }
        add_freq_num();
    }

    // 将节点插入到其当前频率对应的桶中，桶不存在则自动创建。
    void insert_node_to_freq_list(NodePtr node) {
        if (!node) return;
        std::size_t freq = node->freq_;
        if (freq_to_freq_list_.find(freq) == freq_to_freq_list_.end()) {
            freq_to_freq_list_[freq] = std::make_shared<FreqList<Key, Value>>(freq);
        }
        freq_to_freq_list_[freq]->insertNode(node);
    }

    // 淘汰 min_freq_ 桶中的第一个（最旧的）节点，并更新频次统计。
    void evict_least_freq() {
        if (!freq_to_freq_list_.count(min_freq_)) return;
        NodePtr least_freq_node = freq_to_freq_list_[min_freq_]->getFirstNode();
        if (least_freq_node != freq_to_freq_list_[min_freq_]->dummy_tail_) {
            freq_to_freq_list_[min_freq_]->removeNode(least_freq_node);
            node_map_.erase(least_freq_node->key_);
            decrease_freq_num(least_freq_node->freq_);
        }
    }

    // 递增全局频率总和并重算平均值。若平均值超过阈值则触发频次衰减。
    void add_freq_num() {
        cur_total_num_++;
        cur_average_num_ = node_map_.empty() ? 0 : (cur_total_num_ / node_map_.size());
        if (cur_average_num_ > max_average_num_) {
            handle_over_max_average_num();
        }
    }

    // 淘汰节点后减少全局频率总和。
    void decrease_freq_num(std::size_t num) {
        cur_total_num_ = (cur_total_num_ >= num) ? cur_total_num_ - num : 0;
        cur_average_num_ = node_map_.empty() ? 0 : (cur_total_num_ / node_map_.size());
    }

    // 频次衰减处理：将所有节点的频率减去 max_average_num_ / 2（不低于 1），
    // 然后重新插入对应频率桶，最后更新 min_freq_。
    void handle_over_max_average_num() {
        if (node_map_.empty()) return;
        for (auto& pair : node_map_) {
            NodePtr node = pair.second;
            if (!node) continue;
            if (!freq_to_freq_list_.count(node->freq_)) continue;
            freq_to_freq_list_[node->freq_]->removeNode(node);
            node->freq_ = (node->freq_ > max_average_num_ / 2)
                              ? node->freq_ - max_average_num_ / 2
                              : 1;
            insert_node_to_freq_list(node);
        }
        update_min_freq();
    }

    // 遍历所有非空频率桶，找到最小的频率值并更新 min_freq_。
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

}  // namespace cfs::cache
