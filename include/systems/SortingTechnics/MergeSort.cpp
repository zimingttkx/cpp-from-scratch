#pragma once
#include <vector>
#include <algorithm>

namespace cfs {
    /**
     * 归并排序（Merge Sort）
     *
     * 算法原理（分治法）：
     * 1. 分解（Divide）：把数组递归地分成两半，直到每个子数组只有1个元素
     *    - 单个元素的数组天然是有序的
     * 2. 合并（Merge）：将两个有序的子数组合并成一个有序数组
     *    - 这是归并排序的核心操作
     * 3. 递归地从最小子数组开始，两两合并，直到得到完整的有序数组
     *
     * 时间复杂度：O(n log n) — 每一层合并需要 O(n)，共有 log n 层
     * 空间复杂度：O(n) — 需要额外的辅助数组进行合并
     * 稳定性：✅ 稳定排序（相等的元素不会交换顺序）
     */

    /**
     * 合并两个有序子数组
     *
     * @param arr   待合并的原始数组（会被修改）
     * @param left  左边子数组的起始索引（包含）
     * @param mid   中间索引，左边子数组到此为止（包含）
     * @param right 右边子数组的结束索引（包含）
     *
     * 合并逻辑：
     * 1. 创建一个临时数组 temp，大小为 right - left + 1
     * 2. 设置三个指针：
     *    - i 指向左边子数组的当前比较位置（初始为 left）
     *    - j 指向右边子数组的当前比较位置（初始为 mid + 1）
     *    - k 指向临时数组的写入位置（初始为 0）
     * 3. 比较 arr[i] 和 arr[j]，把较小的放入 temp[k]
     * 4. 重复步骤3，直到左边或右边子数组全部拷贝完毕
     * 5. 把剩余的子数组剩余元素全部拷贝到 temp
     * 6. 把 temp 的内容拷贝回原始数组 arr[left..right]
     */

    template<typename T>
    void merge(std::vector<T>& arr, int left, int mid, int right) {
        int left_size = mid - left + 1;
        int right_size = right - mid;

        std::vector<T> temp(right - left + 1);
        // 指向左边/右边/临时子数组的当前元素
        int i = 0; int j = 0; int k = 0;
        // 当两个数组都有元素的时候
        while (i < left_size && j < right_size) {
            if (arr[left + i] <= arr[mid + 1 + j]) {
                // 左边数组的当前元素小于或者等于 等于确保了稳定性
                temp[k] = arr[left + i];
                i++;
            }else {
                // 右边子数组的当前元素较小
                temp[k] = arr[mid + 1 + j];
                j++;
            }
            k++; // 每一个循环临时数组的下标前进一个位置 因为已经有一个数字排好序了
        }
        // 当左边的子数组还有元素 这个时候右边已经全部处理完了 可以直接把左边的拷贝过去
        while(i < left_size){
            temp[k] = arr[left + i];
            k++; i++;
        }
        while (j < right_size) {
            temp[k] = arr[mid + 1 + j];
            k++; j++;
        }
        for (int i = 0; i < right - left + 1; i++) {
            arr[left + i] = temp[i];
        }


    }

    /**
     * 归并排序的递归处理函数
     *
     * @param arr   待排序的数组
     * @param left  当前处理范围的左边界索引（包含）
     * @param right 当前处理范围的右边界索引（包含）
     *
     * 递归终止条件：
     * - 当 left >= right 时，表示子数组只有 1 个元素或为空
     * - 单个元素是有序的，无需再拆分
     *
     * 递归过程：
     * 1. 计算中间位置 mid = (left + right) / 2
     * 2. 递归排序左半部分 [left..mid]
     * 3. 递归排序右半部分 [mid+1..right]
     * 4. 合并两个有序的子数组
     */
    template<typename T>
    void merge_sort_recursive(std::vector<T>& arr, int left, int right) {
        if (left >= right) return;
        int mid = left + (right - left) / 2; // 防止溢出
        merge_sort_recursive(arr, left, mid);
        merge_sort_recursive(arr, mid + 1, right);
        merge(arr, left, mid, right);
    }

}