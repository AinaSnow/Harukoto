#include "../RendererRelease.h"
#include <shlobj.h>
#include <cassert>
#include <cstdio>
#include <iostream>

// RH-2: Only HKCU test keys and temporary folders are written. HKLM and
// Program Files are read-only probes for the protection predicate.
namespace release = harukoto::renderer::release;
using release::Entry;
using release::Parse;
using release::Status;

static const std::wstring Id = L"0123456789AB-CDEF01234567";
static const std::wstring Folder = L"C:\\Program Files\\Meowpuut\\IME\\" + Id;
static const std::wstring HashA(64, L'a');

static std::vector<std::wstring> lines(const std::wstring &id = Id, const std::wstring &sequence = L"1",
        const std::wstring &payload = L"1-1", const std::wstring &folder = Folder) {
    return {
        release::FormatLine,
        L"id=" + id,
        L"sequence=" + sequence,
        L"payload=" + payload,
        L"renderer=" + folder + L"\\MeowpuutGlassPreview.exe",
        L"renderer-sha256=" + HashA
    };
}

static Parse parse(const std::vector<std::wstring> &value, const std::wstring &name = Id) {
    Entry entry;
    return release::parse(name, value, entry);
}

static void parsing() {
    Entry entry;
    assert(release::parse(Id, lines(), entry) == Parse::Ok);
    assert(entry.id == Id && entry.sequence == 1 && entry.minPayload == 1 && entry.maxPayload == 1);
    assert(entry.renderer == Folder + L"\\MeowpuutGlassPreview.exe");
    assert(entry.rendererSha256 == std::string(64, 'a'));
    assert(release::payloadTag(1) == 0x4D50);
    assert(release::payloadTag(2) == 0x4D500002);
    assert(parse(lines(Id, L"9007199254740991", L"1-65535")) == Parse::Ok);
    assert(release::parse(Id, lines(Id, L"42", L"2-5"), entry) == Parse::Ok && entry.minPayload == 2 && entry.maxPayload == 5);

    // Unknown generations (/N) are skipped; malformed headers are invalid.
    auto value = lines(); value[0] = L"format=harukoto-renderer-release/2";
    assert(parse(value) == Parse::Unsupported);
    value.resize(1); assert(parse(value) == Parse::Unsupported);
    for (const auto *header : {L"format=harukoto-renderer-release/", L"format=harukoto-renderer-release/02",
            L"format=harukoto-engine-release/1", L"", L"FORMAT=harukoto-renderer-release/1"}) {
        value = lines(); value[0] = header; assert(parse(value) == Parse::Invalid);
    }

    // Exact line count and fixed order.
    assert(parse({}) == Parse::Invalid);
    value = lines(); value.push_back(L"extra=1"); assert(parse(value) == Parse::Invalid);
    value = lines(); value.pop_back(); assert(parse(value) == Parse::Invalid);
    value = lines(); std::swap(value[2], value[3]); assert(parse(value) == Parse::Invalid);
    value = lines(); std::swap(value[4], value[5]); assert(parse(value) == Parse::Invalid);

    // Value name and id line must agree; ids use the restricted charset.
    assert(parse(lines(), L"other") == Parse::Invalid);
    for (const auto *id : {L"", L"-abc", L".abc", L"a b", L"a\\b", L"a/b", L"a:b"})
        assert(parse(lines(id), id) == Parse::Invalid);
    assert(parse(lines(std::wstring(241, L'A')), std::wstring(241, L'A')) == Parse::Invalid);

    // Sequence bounds.
    for (const auto *sequence : {L"0", L"01", L"-1", L"+1", L"9007199254740992", L"99999999999999999", L" 1", L""})
        assert(parse(lines(Id, sequence)) == Parse::Invalid);

    // Payload range validation: min-max, 1..65535, min <= max, single dash.
    for (const auto *payload : {L"", L"1", L"0-1", L"1-0", L"2-1", L"01-2", L"1-02", L"1-65536", L"1-2-3", L"-1-2", L"1-", L"-1", L"a-b"})
        assert(parse(lines(Id, L"1", payload)) == Parse::Invalid);

    // Digest must be 64 lower-case hex characters.
    value = lines(); value[5] = L"renderer-sha256=" + std::wstring(64, L'A'); assert(parse(value) == Parse::Invalid);
    value = lines(); value[5] = L"renderer-sha256=" + std::wstring(63, L'a'); assert(parse(value) == Parse::Invalid);
    value = lines(); value[5] = L"renderer-sha256=" + std::wstring(64, L'g'); assert(parse(value) == Parse::Invalid);

    // Path rules and fixed executable name directly under version folder named by id.
    for (const auto *folder : {L"Program Files\\x", L"C:Program Files", L"\\\\server\\share", L"\\\\?\\C:\\x",
            L"C:/Program Files", L"C:\\a\\..", L"C:\\a\\.\\b", L"C:\\a\\\\b", L"C:\\a.\\b", L"C:\\a \\b", L"C:\\a:stream\\b"}) {
        const std::wstring prefix(folder);
        value = lines(Id, L"1", L"1-1", prefix + L"\\" + Id);
        assert(parse(value) == Parse::Invalid);
    }
    value = lines(); value[4] = L"renderer=" + Folder + L"\\other.exe"; assert(parse(value) == Parse::Invalid);
    value = lines(); value[4] = L"renderer=C:\\Program Files\\Meowpuut\\IME\\other\\MeowpuutGlassPreview.exe"; assert(parse(value) == Parse::Invalid);
    assert(parse(lines(Id, L"1", L"1-1", L"C:\\" + Id)) == Parse::Ok);
    value = lines(); value[1].push_back(L'\0'); assert(parse(value) == Parse::Invalid);
    std::wcout << L"PASS: strict renderer release entry parsing\n";
}

