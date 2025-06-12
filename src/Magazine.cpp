#include "Magazine.h"
#include <iostream> // 在實現文件中包含 <iostream>

Magazine::Magazine(const std::string& title, int issue)
    : Item(title), issue(issue) {}

std::string Magazine::getType() const {
    return "Magazine";
}

void Magazine::printDetails() const {
    std::cout << "Type: " << getType() << "\n"
              << "Title: " << title << "\n"
              << "Issue: " << issue << "\n";
}
