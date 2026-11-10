#include "../RuntimeStatus.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

static std::string fields(const std::string &phase, char base) {
    return "\"service_version\":\"0.4.4-alpha.21\",\"base_version\":\"1.0.0\",\"base_generation\":\"" +
        std::string(64, base) + "\",\"theme_generation\":\"\",\"pending_base_generation\":\"\",\"phase\":\"" +
        phase + "\",\"last_apply_failed\":false";
}
int main() {
    RuntimeStatus first, second;
    assert(first.open() && second.open());
    first.publish(fields("idle", 'a')); second.publish(fields("loading", 'b'));
    std::cout << "ready" << std::endl;
    for (std::string command; std::getline(std::cin, command);) {
        if (command == "exit") break;
        if (command == "close") first.close();
        else if (command == "reuse") { assert(first.open()); first.publish(fields("waiting_boundary", 'c')); }
        else if (command == "capacity") {
            std::vector<std::unique_ptr<RuntimeStatus>> extra;
            for (int i = 0; i < 62; ++i) {
                auto status = std::make_unique<RuntimeStatus>(); assert(status->open()); extra.push_back(std::move(status));
            }
            RuntimeStatus overflow; assert(!overflow.open());
        } else if (command == "stress") {
            for (int i = 0; i < 50000; ++i) first.publish(fields(i % 2 ? "idle" : "failed", i % 2 ? 'a' : 'b'));
        } else assert(false);
        std::cout << "done" << std::endl;
    }
}
