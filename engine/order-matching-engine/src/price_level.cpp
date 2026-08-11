#include "price_level.h"

void PriceLevel::addToTail(Order* o) {
    o->next = nullptr;
    o->prev = tail;

    if (isEmpty()) {
        head = o;
        tail = o;
    } else {
        tail->next = o;
        tail = o;
    }

    totalQuantity += o->quantity;
}

void PriceLevel::remove(Order* o) {
    if (o->prev != nullptr) {
        o->prev->next = o->next;
    } else {
        head = o->next;
    }

    if (o->next != nullptr) {
        o->next->prev = o->prev;
    } else {
        tail = o->prev;
    }

    totalQuantity -= o->quantity;

    o->next = nullptr;
    o->prev = nullptr;
}

bool PriceLevel::isEmpty() const {
    return head == nullptr;
}

Order* PriceLevel::front() const {
    return head;
}