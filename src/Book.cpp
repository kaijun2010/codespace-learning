#include "Book.h"
#include <iostream> // 在實現文件中包含 iostream

// 構造函式實現
Book::Book(const std::string& title, const std::string& author, int pages)
    : Item(title), author(author), pages(pages) {}

// 返回物品類型
std::string Book::getType() const {
    return "Book";
}

// 列印詳細資訊
void Book::printDetails() const {
    std::cout << "Type: " << getType() << "\n"
              << "Title: " << title << "\n"
              << "Author: " << author << "\n"
              << "Pages: " << pages << "\n";
}