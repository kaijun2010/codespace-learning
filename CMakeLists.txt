# 設置最低 CMake 版本
cmake_minimum_required(VERSION 3.10)

# 專案名稱
project(MyProject VERSION 1.0)

# 設置 C++ 標準
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)


# MyExecutable 的目標檔案
add_executable(MyExecutable
    src/main.cpp
    src/utils.cpp
)
target_include_directories(MyExecutable PUBLIC ${PROJECT_SOURCE_DIR}/include)

# MultiplicationTableExec 的目標檔案
add_executable(MultiplicationTableExec
    src/main_multiplication.cpp
    src/MultiplicationTable.cpp
)
target_include_directories(MultiplicationTableExec PUBLIC ${PROJECT_SOURCE_DIR}/include)

# 添加圖形處理系統執行檔
add_executable(GraphicsExec
    src/graphics_main.cpp
    src/Rectangle.cpp
    src/Circle.cpp
)
target_include_directories(GraphicsExec PUBLIC ${PROJECT_SOURCE_DIR}/include)


# 添加執行檔案
add_executable(LibraryExec
    src/library_main.cpp
    src/Item.cpp
    src/Book.cpp
    src/Magazine.cpp
    src/Library.cpp
)
target_include_directories(LibraryExec PUBLIC ${PROJECT_SOURCE_DIR}/include)


add_executable(MultiThreadExec
    src/MultiThreadExec_main.cpp
    src/FileProcessor.cpp
    src/DataAggregator.cpp
)
target_include_directories(MultiThreadExec PUBLIC ${PROJECT_SOURCE_DIR}/include)


add_executable(PdkTest
    src/Pdktest_main.cpp
)
target_include_directories(PdkTest PUBLIC ${PROJECT_SOURCE_DIR}/include)


add_executable(Copilot_m
    src/Copilot_m.cpp
    src/Copilot.cpp
    src/MultiplicationTable.cpp
)
target_include_directories(Copilot_m PUBLIC ${PROJECT_SOURCE_DIR}/include)

add_executable(test_Copilot_m
    src/test_Copilot_m.cpp
    src/Copilot.cpp # 添加 Copilot_m.cpp 作為連結對象
)
target_include_directories(test_Copilot_m PUBLIC ${PROJECT_SOURCE_DIR}/include)

add_executable(epoch
    src/epoch.cpp
)
target_include_directories(epoch PUBLIC ${PROJECT_SOURCE_DIR}/include)

# 添加 pthread 庫（Linux 平台需要顯式鏈接 pthread）
if(UNIX)
    target_link_libraries(MultiThreadExec pthread)
endif()

add_executable(TicTacToe
    src/TicTacToe_m.cpp
)
target_include_directories(TicTacToe PUBLIC ${PROJECT_SOURCE_DIR}/include)
