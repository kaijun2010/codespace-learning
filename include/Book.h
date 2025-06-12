#ifndef BOOK_H
#define BOOK_H

#include "Item.h"

class Book : public Item {
private:
    std::string author;  // 作者名稱
    int pages;           // 頁數

public:
    Book(const std::string& title, const std::string& author, int pages);

    std::string getType() const override;
    void printDetails() const override;
};

#endif // BOOK_H
