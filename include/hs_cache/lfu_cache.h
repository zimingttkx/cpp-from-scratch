#include <limits>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cstddef>
#include <climits>
#include <optional>

namespace hs_cache{

template <typename Key, typename Value>
class KLfuCache;

template<typename Key, typename Value>
class FreqList{
private:
    struct Node{
        std::size_t freq_;
        Key key_;
        Value value_;
        std::weak_ptr<Node> prev_;
        std::shared_ptr<Node> next_;

        Node(const Key& key, const Value& value) : freq_(1), key_(key), value_(value),next_(nullptr){}
        Node() : freq_(0),next_(nullptr){} // 虚拟节点构造函数 无数据 控制边界
    };

    using NodePtr = std::shared_ptr<Node>;
    std::size_t freq_;          // 新增：当前链表对应的访问频次
    NodePtr dummy_head_;        // 虚拟头结点
    NodePtr dummy_tail_;        // 虚拟尾结点

public:
    explicit FreqList(std::size_t freq) : freq_(freq){
        dummy_head_ = std::make_shared<Node>();
        dummy_tail_ = std::make_shared<Node>();
        dummy_head_->next_ = dummy_tail_;
        dummy_tail_->prev_ = dummy_head_;
    }

    bool isEmpty() const{
        return dummy_head_->next_ == dummy_tail_;
    }

    void insertNode(NodePtr node){
        if(!node || !dummy_head_ || !dummy_tail_) return; // 安全检查

        node->prev_ = dummy_tail_->prev_;
        node->next_ = dummy_tail_;
        dummy_tail_->prev_.lock()->next_ = node;
        dummy_tail_->prev_ = node;
    }

    void removeNode(NodePtr node){
        if(!node || !dummy_head_ || !dummy_tail_) return; // 安全检查
        if (node->prev_.expired()) return; // 新增：弱指针失效检查，避免空指针

        auto prev = node->prev_.lock();
        auto next = node->next_;

        if(prev) prev->next_ = next;
        if(next) next->prev_ = prev;

        node->next_ = nullptr;
        node->prev_.reset(); 
    }

    NodePtr getFirstNode() const{
        return dummy_head_->next_;
    }
    // 友元类：允许KLfuCache访问私有成员，减少封装开销
    friend class KLfuCache<Key, Value>;
};

template<typename Key, typename Value>
class KLfuCache : public ICache<Key, Value>{
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
                    , cur_total_num_(0){}

    ~KLfuCache() override { clear();}

