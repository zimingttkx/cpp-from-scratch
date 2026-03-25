#pragma once

#include "hs_cache/cache.h"
#include <memory> // 智能指针头文件
#include <unordered_map> // 哈希表
#include <mutex> // cpp 互斥锁头文件 用于线程安全


namespace hs_cache{
template<typename Key, typename Value>
class LRUCache : public ICache<Key, Value>{
private:
    struct Node{
        Key key_;
        Value value_;
        size_t accessCount_; // 访问次数 
        // 使用next_指向后序节点 prev_指向前序节点
        std::shared_ptr<Node> next_;
        std::weak_ptr<Node> prev_;
        // 数据节点构造函数
        Node(const Key& key, const Value & value) : key_(key), value_(value),accessCount_(1){}
        // 虚拟首尾节点构造函数 不存储数据 只是作为哨兵节点 方便操作首尾节点
        Node() : accessCount_(0){}
    };
    // 适用类型别名
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    std::size_t capacity_; // 缓存容量
    NodeMap node_map_; // O(1)哈希表映射

    mutable std::mutex mutex_; // 在函数里面使用lock_guard
    
    // - dummy_head_：链表头（靠近它的节点是「最近最久未使用」的，优先淘汰）
    // - dummy_tail_：链表尾（靠近它的节点是「最近刚使用」的，是热点数据）
    NodePtr dummy_head_;
    NodePtr dummy_tail_;

public:
    // 构造函数: 初始化缓存容量 explicit关键字禁止隐式类型转化
    explicit LRUCache(std::size_t capacity) : capacity_(capacity){
        // 初始化虚拟链表骨架 头->尾
        dummy_head_ = std::make_shared<Node>(); // 创建虚拟头结点
        dummy_tail_ = std::make_shared<Node>(); // 创建虚拟尾结点
        dummy_head_->next_ = dummy_tail_; // 虚拟头的后继指向虚拟尾
        dummy_tail_->prev_ = dummy_head_; // 虚拟尾的前驱指向虚拟头
    }

    // 析构函数：override关键字表示重写父类ICache的虚析构函数
    // 作用：确保子类对象析构时能正确调用子类析构函数（多态析构）
    ~LRUCache() override{
        clear(); // 主动清理所有节点 避免链表过长导致析构栈溢出 同时断开智能指针引用链 释放内存
    }

    // 加锁：std::lock_guard是RAII风格的锁（构造时加锁，析构时自动解锁）
        // 作用：保证整个put操作是原子的，多线程同时调用put不会导致数据错乱
    void put(const Key& key, const Value& value) override{
        if(capacity_ == 0) return; // 容量为0无法存储任何数据
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if(it != node_map_.end()){
            // 说明找到了 更新值 标记为最近使用过
            it->second->value_ = value;
            it->second->accessCount_++; // 访问次数 + 1
            moveToMostRecent(it->second); // 将节点移动到链表末尾
            return;
        }
        if(node_map_.size() >= capacity_){
            evictLeastRecent(); // 没找到且缓存满了就删除链表头部的节点
        }
        NodePtr new_node = std::make_shared<Node>(key, value); // 创建新节点
        insertNode(new_node);
        node_map_[key] = new_node;
    }

    // std::optional：C++17特性，优雅处理"键不存在"的情况（替代返回null/特殊值）
    std::optional<Value> get(const Key& key) override{
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if(it != node_map_.end()){
            it->second->accessCount_++; // 访问次数加1
            moveToMostRecent(it->second); // 移动到尾部
            return it->second->value_; // 返回节点的值
        }
        // key不存在
        return std::nullopt;
    }

    // 检查当前缓存是否含有某个键 仅仅读取 可作为const
    bool contains(const Key& key) const override{
        std::lock_guard<std::mutex> lock(mutex_);
        return node_map_.find(key) != node_map_.end();
    }

    std::size_t size() const override{
        std::lock_guard<std::mutex> lock(mutex_);
        return node_map_.size();
    }

    std::size_t capacity() const override{
        return capacity_;
    }

    void clear() override{
        std::lock_guard<std::mutex> lock(mutex_);
        node_map_.clear();
        dummy_head_->next_ = dummy_tail_;
        dummy_tail_->prev_ = dummy_head_;
    }

    // 添加三个接口提供给ARC

    // 获取指定Key的访问次数 ARC判断是否迁移到LFU核心
    std::optional<size_t> getAccessCount(const Key& key) const{
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if(it != node_map_.end()){
            return it->second->accessCount_;
        }
        return std::nullopt;
    }

    // 淘汰很久没有使用的节点并返回被淘汰的Key ARC将淘汰key加入到GhostList中
    std::optional<Key> evictAndGetKey(){
        std::lock_guard<std::mutex> lock(mutex_);
        NodePtr least_recent = dummy_head_->next_;
        if(least_recent == dummy_tail_){
            return std::nullopt;
        }
        Key evict_key = least_recent->key_;
        removeNode(least_recent);
        node_map_.erase(evict_key);
        return evict_key;
    }

    // 删除指定key的节点 ARC中LRU→LFU迁移时，从LRU移除数据的核心
    void remove(const Key& key){
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if(it != node_map_.end()){
            removeNode(it->second);
            node_map_.erase(it);
        }
    }

private:
    void moveToMostRecent(NodePtr node){
        removeNode(node); // 从当前位置离开链表
        insertNode(node); // 插入到末尾
    }
    void removeNode(NodePtr node){
        if(!node->prev_.expired() && node->next_){
            auto prev = node->prev_.lock(); // 从weakptr变成sharedptr 因为weak本身没有所有权
            auto next = node->next_;

            prev->next_ = next;
            next->prev_ = prev;

            node->next_ = nullptr;
            node->prev_.reset(); // weak_ptr重置
        }
    }

    void insertNode(NodePtr node){
        auto tail_prev = dummy_tail_->prev_.lock();
        node->next_ = dummy_tail_;
        node->prev_ = tail_prev;
        tail_prev->next_ = node;
        dummy_tail_->prev_ = node;
    }

    void evictLeastRecent(){
        // 虚拟头结点的下一个就是即将淘汰的节点
        NodePtr least_recent = dummy_head_->next_;
        if(least_recent != dummy_tail_){
            removeNode(least_recent);
            node_map_.erase(least_recent->key_);
        }
    }
    
};
}