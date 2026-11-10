#define NOMINMAX
#include "BaseDictionary.h"
#include <iostream>

// Developer checker: loads and releases a snapshot without TSF registration.
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc != 3 || (std::wstring(argv[1]) != L"--snapshot" && std::wstring(argv[1]) != L"--store"))
            throw std::runtime_error("Usage: harukoto_base_dictionary_check --snapshot FOLDER | --store STORE");
        const auto snapshot = std::wstring(argv[1]) == L"--store"
            ? base_dictionary::loadCurrent(argv[2]) : base_dictionary::Snapshot::load(argv[2]);
        // Ensure allocation and both script modes work, without assuming that
        // every language data set must contain a particular demonstration word.
        auto hans = snapshot->session(true), hant = snapshot->session(false);
        std::cout << "{\"generation\":\"" << snapshot->generation()
                  << "\",\"version\":\"" << snapshot->version()
                  << "\",\"native_loaded\":true,\"runtime_activated\":false}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Base snapshot rejected: " << error.what() << '\n';
        return 1;
    }
}