    void put(const Key& key, const Value& value) override {
        if(capacity_ == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = node_map_.find(key);
        if(it != node_map_.end()){
            it->second->value_ = value;
            update_node_freq(it->second); // 更新节点频次
            return;
        }
        if(node_map_.size() >= capacity_){
            evict_least_freq(); // 淘汰访问频次最少的节点
        }
        
        NodePtr new_node = std::make_shared<Node>(key, value);
        insert_node_to_freq_list(new_node);
        node_map_[key] = new_node;
        min_freq_ = std::min(min_freq_, static_cast<std::size_t>(1));
        add_freq_num();  // 统计访问次数（新节点创建视为一次访问）
    }

    std::optional<Value> get(const Key& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = node_map_.find(key);
        if(it != node_map_.end()){
            update_node_freq(it->second); // 更新访问次数
            return it->second->value_; // 返回节点值
        }
        return std::nullopt;
    }

    bool contains(const Key& key) const override {
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
        freq_to_freq_list_.clear(); 
        min_freq_ = std::numeric_limits<std::size_t>::max();
        cur_total_num_  = 0;
        cur_average_num_ = 0;
    }

private:
    // 更新节点访问频次：移除旧频次链表→频次+1→插入新频次链表
    void update_node_freq(NodePtr node)
    {
        std::size_t old_freq = node->freq_;
        // 新增：检查key是否存在，避免自动插入空值
        if (!freq_to_freq_list_.count(old_freq)) return;
        // 从旧频次链表中移除节点
        freq_to_freq_list_[old_freq]->removeNode(node);
        // 频次+1，更新节点热度
        node->freq_++;
        // 插入到新频次的链表中
        insert_node_to_freq_list(node);
        // 新增：检查key是否存在，避免空指针
        if (old_freq == min_freq_ && freq_to_freq_list_.count(old_freq) && freq_to_freq_list_[old_freq]->isEmpty())
        {
            min_freq_++;
        }
        // 统计访问次数，判断是否触发全局降频
        add_freq_num();
    }

    void insert_node_to_freq_list(NodePtr node){
        if(!node) return;
        std::size_t freq = node->freq_;
        if(freq_to_freq_list_.find(freq) == freq_to_freq_list_.end()){
            
            freq_to_freq_list_[freq] = std::make_shared<FreqList<Key, Value>>(freq);
        }
        freq_to_freq_list_[freq]->insertNode(node);
    }

    void evict_least_freq()
    {
        // 新增：检查最小频次key是否存在，避免空指针
        if (!freq_to_freq_list_.count(min_freq_)) return;
        // 找到最小频次对应的链表，取第一个有效节点（最久未使用）
        NodePtr least_freq_node = freq_to_freq_list_[min_freq_]->getFirstNode();
        if (least_freq_node != freq_to_freq_list_[min_freq_]->dummy_tail_)
        {
            // 从频次链表中移除该节点
            freq_to_freq_list_[min_freq_]->removeNode(least_freq_node);
            // 从哈希表中删除映射
            node_map_.erase(least_freq_node->key_);
            // 减少总访问频次统计
            decrease_freq_num(least_freq_node->freq_);
        }
    }

    // 增加访问频次统计：更新总次数+计算平均值，判断是否触发全局降频
    void add_freq_num()
    {
        cur_total_num_++;
        // 计算平均访问频次：总次数 / 有效节点数（空缓存则为0）
        cur_average_num_ = node_map_.empty() ? 0 : (cur_total_num_ / node_map_.size());
        // 超过最大平均频次阈值，触发全局频次衰减（K-LFU核心优化，解决缓存老化）
        if (cur_average_num_ > max_average_num_)
        {
            handle_over_max_average_num();
        }
    }

    void decrease_freq_num(std::size_t num){
        if(cur_total_num_ >= num){
            cur_total_num_ -= num;
        }else{
            cur_total_num_ = 0;
        }
        cur_average_num_ = node_map_.empty() ? 0 : (cur_total_num_ / node_map_.size());
    }

    // 处理平均访问频次超限：全局所有节点频次衰减，解决LFU缓存老化问题（K-LFU核心）
    void handle_over_max_average_num()
    {
        if (node_map_.empty()) return;
        // 遍历所有节点，频次减半（最低为1，避免频次为0）
        for (auto& pair : node_map_)
        {
            NodePtr node = pair.second;
            if (!node) continue;
            // 新增：检查key是否存在，避免空指针
            if (!freq_to_freq_list_.count(node->freq_)) continue;
            // 从原频次链表移除
            freq_to_freq_list_[node->freq_]->removeNode(node);
            // 频次衰减：超过阈值一半则减一半，否则重置为1
            if (node->freq_ > max_average_num_ / 2)
            {
                node->freq_ -= max_average_num_ / 2;
            }
            else
            {
                node->freq_ = 1;
            }
            // 插入到新频次链表
            insert_node_to_freq_list(node);
        }
        // 衰减后更新全局最小频次
        update_min_freq();
    }

    // 更新全局最小频次：遍历所有频次链表，找到非空的最小频次
    void update_min_freq()
    {
        min_freq_ = std::numeric_limits<std::size_t>::max();
        for (const auto& pair : freq_to_freq_list_)
        {
            // 链表非空时，更新最小频次
            if (pair.second && !pair.second->isEmpty())
            {
                min_freq_ = std::min(min_freq_, pair.first);
            }
        }
        // 缓存非空但最小频次未找到，重置为1（默认初始频次）
        if (min_freq_ == std::numeric_limits<std::size_t>::max() && !node_map_.empty())
        {
            min_freq_ = 1;
        }
    }
};

}