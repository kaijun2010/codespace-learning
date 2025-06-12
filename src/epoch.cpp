#include <iostream>
#include <chrono>
#include <ctime>
#include <cstdlib>

// 取得目前的 Epoch 時間
long getCurrentEpochTime() {
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

// 將 Epoch 時間轉換為 YYYY-MM-DD:HHMMSS 格式
std::string convertEpochToHumanReadable(long epochTime) {
    std::time_t time = static_cast<std::time_t>(epochTime);
    std::tm *timeStruct = std::localtime(&time);
    char buffer[20];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d:%H%M%S", timeStruct);
    return std::string(buffer);
}

int main(int argc, char *argv[]) {
    // 如果沒有輸入參數
    if (argc < 2) {
        std::cout << "Usage: ./epoch <1|2>\n";
        std::cout << "1: Generate current epoch time\n";
        std::cout << "2: Convert epoch time to human-readable format\n";
        std::cout << "Example:\n";
        std::cout << "    ./epoch 1\n";
        std::cout << "    ./epoch 2 1734418442\n";
        return 1;
    }

    int option = std::atoi(argv[1]);

    if (option == 1) {
        // 選項 1: 產生目前的 Epoch 時間
        long epochTime = getCurrentEpochTime();
        std::cout << "Current Epoch time: " << epochTime << std::endl;

    } else if (option == 2) {
        // 選項 2: 轉換指定的 Epoch 時間
        if (argc < 3) {
            std::cerr << "Error: Please provide an epoch time.\n";
            std::cerr << "Usage: ./epoch 2 <epoch_time>\n";
            return 1;
        }
        long inputEpochTime = std::atol(argv[2]);
        std::string humanTime = convertEpochToHumanReadable(inputEpochTime);
        std::cout << "Human-readable time: " << humanTime << std::endl;

    } else {
        // 未知的選項
        std::cerr << "Error: Invalid option. Use 1 or 2.\n";
        std::cerr << "Usage: ./epoch <1|2>\n";
        return 1;
    }

    return 0;
}
