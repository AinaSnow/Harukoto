#define NOMINMAX
#include "../ThemeReload.h"
#include <cassert>
#include <chrono>
#include <iostream>

struct ThemeReloadTests {
    static void personal() {
        const auto root = std::filesystem::temp_directory_path() / ("harukoto-personal-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(root);
        const auto file = root / "japanese-custom.tsv";
        const std::string first = "riben|日本|5000|にほん|pinyin|日本\n";
        const std::string second = "riben|二本|5000|にほん|pinyin|二本\n";
        const std::vector<JapaneseWord> base{{"coffee", "コーヒー", 5000, "こーひー", "english", "咖啡"}};
        auto active = base;
        ThemeReload reload(root / "themes", base, "", file);
        ULONGLONG tick = 0;
        const auto poll = [&] (bool boundary = true) {
            tick += 1000; reload.poll(false, active, tick); wait(reload);
            return reload.poll(boundary, active, tick);
        };
        assert(!poll() && !reload.failed());
        write(file, first); assert(!poll(false));
        assert(active.size() == 1);
        assert(reload.poll(true, active, tick) && active.size() == 2);
        assert(active[1].text == "日本" && !active[1].theme);
        write(file, first.substr(0, first.size() - 1) + "\r\n");
        auto crlf = readPersonalDictionary(file); assert(crlf && crlf->words.size() == 1);
        write(file, first);
        auto before = reload.personal_;
        assert(!poll() && reload.personal_ == before);
        // Same size and timestamp must not hide a content change.
        const auto stamp = std::filesystem::last_write_time(file);
        write(file, second); std::filesystem::last_write_time(file, stamp);
        assert(poll() && active[1].text == "二本");
        for (const auto &bad : {first + "broken\n", std::string("riben|日本|5000|nihon|pinyin|\n"),
                std::string("riben|日本|5000|にほん||\n"), std::string("riben|") + char(0xff) + "|5000|にほん|pinyin|\n"}) {
            write(file, bad); assert(!poll() && reload.failed() && active[1].text == "二本");
        }
        std::filesystem::remove(file);
        assert(!poll() && reload.failed() && active.size() == 2);
        // Explicit empty content clears personal entries; absence is not a clear command.
        write(file, ""); assert(poll() && !reload.failed() && active.size() == 1);
        write(file, first); assert(poll() && active.size() == 2);
        // A denied sharing mode retains the last successful snapshot.
        HANDLE locked = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        assert(locked != INVALID_HANDLE_VALUE);
        assert(!poll() && reload.failed() && active.size() == 2);
        CloseHandle(locked); assert(!poll() && !reload.failed());
        write(file, std::string(32 * 1024 * 1024 + 1, 'x'));
        assert(!poll() && reload.failed() && active.size() == 2);
        write(file, first); assert(!poll() && !reload.failed());
        // Theme changes always merge against the current personal snapshot.
        const auto themeRoot = root / "themes"; const std::string gen(32, 'a');
        std::filesystem::create_directories(themeRoot / gen);
        write(themeRoot / gen / "enabled.tsv", "tokyo|東京|5000|とうきょう|pinyin|东京\n");
        write(themeRoot / "CURRENT", gen);
        assert(poll() && active.size() == 3);
        write(file, second); assert(poll() && active.size() == 3 && active[1].text == "二本" && active[2].theme);
        ThemeReload alreadyLoaded(themeRoot, base, gen, file, reload.personal_);
        alreadyLoaded.poll(true, active, 0); wait(alreadyLoaded);
        assert(!alreadyLoaded.poll(true, active, 0) && active.size() == 3);
        std::filesystem::remove_all(root);
    }
    static void bulk(const std::filesystem::path &root, const std::filesystem::path &lexiconRoot, const std::filesystem::path &personalPath) {
        auto snapshot = readJapanesePackageSnapshot(root); assert(snapshot && snapshot->words.size() > 100000);
        const auto enabled = snapshot->generation;
        const std::string disabled(32, 'b');
        std::filesystem::create_directories(root / disabled);
        write(root / disabled / "enabled.tsv", "");
        std::ifstream file(lexiconRoot / "native-pinyin.tsv", std::ios::binary); assert(file);
        const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        auto *lexicon = mpn_lexicon_create(data.data(), data.size()); assert(lexicon);
        auto *session = mpn_session_create(lexicon); assert(session);
        mpn_set_simplified(session, true);
        auto personal = readPersonalDictionary(personalPath); assert(personal && personal->words.size() == 1);
        std::vector<JapaneseWord> active; ThemeReload reload(root, {}, "", personalPath);
        const auto begin = std::chrono::steady_clock::now();
        reload.poll(false, active, 0); wait(reload);
        assert(active.empty());
        assert(reload.poll(true, active, 0) && active.size() == snapshot->words.size() + 1);
        for (bool simplified : {false, true}) {
            mpn_set_simplified(session, simplified);
            mpn_clear(session); for (char c : std::string("testcoffee")) mpn_key(session, c);
            const auto before = windowsCandidates(session, active, [](const std::string &) { return ChineseEvidence{}; }, false);
            const auto coffee = std::find_if(before.begin(), before.end(), [](const auto &c) { return c.text == "コーヒー"; });
            assert(coffee != before.end() && coffee->directJapanese && !coffee->coreIndex);
            const auto kana = std::find_if(before.begin(), before.end(), [](const auto &c) { return c.text == "こーひー"; });
            assert(kana != before.end());
        }
        mpn_set_simplified(session, true);
        const auto check = [&] (bool expected) {
            mpn_clear(session); for (char c : std::string("tejihuifuyao")) assert(mpn_key(session, c));
            const auto choices = windowsCandidates(session, active, [](const std::string &) { return ChineseEvidence{}; }, false);
            assert(std::any_of(choices.begin(), choices.end(), [](const auto &c) { return c.text == "エクスポーション"; }) == expected);
            if (expected) {
                assert(choices.front().text == "特级恢复药" && choices.front().coreIndex);
                assert(mpn_choose(session, *choices.front().coreIndex));
                assert(std::string(mpn_committed(session)) == "特级恢复药");
            }
        };
        check(true);
        write(root / "CURRENT", disabled);
        reload.poll(false, active, 1000); wait(reload);
        reload.poll(false, active, 1000); assert(!active.empty());
        mpn_cancel_composition(session); reload.poll(true, active, 1000); assert(active.size() == 1 && active.front().reading == "testcoffee"); check(false);
        write(root / "CURRENT", enabled);
        reload.poll(false, active, 2000); wait(reload);
        mpn_cancel_composition(session); reload.poll(true, active, 2000); check(true);
        std::cout << "Full theme reload/disable/rollback with " << active.size() << " rows: "
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() << " ms total\n";
        mpn_session_destroy(session); mpn_lexicon_destroy(lexicon);
    }
    static void write(const std::filesystem::path &path, const std::string &text) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc); stream << text; assert(stream);
    }
    static void wait(ThemeReload &reload) {
        const auto deadline = GetTickCount64() + 10000;
        assert(reload.job_);
        while (!reload.job_->done.load(std::memory_order_acquire) && GetTickCount64() < deadline) Sleep(1);
        assert(reload.job_->done.load(std::memory_order_acquire));
    }
    static void run() {
        const auto root = std::filesystem::temp_directory_path() / ("harukoto-reload-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(root);
        const std::vector<JapaneseWord> base{{"coffee", "コーヒー", 5000, "こーひー", "english", "咖啡"}};
        auto active = base;
        ThemeReload reload(root, base, "");
        reload.poll(true, active, 0); wait(reload); reload.poll(true, active, 0);
        assert(!reload.failed() && active.size() == 1);
        const auto publish = [&](char id, const std::string &rows) {
            const std::string generation(32, id);
            std::filesystem::create_directories(root / generation);
            write(root / generation / "enabled.tsv", rows);
            write(root / "CURRENT", generation);
        };
        const std::string rows = "riben|日本|5000|にほん|pinyin|日本\n";
        publish('a', rows);
        reload.poll(false, active, 1000); wait(reload);
        for (int i = 0; i < 10; ++i) assert(!reload.poll(false, active, 1000));
        assert(active.size() == 1 && reload.activeGeneration().empty());
        assert(reload.poll(true, active, 1000));
        assert(active.size() == 2 && active[0].reading == "coffee" && active[1].theme);
        assert(reload.activeGeneration() == std::string(32, 'a'));
        // The same native session sees updated theme choices only after the boundary.
        const std::string data = "MEOWPUUT-PINYIN-1\nriben\tri4ben3\t日本\t日本\t9000\n";
        auto *lexicon = mpn_lexicon_create(data.data(), data.size()); assert(lexicon);
        auto *session = mpn_session_create(lexicon); assert(session);
        for (char c : std::string("riben")) assert(mpn_key(session, c));
        const auto lookup = [](const std::string &) { return ChineseEvidence{}; };
        const auto oldChoices = windowsCandidates(session, active, lookup, false);
        assert(std::any_of(oldChoices.begin(), oldChoices.end(), [](const auto &c) { return c.text == "にほん"; }));
        // Valid empty snapshot means disable all themes, not a failed load.
        publish('b', "");
        assert(readJapanesePackageSnapshot(root).has_value());
        reload.poll(false, active, 2000); wait(reload); reload.poll(false, active, 2000);
        assert(active.size() == 2 && std::string(mpn_input(session)) == "riben");
        assert(mpn_choose(session, *oldChoices.front().coreIndex));
        assert(std::string(mpn_committed(session)) == "日本");
        assert(reload.poll(true, active, 2000) && active.size() == 1);
        mpn_clear(session); for (char c : std::string("riben")) mpn_key(session, c);
        const auto disabled = windowsCandidates(session, active, lookup, false);
        assert(std::none_of(disabled.begin(), disabled.end(), [](const auto &c) { return c.text == "にほん"; }));
        // A broken update must not clear either the base or last valid theme.
        publish('c', "broken\n");
        reload.poll(true, active, 3000); wait(reload); reload.poll(true, active, 3000);
        assert(reload.failed() && active.size() == 1 && reload.activeGeneration() == std::string(32, 'b'));
        write(root / "CURRENT", std::string(32, 'a'));
        reload.poll(true, active, 4000); wait(reload); reload.poll(true, active, 4000);
        assert(!reload.failed() && active.size() == 2);
        write(root / "CURRENT", "../escape");
        reload.poll(true, active, 5000); wait(reload); reload.poll(true, active, 5000);
        assert(reload.failed() && active.size() == 2);
        std::filesystem::remove(root / "CURRENT");
        reload.poll(true, active, 6000); wait(reload); reload.poll(true, active, 6000);
        assert(reload.failed() && active.size() == 2);
        publish('d', rows + "nihon|日本|4000|にほん|pinyin|日本\n");
        reload.poll(true, active, 7000); wait(reload); reload.poll(true, active, 7000);
        assert(!reload.failed() && active.size() == 3);
        // Repeated generations do not duplicate rows; no-change is successful.
        reload.poll(true, active, 8000); wait(reload); assert(!reload.poll(true, active, 8000));
        assert(active.size() == 3 && !reload.failed());
        // Worker lifetime is independent of its owning service.
        std::shared_ptr<ThemeReload::Job> detached;
        {
            ThemeReload transient(root, base, "");
            transient.poll(false, active, 0); detached = transient.job_;
        }
        const auto deadline = GetTickCount64() + 10000;
        while (!detached->done.load(std::memory_order_acquire) && GetTickCount64() < deadline) Sleep(1);
        assert(detached->done.load(std::memory_order_acquire) && detached->result);
        mpn_session_destroy(session); mpn_lexicon_destroy(lexicon);
        std::filesystem::remove_all(root);
    }
};
int main(int argc, char **argv) {
    if (argc == 4) ThemeReloadTests::bulk(argv[1], argv[2], argv[3]);
    else { assert(argc == 1); ThemeReloadTests::run(); ThemeReloadTests::personal(); }
    std::cout << "PASS: background theme reload checks\n";
}
