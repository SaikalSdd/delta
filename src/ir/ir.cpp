#include "ir.h"

using namespace delta;

IRBasicBlock::IRBasicBlock(std::string name, delta::IRFunction* parent) : name(std::move(name)), parent(parent) {
    if (parent) {
        parent->body.push_back(this);
    }
}
