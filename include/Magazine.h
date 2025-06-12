#ifndef MAGAZINE_H
#define MAGAZINE_H

#include "Item.h"

// Magazine 類：頭文件中只聲明接口
class Magazine : public Item {
private:
    int issue;

public:
    Magazine(const std::string& title, int issue);

    std::string getType() const override;   
    void printDetails() const override;
};

#endif // MAGAZINE_H
