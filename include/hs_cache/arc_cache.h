#pragma once
#include "hs_cache/cache.h"
#include "lru_cache.h"
#include "lfu_cache.h"
#include <list>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <cstddef>

namespace hs_cache
{

// ====================== 独立封装：GhostList 幽灵淘汰链表（ARC专用） ======================
// 设计：FIFO淘汰策略，固定容量，O(1)增删/查找，LRU/LFU的Ghost均基于此类实现 提高代码复用性
template<typename Key>
class GhostList{

private:
    std::list<Key> ghost_list_; // FIFO链表 存储淘汰的key full就删除头部
    std::unordered_map<Key, bool> ghost_map_; // 判断key是否存在
    std::size_t capacity_; // 幽灵链表容量

public:
    explicit GhostList(std::size_t capacity) : capacity_(capacity){
        if(capacity == 0){
            throw std::invalid_argument("幽灵链表容量必须大于0")
        }
    }

    ~GhostList(){
        clear();
    }

    void add(const Key& key){
        if(contains(key)){
            remove(key);
        }
        if(ghost_list_.size() >= capacity_){
            Key old_key = ghost_list_.front();
            ghost_list_.pop_front();
            ghost_map_.erase(old_key);
        }

        ghost_list_.push_back(key);
        ghost_map_[key] = true;
    }

    void remove(const Key& key){
        auto map_it = ghost_list_.find(key);
        if(map_it != ghost_list_.end()){
            ghost_list_.remove(key);
            ghost_map_.erase(map_it);
        }
    }

    bool contains(const Key & key) const{
        return ghost_map_.count(key) > 0;
    }

    void clear(){
        ghost_list_.clear();
        ghost_map_.clear();
    }

    std::size_t size() const{
        return ghost_list_.size();
    }

    std::size_t capacity const {
        return capacity_;
    }

};
// ====================== ARC主类：ARCCache（继承统一ICache接口） ======================
// 核心：100%复用LRUCache/KLfuCache
// 线程安全：外层mutex保证ARC组合操作原子性，内层复用LRU/LFU自身的mutex

template<typename Key, typename Value>
class ARCCache : public ICache<Key, Value>{
private:
    // ARC核心 访问次数大于3 从LRU迁移到LFU
    static constexpr size_t transformTime_ = 3;
    const std::size_t total_capacity_; // ARC总容量
    std::size_t partition_; // 分区指针 LRU容量=partition_，LFU=总容量-partition_

    LRUCache<Key, Value> lru_cache_; // arc的LRU分区
    KLfuCache<Key, Value> lfu_cache_; // arc的LFU分区

    GhostList<Key> lru_ghost_; // 存储lru淘汰的key
    GhostList<Key> lfu_ghost_; // 存储lfu淘汰的key

    // 外层互斥锁：保证ARC**组合操作**的原子性（如LRU淘汰+Ghost添加、分区调整）
    // 避免多线程下缓存和GhostList的状态不一致，和你的LRU锁风格一致（RAII）
    mutable std::mutex mutex_;

public:
    // 构造函数初始化总容量 : LRU / LFU各一半
    explicit ARCCache(std::size total_capacity) : total_capacity_(total_capacity),
                                                  partition_(total_capacity / 2),
                                                  lru_cache_(partition_),
                                                  lfu_cache_(total_capacity - partition_),
                                                  lru_ghost_(partition_),
                                                  lfu_ghost_(total_capacity - partition_)
                                                  {
                                                    if(total_capacity < 2){
                                                        throw std::invalid_argument("ARC的初始容量至少为2")
                                                    }
                                                  }
    ~ARCCache() override{
        clear();
    }
     // ====================== 实现ICache统一对外接口 ======================
    // 插入/更新缓存：ARC核心业务逻辑入口
    void put(const Key& key, const Value& value) override{
        std::lock_guard<std::mutex> lock(mutex_);
        arc_core_put(key, value);
    }

    std::optional<Value> get(const Key& key) override{
        std::lock_guard<std::mutex> lock(mutex_);
        return lru_cache_.contains(key) || lfu_cache_.contains(key);
    }

