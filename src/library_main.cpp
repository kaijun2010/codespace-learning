#include "Book.h"
#include "Magazine.h"
#include "Library.h"
#include <iostream>

int main() {
    Library library;

    // 添加一些物品到圖書館
    library.addItem(std::make_unique<Book>("C++ Programming", "Bjarne Stroustrup", 1024));
    library.addItem(std::make_unique<Magazine>("Tech Monthly", 42));
    library.addItem(std::make_unique<Book>("Effective C++", "Scott Meyers", 320));
    library.addItem(std::make_unique<Magazine>("Science Weekly", 128));

    // 列印所有物品資訊
    library.printAllItems();

    return 0;
}

#if 0
MyLibrary/
├── CMakeLists.txt          # CMake 配置文件
├── include/                # 頭文件
│   ├── Item.h              # 抽象基類Item
│   ├── Book.h              # Book 類
│   ├── Magazine.h          # Magazine 類
│   ├── Library.h           # Library 類
├── src/                    # 源文件
│   ├── Item.cpp            # Item 實現
│   ├── Book.cpp            # Book 實現
│   ├── Magazine.cpp        # Magazine 實現
│   ├── Library.cpp         # Library 實現
│   ├── library_main.cpp    # 主程式
└── build/                  # 構建目錄
#endif