static std::vector<BYTE> multi(const std::vector<std::wstring> &value) {
    std::wstring text;
    for (const auto &line : value) { text += line; text.push_back(L'\0'); }
    text.push_back(L'\0');
    std::vector<BYTE> bytes(text.size() * 2);
    std::memcpy(bytes.data(), text.data(), bytes.size());
    return bytes;
}

struct TestKey {
    std::wstring path;
    harukoto::engine::release::detail::Key key;
    TestKey() {
        path = L"Software\\HarukotoTest\\RendererRelease-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        assert(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key.value, nullptr) == ERROR_SUCCESS);
    }
    ~TestKey() {
        RegCloseKey(key.value); key.value = nullptr;
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\HarukotoTest");
    }
    void put(const std::wstring &name, const std::vector<BYTE> &data, DWORD type = REG_MULTI_SZ) {
        assert(RegSetValueExW(key.value, name.c_str(), 0, type, data.data(), static_cast<DWORD>(data.size())) == ERROR_SUCCESS);
    }
    void put(const std::wstring &name, const std::vector<std::wstring> &value) { put(name, multi(value)); }
    void remove(const std::wstring &name) { assert(RegDeleteValueW(key.value, name.c_str()) == ERROR_SUCCESS); }
    release::Source source(bool protect = false) const { return {HKEY_CURRENT_USER, path, protect}; }
};

static std::wstring folder(const std::wstring &id) { return L"C:\\Program Files\\Meowpuut\\IME\\" + id; }

