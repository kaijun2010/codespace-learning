#include <iostream>
#include <cmath>
#include "MultiplicationTable.h"
#include <chrono>
#include <thread>

int sum(int a, int b) {
    return a + b;
}

int max(int a, int b) {
    return a > b ? a : b;
}

// 費氏數列
int fibonacci(int n) {
    if (n <= 1) {
        return n;
    }
    return fibonacci(n - 1) + fibonacci(n - 2);
}
// 等比數列
int geometric(int a, int r, int n) {
    return a * (1 - std::pow(r, n)) / (1 - r);
}

// 二分查找
int binarySearch(int arr[], int target, int left, int right) {
    if (left > right) {
        return -1;
    }
    int mid = left + (right - left) / 2;
    if (arr[mid] == target) {
        return mid;
    } else if (arr[mid] < target) {
        return binarySearch(arr, target, mid + 1, right);
    } else {
        return binarySearch(arr, target, left, mid - 1);
    }
}

// 這個函數可以取得現在系統的時間需要到時分秒毫秒，HH:MM:SS:MS
std::string getCurrentTime() {
    time_t now = time(0);
    tm* ltm = localtime(&now);
    int hour = ltm->tm_hour;
    int minute = ltm->tm_min;
    int second = ltm->tm_sec;
    int millisecond = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count() % 1000;
    return std::to_string(hour) + ":" + std::to_string(minute) + ":" + std::to_string(second) + ":" + std::to_string(millisecond);
}


// 這個函數是10個loop，每個loop 間隔 1 秒，然後每一個loop 呼叫一次 getCurrentTime()
void loopWithCurrentTime() {
    for (int i = 0; i < 10; ++i) {
        std::cout << getCurrentTime() << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1120));
    }
}