    std::size_t size() const override{
        std::lock_guard<std::mutex> lock(mutex_);
        return lru_cache_.size() + lfu_cache_.size();
    }
    // 清空ARC所有资源：LRU+LFU+双GhostList
    void clear() override{
        std::lock_guard<std::mutex> lock(mutex_);
        lru_cache_.clear();
        lfu_cache_.clear();
        lru_ghost_.clear();
        lfu_ghost_.clear();
    }
private:
    // ====================== ARC核心私有逻辑 ======================
    // ARC内部put逻辑：拆分公有接口，让核心逻辑更清晰
    void arc_core_put(const Key& key, const Value& value) {
        // 规则3：命中LRU → 调用LRU原生put（自动+1访问次数+移到最近使用位）
        if (lru_cache_.contains(key)) {
            lru_cache_.put(key, value);
            // 访问次数≥3次 → 执行LRU→LFU迁移
            if (auto cnt = lru_cache_.getAccessCount(key); cnt && *cnt >= transformTime_) {
                lru_to_lfu_move(key, value);
            }
            return;
        }

        // 规则3：命中LFU → 调用LFU原生put（自动更新频次+调整位置，复用你的LFU逻辑）
        if (lfu_cache_.contains(key)) {
            lfu_cache_.put(key, value);
            return;
        }

        // 规则5：未命中缓存，但LRU Ghost命中 → LRU容量太小，分区右移（LRU+1，LFU-1）
        if (lru_ghost_.contains(key)) {
            adjust_partition(true);    // true=扩大LRU分区
            lru_ghost_.remove(key);    // 从LRU Ghost移除该key
            add_to_lru_safe(key, value);// 安全添加到LRU（满则淘汰）
            return;
        }

        // 规则6：未命中缓存，但LFU Ghost命中 → LFU容量太小，分区左移（LRU-1，LFU+1）
        if (lfu_ghost_.contains(key)) {
            adjust_partition(false);   // false=扩大LFU分区
            lfu_ghost_.remove(key);    // 从LFU Ghost移除该key
            add_to_lru_safe(key, value);// 新数据一律先入LRU（你的规则）
            return;
        }

        // 规则2：缓存穿透（未命中+无Ghost命中）→ 从磁盘取数后插入LRU尾部
        add_to_lru_safe(key, value);
    }

    // ARC内部get逻辑：拆分公有接口，封装核心查询逻辑
    std::optional<Value> arc_core_get(const Key& key) {
        // 先查LRU：调用LRU原生get（自动+1访问次数+移到最近使用位）
        if (auto res = lru_cache_.get(key); res) {
            // 访问次数≥3次 → 执行LRU→LFU迁移
            if (auto cnt = lru_cache_.getAccessCount(key); cnt && *cnt >= transformTime_) {
                lru_to_lfu_move(key, *res);
            }
            return res;
        }

        // 再查LFU：调用LFU原生get（自动更新频次，命中返回值）
        if (auto res = lfu_cache_.get(key); res) {
            return res;
        }

        // 规则2：缓存穿透 → 未命中任何缓存/幽灵链表，返回空
        return std::nullopt;
    }

    // 安全添加到LRU：规则4 → LRU满则淘汰头部，淘汰key加入LRU Ghost，再添加新数据
    void add_to_lru_safe(const Key& key, const Value& value) {
        // LRU已达容量上限，循环淘汰直到有空闲空间
        while (lru_cache_.size() >= lru_cache_.capacity()) {
            // 调用LRU扩展接口，淘汰并获取被淘汰的key
            if (auto evict_key = lru_cache_.evictAndGetKey(); evict_key) {
                lru_ghost_.add(*evict_key); // 淘汰key加入LRU Ghost（Ghost满则内部FIFO淘汰）
            } else {
                break; // 无节点可淘汰，退出循环（容错）
            }
        }
        // 调用LRU原生put，将新数据插入LRU尾部（最近使用位）
        lru_cache_.put(key, value);
    }

    // LRU→LFU迁移：从LRU删除key，插入到LFU（保证数据唯一，不重复存储）
    void lru_to_lfu_move(const Key& key, const Value& value) {
        lru_cache_.remove(key);    // 调用LRU扩展接口，删除指定key
        lfu_cache_.put(key, value);// 调用LFU原生put，插入数据（自动初始化频次）
    }

    // 动态调整分区指针：ARC的核心灵魂（你的规则5/6）
    // is_expand_lru：true=扩大LRU（分区右移），false=扩大LFU（分区左移）
    void adjust_partition(bool is_expand_lru) {
        // 边界保护：LRU最小容量=1，最大容量=总容量-1（保证LFU至少有1个容量，避免单分区）
        if (is_expand_lru) {
            if (partition_ >= total_capacity_ - 1) {
                return; // LRU已达最大容量，无法再扩大
            }
            partition_++; // 分区右移，LRU容量+1
        } else {
            if (partition_ <= 1) {
                return; // LRU已达最小容量，LFU无法再扩大
            }
            partition_--; // 分区左移，LFU容量+1
        }

        // 重新初始化LRU/LFU，设置新的分区容量
        std::size_t new_lru_cap = partition_;
        std::size_t new_lfu_cap = total_capacity_ - partition_;
        lru_cache_ = LRUCache<Key, Value>(new_lru_cap);
        lfu_cache_ = KLfuCache<Key, Value>(new_lfu_cap);

        // 同步调整GhostList容量，和对应缓存分区保持一致
        lru_ghost_ = GhostList<Key>(new_lru_cap);
        lfu_ghost_ = GhostList<Key>(new_lfu_cap);
    }
};

} // namespace hs_cache
