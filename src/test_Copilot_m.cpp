// src/test_Copilot_m.cpp

#include <iostream>
#include <cassert>
#include "Copilot.h"

void test_sum() {
    assert(sum(1, 2) == 3);
    assert(sum(-1, -2) == -3);
    assert(sum(0, 0) == 0);
    std::cout << "sum() tests passed!" << std::endl;
}

void test_max() {
    assert(max(1, 2) == 2);
    assert(max(-1, -2) == -1);
    assert(max(0, 0) == 0);
    std::cout << "max() tests passed!" << std::endl;
}

void test_fibonacci() {
    assert(fibonacci(0) == 0);
    assert(fibonacci(1) == 1);
    assert(fibonacci(5) == 5);
    assert(fibonacci(10) == 55);
    std::cout << "fibonacci() tests passed!" << std::endl;
}

void test_geometric() {
    assert(geometric(1, 2, 3) == 7);
    assert(geometric(2, 3, 2) == 8);
    assert(geometric(3, 2, 4) == 45);
    std::cout << "geometric() tests passed!" << std::endl;
}

void test_binarySearch() {
    int arr[] = {1, 2, 3, 4, 5};
    assert(binarySearch(arr, 3, 0, 4) == 2);
    assert(binarySearch(arr, 1, 0, 4) == 0);
    assert(binarySearch(arr, 5, 0, 4) == 4);
    assert(binarySearch(arr, 6, 0, 4) == -1);
    std::cout << "binarySearch() tests passed!" << std::endl;
}

void test_getCurrentTime() {
    std::string time = getCurrentTime();
    assert(time.length() > 0); // Basic check to ensure the function returns a non-empty string
    std::cout << "getCurrentTime() test passed!" << std::endl;
}

void test_loopWithCurrentTime() {
    // This function prints output and sleeps, so it's not practical to unit test it directly.
    // We can manually verify its behavior by running the program.
    std::cout << "loopWithCurrentTime() test needs manual verification." << std::endl;
}

int main() {
    test_sum();
    test_max();
    test_fibonacci();
    test_geometric();
    test_binarySearch();
    test_getCurrentTime();
    test_loopWithCurrentTime();

    std::cout << "All tests passed!" << std::endl;
    return 0;
}