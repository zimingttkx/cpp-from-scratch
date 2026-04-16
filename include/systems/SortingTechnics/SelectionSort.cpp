#pragma once
#include<algorithm>
#include <vector>

#include <concepts>

namespace cfs {
    /**
 * 解法一：基础选择排序 (Naive Selection Sort)
 * * 算法原理：
 * 1. 在未排序序列中找到最小（大）元素，存放到排序序列的起始位置。
 * 2. 再从剩余未排序元素中继续寻找最小（大）元素，然后放到已排序序列的末尾。
 * 3. 重复第二步，直到所有元素均排序完毕。
 * * 时间复杂度：
 * - 最好情况：O(n²) - 即使数组有序，仍需嵌套循环比较
 * - 最坏情况：O(n²)
 * - 平均情况：O(n²)
 * * 空间复杂度：O(1) - 原地排序
 */
    template<typename T>
    void selection_sort(std::vector<T>& arr) {
        size_t n = arr.size();
        if (n < 2) return;
        for (size_t i = 0; i < n - 1; i++) {
            size_t min_index = i;
            for (size_t j = i + 1; j < n; j++) {
                if (arr[j] < arr[min_index]) {
                    min_index  = j; // 更新最小坐标
                }
            }
            if (min_index != i) {
                std::swap(arr[i], arr[min_index]);
            }
        }
    }


    /**
 * 解法二：基于 C++ 标准库优化的选择排序
 * * 思路：
 * 使用 <algorithm> 中的 std::min_element 来寻找区间最小值。
 * 这种方式利用了标准库的迭代器支持，代码更简洁，且符合现代 C++ 风格。
 * * 复杂度：
 * - 时间复杂度：O(n²)
 * - 空间复杂度：O(1)
 */

    template<typename T>
    void selection_sort_std(std::vector<T>& arr) {
        if (arr.size() < 2) return;
        for (auto it = arr.begin(); it != arr.end(); it++) {
            auto min_it = std::min_element(it, arr.end());
            if (min_it != it) {
                std::iter_swap(it, min_it);
            }
        }
    }
    /**
 * 解法三：双向选择排序 (Bidirectional Selection Sort / Double Selection Sort)
 * * 思路：
 * 在每一次遍历中，同时找出最大值和最小值。
 * 将最小值交换到左端，最大值交换到右端。
 * 这样可以将外部循环的次数减少一半。
 * * 复杂度分析：
 * - 时间复杂度：O(n²) - 比较总次数虽有减少，但数量级不变
 * - 空间复杂度：O(1)
 */
    template<typename T>
    void selection_sort_bidirectional(std::vector<T>& arr) {
        size_t n = arr.size();
        if (n < 2) return;
        size_t left = 0;
        size_t right = n - 1;
        while (left < right) {
            size_t min_index = left;
            size_t max_index = left;
            for (size_t i = left + 1; i <= right; i++) {
                if (arr[i] < arr[min_index]) {
                    min_index = i;
                }else {
                    max_index = i;
                }
            }
            if (min_index != left) {
                std::swap(arr[left], arr[min_index]);
            }
            if (max_index != left) {
                std::swap(arr[right], arr[max_index]);
            }
            left++;
            right--;
        }
    }
}