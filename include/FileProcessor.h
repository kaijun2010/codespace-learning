#ifndef FILEPROCESSOR_H
#define FILEPROCESSOR_H

#include <string>
#include <vector>

// FileProcessor 類：負責處理檔案的讀取
class FileProcessor {
public:
    static std::vector<int> readFile(const std::string& fileName);
};

#endif // FILEPROCESSOR_H
