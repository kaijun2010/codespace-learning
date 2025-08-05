#include "snake.h"
#include <chrono>
#include <thread>

SnakeGame::SnakeGame(int h, int w)
    : height_(h), width_(w), direction_(1), running_(true), rng_(std::random_device{}()) {
    Init();
}

SnakeGame::~SnakeGame() {
    endwin();
}

void SnakeGame::Init() {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    win_ = newwin(height_, width_, 0, 0);
    keypad(win_, true);
    nodelay(win_, true);
    box(win_, 0, 0);
    snake_.push_back({height_ / 2, width_ / 2});
    SpawnFood();
    wrefresh(win_);
}

void SnakeGame::SpawnFood() {
    std::uniform_int_distribution<int> distY(1, height_ - 2);
    std::uniform_int_distribution<int> distX(1, width_ - 2);
    Position pos;
    do {
        pos = {distY(rng_), distX(rng_)};
    } while (IsCollision(pos));
    food_ = pos;
}

bool SnakeGame::IsCollision(const Position& pos) const {
    for (const auto& p : snake_) {
        if (p.y == pos.y && p.x == pos.x) return true;
    }
    return false;
}

void SnakeGame::Draw() const {
    werase(win_);
    box(win_, 0, 0);
    mvwaddch(win_, food_.y, food_.x, 'O');
    for (const auto& p : snake_) {
        mvwaddch(win_, p.y, p.x, '#');
    }
    wrefresh(win_);
}

void SnakeGame::Input() {
    int ch = wgetch(win_);
    switch (ch) {
        case KEY_LEFT:
            if (direction_ != 1) direction_ = 0;
            break;
        case KEY_RIGHT:
            if (direction_ != 0) direction_ = 1;
            break;
        case KEY_UP:
            if (direction_ != 3) direction_ = 2;
            break;
        case KEY_DOWN:
            if (direction_ != 2) direction_ = 3;
            break;
        case 'q':
            running_ = false;
            break;
        default:
            break;
    }
}

void SnakeGame::Logic() {
    Position head = snake_.front();
    switch (direction_) {
        case 0: head.x--; break;
        case 1: head.x++; break;
        case 2: head.y--; break;
        case 3: head.y++; break;
    }

    if (head.x <= 0 || head.x >= width_ - 1 || head.y <= 0 || head.y >= height_ - 1 || IsCollision(head)) {
        running_ = false;
        return;
    }

    snake_.push_front(head);
    if (head.y == food_.y && head.x == food_.x) {
        SpawnFood();
    } else {
        snake_.pop_back();
    }
}

void SnakeGame::Run() {
    while (running_) {
        Draw();
        Input();
        Logic();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
