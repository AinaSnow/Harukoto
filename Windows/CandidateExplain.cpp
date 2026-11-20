// DICT-02a: harukoto_candidate_explain.exe, a read-only console tool that
// shows where each candidate of a reading comes from and which sources
// conflict. See docs/CANDIDATE_EXPLAIN.md.
#include "CandidateExplain.h"
#include <iostream>

namespace {
std::string utf8(const std::wstring &text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring wide(const std::string &text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

// Console output is written as UTF-16 so that Chinese shows whatever the code
// page; redirected output is UTF-8.
void write(const std::string &text) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        const auto value = wide(text);
        DWORD written = 0;
        WriteConsoleW(out, value.data(), static_cast<DWORD>(value.size()), &written, nullptr);
    } else {
        std::cout << text;
        std::cout.flush();
    }
}
bool readLine(std::string &line) {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode)) {
        std::wstring value;
        wchar_t buffer[512];
        for (;;) {
            DWORD count = 0;
            if (!ReadConsoleW(in, buffer, 512, &count, nullptr) || !count) return !value.empty();
            value.append(buffer, count);
            if (value.back() == L'\n') break;
        }
        while (!value.empty() && (value.back() == L'\n' || value.back() == L'\r')) value.pop_back();
        line = utf8(value);
        return true;
    }
    if (!std::getline(std::cin, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

// The folder of the registered input service, where the installer puts the
// dictionaries the IME actually loads.
std::filesystem::path registeredFolder() {
    wchar_t value[32768];
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\{961C0ADA-4DC6-488F-9563-4FC144EFAE27}\\InprocServer32",
                     nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS) return {};
    return std::filesystem::path(value).parent_path();
}
std::filesystem::path programFolder() {
    if (auto folder = registeredFolder(); !folder.empty() && std::filesystem::exists(folder / "native-pinyin.tsv")) return folder;
    wchar_t self[32768];
    const DWORD length = GetModuleFileNameW(nullptr, self, 32768);
    const auto here = std::filesystem::path(std::wstring(self, length)).parent_path();
    for (const auto &candidate : {here / "payload", here, here.parent_path() / "ime", here.parent_path().parent_path() / "ime"})
        if (std::filesystem::exists(candidate / "native-pinyin.tsv")) return candidate;
    return {};
}
std::filesystem::path localFolder() {
    wchar_t value[32768];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", value, 32768);
    return length && length < 32768 ? std::filesystem::path(std::wstring(value, length)) / L"Meowpuut" : std::filesystem::path();
}

const char *usage =
    "晴词候选诊断（DICT-02a，只读）\n"
    "用法：harukoto_candidate_explain [选项] [拼音 ...]\n"
    "  拼音          诊断这串拼音的候选：来源、得分组成、个人数据影响和冲突\n"
    "  --text 文字   反查：文字在基础词库、个人词、选词记忆、日语词表和主题中的位置\n"
    "  --audit       检查个人词读音、个人词与词库重复、自定义词与主题同触发\n"
    "  --all         列出全部候选（含翻页扩展）；默认前 20 项\n"
    "  --limit=N     显示前 N 项\n"
    "  --context=文字 组字前的上文（默认无）\n"
    "  --hans / --hant  简体／繁体（默认读取当前用户设置）\n"
    "  --fuzzy=MASK  模糊音（CN-03 位掩码，0 关）；--typo=0|1 纠错；--memory=0|1 选词记忆\n"
    "  --data=目录   程序词库目录（默认：已注册的输入法目录）\n"
    "  --local=目录  用户数据目录（默认：%LOCALAPPDATA%\\Meowpuut）\n"
    "  --bundled     忽略已安装的基础词库快照，只用程序目录\n"
    "不带参数时进入交互模式：每行输入拼音，或以 = 开头反查文字（=公司），空行退出。\n"
    "只读取文件和设置，不写入任何文件，不联网。\n";
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        candidate_explain::Options options;
        options.local = localFolder();
        options = candidate_explain::settingsFromRegistry(options);
        std::vector<std::string> inputs, texts;
        bool audit = false, interactive = true;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = utf8(argv[i]);
            std::string v;
            const auto value = [&](const char *name) {
                const std::string prefix = std::string(name) + "=";
                if (argument.rfind(prefix, 0) != 0) return false;
                v = argument.substr(prefix.size());
                return true;
            };
            if (argument == "--help" || argument == "-h" || argument == "/?") { write(usage); return 0; }
            else if (argument == "--text" && i + 1 < argc) texts.push_back(utf8(argv[++i]));
            else if (value("--text")) texts.push_back(v);
            else if (argument == "--audit") audit = true;
            else if (argument == "--all") { options.limit = 0; options.expand = true; }
            else if (value("--limit")) options.limit = std::stoul(v);
            else if (value("--context")) options.context = v;
            else if (argument == "--hans") options.simplified = true;
            else if (argument == "--hant") options.simplified = false;
            else if (value("--fuzzy")) options.fuzzy = static_cast<uint32_t>(std::stoul(v, nullptr, 0));
            else if (value("--typo")) options.typo = v != "0";
            else if (value("--memory")) options.memory = v != "0";
            else if (value("--data")) options.program = std::filesystem::u8path(v);
            else if (value("--local")) options.local = std::filesystem::u8path(v);
            else if (argument == "--bundled") options.baseSnapshot = false;
            else if (!argument.empty() && argument[0] == '-') throw std::runtime_error("未知选项：" + argument + "（--help 查看用法）");
            else inputs.push_back(argument);
        }
        interactive = inputs.empty() && texts.empty() && !audit;
        if (options.program.empty()) options.program = programFolder();
        if (options.program.empty()) throw std::runtime_error("找不到词库目录；请用 --data 指定含 native-pinyin.tsv 的目录");
        if (options.local.empty()) throw std::runtime_error("找不到用户数据目录；请用 --local 指定");
        if (interactive) write("正在读取词库……\n");
        candidate_explain::Explainer explainer(options);
        write(explainer.sources() + "\n");
        for (const auto &input : inputs) write(explainer.explain(input) + "\n");
        for (const auto &text : texts) write(explainer.lookup(text) + "\n");
        if (audit) write(explainer.audit() + "\n");
        if (!interactive) return 0;
        write("输入拼音诊断候选，=文字 反查，空行退出。\n");
        for (std::string line;;) {
            write("> ");
            if (!readLine(line) || line.empty()) break;
            if (line[0] == '=') write(explainer.lookup(line.substr(1)) + "\n");
            else if (line == "?audit") write(explainer.audit() + "\n");
            else write(explainer.explain(line) + "\n");
        }
        return 0;
    } catch (const std::exception &error) {
        write(std::string("错误：") + error.what() + "\n");
        return 1;
    }
}
