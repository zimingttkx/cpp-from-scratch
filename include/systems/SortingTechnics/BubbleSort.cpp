#pragma once
#include <algorithm>
#include <concepts>
#include <functional>
#include <iterator>
#include <vector>

namespace cfs{

// 朴素版本 未实现优化

template<typename T>
void bubble_sort(std::vector<T>& arr){
    size_t n = arr.size();
    if(n < 2) return; // 如果数组长度小于2 则不需要排序
    for(size_t i = 0; i < n - 1; i++){
        for(size_t j = 0; j < n - i - 1; j++){
            if(arr[j] > arr[j + 1]){
                std::swap(arr[j], arr[j + 1]);
            }
        }
    }
}
template<typename T>
void bubble_sort_optimized(std::vector<T>& arr){
    size_t n = arr.size();
    if(n < 2) return;
    for(size_t i = 0; i < n - 1; i++){
        bool swapped = false; // 记录本轮有没有进行交换
        for(size_t j = 0; j < n - 1; j++){
            if(arr[j] > arr[j + 1]){
                std::swap(arr[j], arr[j + 1]);
                swapped = true;
            }
        }
        // 如果某一轮没有发生交换 说明数组已经有序了
        if(!swapped) break;
    }
}
}