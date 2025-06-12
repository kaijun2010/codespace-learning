#include "DataAggregator.h"
#include <iostream>

// 添加數據（加鎖保護共享資源）
void DataAggregator::addData(const std::vector<int>& data) {
    std::lock_guard<std::mutex> lock(dataMutex); // 自動管理鎖的範圍
    aggregatedData.insert(aggregatedData.end(), data.begin(), data.end());
}

// 列印數據
void DataAggregator::printData() const {
    for (int value : aggregatedData) {
        std::cout << value << " ";
    }
    std::cout << std::endl;
}
