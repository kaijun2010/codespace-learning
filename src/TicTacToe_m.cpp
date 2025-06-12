#include <iostream>
#include <vector>

using namespace std;

// 顯示棋盤
void displayBoard(const vector<vector<char>>& board) {
    cout << "\n";
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            cout << " " << board[i][j];
            if (j < 2) cout << " |";
        }
        cout << "\n";
        if (i < 2) cout << "---|---|---\n";
    }
    cout << "\n";
}

// 檢查是否有勝者
bool checkWin(const vector<vector<char>>& board, char player) {
    // 檢查行和列
    for (int i = 0; i < 3; ++i) {
        if ((board[i][0] == player && board[i][1] == player && board[i][2] == player) || 
            (board[0][i] == player && board[1][i] == player && board[2][i] == player)) {
            return true;
        }
    }
    // 檢查對角線
    if ((board[0][0] == player && board[1][1] == player && board[2][2] == player) || 
        (board[0][2] == player && board[1][1] == player && board[2][0] == player)) {
        return true;
    }
    return false;
}

// 檢查是否平局
bool isDraw(const vector<vector<char>>& board) {
    for (const auto& row : board) {
        for (char cell : row) {
            if (cell == ' ') {
                return false;
            }
        }
    }
    return true;
}

int main() {
    vector<vector<char>> board(3, vector<char>(3, ' ')); // 3x3 棋盤
    char currentPlayer = 'X'; // 先由玩家 X 開始

    cout << "歡迎來到井字棋遊戲！\n";
    displayBoard(board);

    while (true) {
        int row, col;
        cout << "玩家 " << currentPlayer << "，請輸入行和列 (0, 1, 2)，以空格分隔: ";
        cin >> row >> col;

        // 驗證輸入
        if (row < 0 || row >= 3 || col < 0 || col >= 3 || board[row][col] != ' ') {
            cout << "無效輸入，請重新輸入！\n";
            continue;
        }

        // 標記棋盤
        board[row][col] = currentPlayer;
        displayBoard(board);

        // 檢查勝負或平局
        if (checkWin(board, currentPlayer)) {
            cout << "恭喜！玩家 " << currentPlayer << " 獲勝！\n";
            break;
        }
        if (isDraw(board)) {
            cout << "遊戲平局！\n";
            break;
        }

        // 切換玩家
        currentPlayer = (currentPlayer == 'X') ? 'O' : 'X';
    }

    return 0;
}
