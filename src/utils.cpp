#include "utils.h"
// 這是一個加法函數
int add(int a, int b) { return a + b; }

// 檢查PDK種類ID是否有效
bool isPdkKindIdValid_AH_define(const char* id) {
    const std::vector<std::string> validIds = {"EXF", "ZEF", "SOF", "M1F"};
    for (const auto& validId : validIds) {
        if (strcmp(id, validId.c_str()) == 0) {
            return true;
        }
    }
    return false;
}