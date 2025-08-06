#pragma once
#include <deque>
#include <ncurses.h>
#include <random>

struct Position {
    int y;
    int x;
};

class SnakeGame {
public:
    SnakeGame(int height, int width);
    ~SnakeGame();
    void Run();

private:
    void Init();
    void Reset();
    void Draw() const;
    void Input();
    void Logic();
    void SpawnFood();
    bool IsCollision(const Position& pos) const;

    int height_;
    int width_;
    WINDOW* win_;
    std::deque<Position> snake_;
    Position food_;
    int direction_; // 0: left, 1: right, 2: up, 3: down
    bool running_;
    std::mt19937 rng_;
};