static void registryAndSelection() {
    release::Source missing{HKEY_CURRENT_USER, L"Software\\HarukotoTest\\AbsentRenderer-" + std::to_wstring(GetCurrentProcessId()), false};
    assert(release::read(missing).status == Status::Missing);
    assert(!release::select(release::read(missing), 1));
    assert(release::read({HKEY_CURRENT_USER, L"", false}).status == Status::Invalid);

    // Production location is either absent or protected HKLM (read-only probe).
    const auto production = release::read();
    assert(production.status == Status::Missing || production.status == Status::Ready);

    TestKey key;
    auto snapshot = release::read(key.source());
    assert(snapshot.status == Status::Ready && snapshot.entries.empty() && !release::select(snapshot, 1));

    // Range selection: V1 (seq 3, 1-1), V2 (seq 4, 1-2), V3 (seq 5, 2-3).
    key.put(L"V1", lines(L"V1", L"3", L"1-1", folder(L"V1")));
    key.put(L"V2", lines(L"V2", L"4", L"1-2", folder(L"V2")));
    key.put(L"V3", lines(L"V3", L"5", L"2-3", folder(L"V3")));
    snapshot = release::read(key.source());
    assert(snapshot.status == Status::Ready && snapshot.entries.size() == 3 && !snapshot.rejected);
    assert(release::select(snapshot, 1)->id == L"V2");
    assert(release::select(snapshot, 2)->id == L"V3");
    assert(release::select(snapshot, 3)->id == L"V3");
    assert(!release::select(snapshot, 4));

    // Corrupt values rejected individually; unknown format skipped.
    key.put(L"BIN", multi(lines(L"BIN", L"9", L"1-1", folder(L"BIN"))), REG_BINARY);
    auto future = lines(L"FUTURE", L"9", L"1-2", folder(L"FUTURE"));
    future[0] = L"format=harukoto-renderer-release/2";
    key.put(L"FUTURE", future);
    snapshot = release::read(key.source());
    assert(snapshot.status == Status::Ready && snapshot.entries.size() == 3 && snapshot.rejected == 1 && snapshot.unsupported == 1);
    assert(release::select(snapshot, 1)->id == L"V2");
    key.remove(L"BIN"); key.remove(L"FUTURE");

    // Tie within the matching payload range approves nothing; other ranges stay unaffected.
    key.put(L"TWIN", lines(L"TWIN", L"4", L"1-1", folder(L"TWIN")));
    snapshot = release::read(key.source());
    assert(!release::select(snapshot, 1) && release::select(snapshot, 2)->id == L"V3");
    key.remove(L"TWIN");

    // Entry count bound fails closed.
    for (int i = 0; i < 6; ++i) {
        const auto id = L"EXTRA" + std::to_wstring(i);
        key.put(id, lines(id, std::to_wstring(10 + i), L"1-1", folder(id)));
    }
    snapshot = release::read(key.source());
    assert(snapshot.status == Status::Invalid && snapshot.error == ERROR_TOO_MANY_NAMES && snapshot.entries.empty());
    for (int i = 0; i < 6; ++i) key.remove(L"EXTRA" + std::to_wstring(i));

    // User-writable HKCU key is rejected when protection is required.
    snapshot = release::read(key.source(true));
    assert(snapshot.status == Status::Unprotected && snapshot.entries.empty() && !release::select(snapshot, 1));
    std::wcout << L"PASS: renderer release registry source, payload ranges, ties, bounds and ACL rejection\n";
}

static std::string utf8(const std::wstring &text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

static int dump(const std::wstring &subkey, uint16_t payloadVersion) {
    const auto snapshot = release::read({HKEY_CURRENT_USER, subkey, false});
    const char *names[] = {"ready", "missing", "unprotected", "invalid"};
    std::printf("status\t%s\t%zu\t%zu\n", names[static_cast<int>(snapshot.status)], snapshot.rejected, snapshot.unsupported);
    for (const auto &entry : snapshot.entries) {
        std::printf("entry\t%s\t%llu\t%u\t%u\t%s\t%s\n",
            utf8(entry.id).c_str(),
            static_cast<unsigned long long>(entry.sequence),
            static_cast<unsigned>(entry.minPayload),
            static_cast<unsigned>(entry.maxPayload),
            utf8(entry.renderer.native()).c_str(),
            entry.rendererSha256.c_str());
    }
    const auto chosen = release::select(snapshot, payloadVersion);
    std::printf("selected\t%s\n", chosen ? utf8(chosen->id).c_str() : "");
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--dump")
        return dump(argv[2], static_cast<uint16_t>(std::stoul(argv[3])));
    parsing();
    registryAndSelection();
    std::wcout << L"PASS: renderer release source\n";
    return 0;
}
