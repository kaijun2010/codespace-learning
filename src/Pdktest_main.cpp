#include <iostream>
#include <cstring>

// 原始寫法
bool isCustomPdkKindValid(const char* id) {
    return !strcmp(id, "EXF") || !strcmp(id, "ZEF") || !strcmp(id, "SOF") || !strcmp(id, "M1F");
}

// 宏定義
#define IS_AH_CUSTOM_PDK_KIND_VALID(id) \
    (!strcmp(id, "EXF") || !strcmp(id, "ZEF") || !strcmp(id, "SOF") || !strcmp(id, "M1F"))

void testValidation(const char* id) {
    bool result1 = isCustomPdkKindValid(id);       // 使用函式
    bool result2 = IS_AH_CUSTOM_PDK_KIND_VALID(id); // 使用宏

    std::cout << "Testing ID: " << id << "\n";
    std::cout << "Using Function: " << (result1 ? "Valid" : "Invalid") << "\n";
    std::cout << "Using Macro:    " << (result2 ? "Valid" : "Invalid") << "\n";

    if (result1 == result2) {
        std::cout << "Results Match!\n";
    } else {
        std::cout << "Results Do Not Match!\n";
    }
    std::cout << "------------------------\n";
}

int main() {
    // 測試多種 ID
    const char* testIds[] = {"EXF", "ZEF", "SOF", "M1F", "ABC", "123", nullptr};

    std::cout << "test" << std::endl;
    for (int i = 0; testIds[i] != nullptr; ++i) {
        testValidation(testIds[i]);
    }

    return 0;
}