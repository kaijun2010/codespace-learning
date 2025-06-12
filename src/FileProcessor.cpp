#include "FileProcessor.h"
#include <iostream>
#include <fstream>
#include <vector>

// 模擬從檔案讀取數據（這裡用隨機數代替真實檔案讀取）
std::vector<int> FileProcessor::readFile(const std::string& fileName) {
    std::vector<int> data = {1, 2, 3, 4, 5}; // 模擬檔案中的數據
    std::cout << "Reading file: " << fileName << std::endl;
    return data;
}
