#include "../RuntimeStatus.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

static std::string fields(const std::string &phase, char base) {
    return "\"service_version\":\"0.4.4-alpha.35\",\"base_version\":\"1.0.0\",\"base_generation\":\"" +
        std::string(64, base) + "\",\"theme_generation\":\"" + std::string(32, 'f') +
        "\",\"pending_base_generation\":\"" + std::string(64, 'c') + "\",\"phase\":\"" +
        phase + "\",\"last_apply_failed\":false"
        ",\"input_mode\":\"chinese\",\"open_close_changes\":1,\"host_closes\":0,\"candidate_ui\":\"own\""
        ",\"ui_elements\":2,\"ui_elements_host_drawn\":0,\"ui_element_failures\":0"
        ",\"compositions\":2,\"composition_failures\":0,\"compositions_terminated\":0"
        ",\"engine_path\":\"engine\",\"engine_served\":123,\"engine_fallbacks\":4,\"engine_reason\":\"unrepresentable\""
        ",\"engine_memory_behind\":0,\"engine_sequence\":9,\"engine_cutovers\":1,\"engine_cutover_failures\":0"
        ",\"engine_incompatible\":false,\"engine_yields\":2,\"engine_reclaimed\":1,\"engine_local_lexicon\":false"
        ",\"renderer_source\":\"desktop\",\"renderer_sequence\":5,\"renderer_payload\":1,\"renderer_handoffs\":2,\"renderer_failures\":0";
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
