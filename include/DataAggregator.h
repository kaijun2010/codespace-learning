#ifndef DATAAGGREGATOR_H
#define DATAAGGREGATOR_H

#include <mutex>
#include <vector>

// DataAggregator 類：負責數據彙總
class DataAggregator {
private:
    std::vector<int> aggregatedData; // 彙總數據
    std::mutex dataMutex;            // 保護共享資源的互斥鎖

public:
    void addData(const std::vector<int>& data); // 添加數據
    void printData() const;                    // 列印數據
};

#endif // DATAAGGREGATOR_H
