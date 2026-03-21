#pragma once
#include <optional>
#include <cstddef>

namespace hs_cache{
    /**
     * @brief 缓存接口的抽象基类 定义了所有缓存策略必须实现的通用操作
     * @tparam key 缓存键的类型
     * @tparam value 缓存值的类型
     * 
     * */
    template<typename Key, typename Value>
    class ICache{
    public:
        // 虚析构函数 确保派生类对象能够正常销毁
        virtual ~ICache() = default;

        /**
         * @brief 将一个键值对放入缓存。
         *        如果键已存在，则更新其值并根据策略调整其位置/状态。
         *        如果缓存已满，则根据替换策略移除一个元素。
         * @param key 要插入或更新的键
         * @param value 要插入或更新的值
         */
        virtual void put(const Key& key, const Value& value) = 0;

        /**
         * @brief 从缓存中获取与给定键关联的值。
         *        如果键不存在，返回一个空的 std::optional。
         *        如果键存在，返回包含值的 std::optional，并根据策略（如LRU）更新其状态。
         * @param key 要查找的键
         * @return 包含值的 std::optional，如果键不存在则为空
         */
        virtual std::optional<Value> get(const Key& key) = 0;

        /**
         * @brief 检查缓存是否包含指定的键。
         * @param key 要检查的键
         * @return 如果缓存包含该键则为 true，否则为 false
         */
        virtual bool contains(const Key& key) const = 0;

        // 获取缓存中当前存储的元素数量
        virtual std::size_t size() const = 0;

        // 获取缓存的最大容量
        virtual std::size_t capacity() const = 0;

        // 清空缓存 移除所有元素
        virtual void clear() = 0;
    };
}