#include "FileProcessor.h"
#include "DataAggregator.h"
#include <thread>
#include <vector>
#include <string>
#include <iostream>


int main() {
    // 檔案列表
    std::vector<std::string> files = {"file1.txt", "file2.txt", "file3.txt"};

    // 數據彙總類
    DataAggregator aggregator;

    // 執行緒向量
    std::vector<std::thread> threads;

    // 為每個檔案創建執行緒
    for (const auto& file : files) {
        threads.emplace_back([&aggregator, &file]() {
            auto data = FileProcessor::readFile(file);
            aggregator.addData(data);
        });
    }

    // 等待所有執行緒完成
    for (auto& thread : threads) {
        thread.join();
    }

    // 列印彙總數據
    std::cout << "Aggregated Data: ";
    aggregator.printData();

    return 0;
}
