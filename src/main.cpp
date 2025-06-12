#include <iostream>
#include <cstdlib> // 用於 std::getenv

// 假設 utils.h 中有 add 函數的宣告
#include "utils.h"

int main() {
    std::cout << "MK1.0" << std::endl;

    // 獲取環境變數
    const char* nameNo = std::getenv("nameNo");
    const char* timeValue = std::getenv("Timevalue");

    // 確保變數存在，否則提供默認值
    if (nameNo == nullptr) {
        nameNo = "DefaultNameNo";
    }
    if (timeValue == nullptr) {
        timeValue = "DefaultTimeValue";
    }

    // 打印環境變數
    std::cout << "Environment Variables:" << std::endl;
    std::cout << "nameNo: " << nameNo << std::endl;
    std::cout << "Timevalue: " << timeValue << std::endl;

    int a = 5, b = 10;
    std::cout << "The sum of " << a << " and " << b << " is " << add(a, b) << std::endl;

    std::cout << "MK2.0" << std::endl;
    return 0;
}
