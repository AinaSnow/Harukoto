#pragma once
#include "EngineRelease.h"

// RH-2: the only trusted source of approved candidate-window renderers
// (docs/RENDERER_HANDOFF.md). Shares the registry decoding, path validation
// and ACL verification rules with EngineRelease.h under a separate HKLM key
// so older engine-only readers remain unaffected.
// All read() calls are blocking system calls: run them on a background thread,
// never inside a TSF foreground callback.
namespace harukoto::renderer::release {
namespace shared = harukoto::engine::release;

constexpr wchar_t DefaultKey[] = L"SOFTWARE\\Harukoto\\RendererReleases";
constexpr wchar_t FormatLine[] = L"format=harukoto-renderer-release/1";
constexpr wchar_t FormatPrefix[] = L"format=harukoto-renderer-release/";
constexpr wchar_t RendererName[] = L"MeowpuutGlassPreview.exe";

// Payload version sent by this DLL (checked against CandidatePayload.cs and
// InstallSupport.ps1 by renderer_release_install).
constexpr uint16_t PayloadVersion = 1;
constexpr ULONG_PTR PayloadTagV1 = 0x4D50;
constexpr ULONG_PTR PayloadTagBase = 0x4D500000;
inline ULONG_PTR payloadTag(uint16_t version = PayloadVersion) {
    return version == 1 ? PayloadTagV1 : (PayloadTagBase + version);
}

using shared::MaxEntries;
using shared::MaxIdLength;
using shared::MaxPathLength;
using shared::MaxValueBytes;
using shared::MaxLines;
using shared::MaxSequence;
using shared::Parse;
using shared::Status;
using shared::Object;
using shared::validId;
using shared::validPath;
using shared::splitMulti;
using shared::protectedDescriptor;
using shared::protectedHandle;
using shared::protectedDirectory;

struct Entry {
    std::wstring id;
    uint64_t sequence = 0;
    uint16_t minPayload = 0, maxPayload = 0;
    std::filesystem::path renderer;
    std::string rendererSha256;
};

struct Snapshot {
    Status status = Status::Invalid;
    DWORD error = 0;
    std::vector<Entry> entries;
    size_t rejected = 0, unsupported = 0;
};

// Production callers use the defaults. Tests inject an HKCU subkey and may
// disable the protection checks, which a user-writable location cannot pass.
struct Source {
    HKEY root = HKEY_LOCAL_MACHINE;
    std::wstring subkey = DefaultKey;
    bool requireProtected = true;
};

inline bool parsePayloadRange(std::wstring_view text, uint16_t &minOut, uint16_t &maxOut) {
    const auto dash = text.find(L'-');
    if (dash == std::wstring_view::npos || text.find(L'-', dash + 1) != std::wstring_view::npos) return false;
    uint64_t lo = 0, hi = 0;
    if (!shared::detail::decimal(text.substr(0, dash), 65535, lo)) return false;
    if (!shared::detail::decimal(text.substr(dash + 1), 65535, hi)) return false;
    if (lo > hi) return false;
    minOut = static_cast<uint16_t>(lo);
    maxOut = static_cast<uint16_t>(hi);
    return true;
}

// Strict, fixed-order parse of one value. `name` is the registry value name.
// A different format generation (/N) is Unsupported (skipped) so older DLLs
// tolerate entries written by a newer installer.
inline Parse parse(std::wstring_view name, const std::vector<std::wstring> &lines, Entry &out) {
    if (lines.empty() || lines.size() > MaxLines) return Parse::Invalid;
    if (lines[0] != FormatLine) {
        const std::wstring_view prefix = FormatPrefix;
        uint64_t generation = 0;
        if (std::wstring_view(lines[0]).substr(0, prefix.size()) == prefix &&
                shared::detail::decimal(std::wstring_view(lines[0]).substr(prefix.size()), 65535, generation))
            return Parse::Unsupported;
        return Parse::Invalid;
    }
    static const wchar_t *const keys[] = {
        L"format=", L"id=", L"sequence=", L"payload=", L"renderer=", L"renderer-sha256="
    };
    constexpr size_t count = sizeof(keys) / sizeof(keys[0]);
    if (lines.size() != count) return Parse::Invalid;
    std::wstring_view field[count];
    for (size_t i = 0; i < count; ++i) {
        const std::wstring_view key = keys[i], line = lines[i];
        if (line.size() <= key.size() || line.substr(0, key.size()) != key) return Parse::Invalid;
        field[i] = line.substr(key.size());
    }
    Entry entry;
    uint64_t value = 0;
    if (!validId(name) || field[1] != name) return Parse::Invalid;
    entry.id = std::wstring(name);
    if (!shared::detail::decimal(field[2], MaxSequence, value)) return Parse::Invalid;
    entry.sequence = value;
    if (!parsePayloadRange(field[3], entry.minPayload, entry.maxPayload)) return Parse::Invalid;
    if (!validPath(field[4])) return Parse::Invalid;
    if (!shared::detail::digest(field[5], entry.rendererSha256)) return Parse::Invalid;
    const std::filesystem::path renderer{std::wstring(field[4])};
    if (renderer.filename().native() != RendererName) return Parse::Invalid;
    const auto folder = renderer.parent_path().native();
    if (!shared::detail::equalNoCase(renderer.parent_path().filename().native(), entry.id) || folder.size() <= 3)
        return Parse::Invalid;
    entry.renderer = renderer;
    out = std::move(entry);
    return Parse::Ok;
}

inline Snapshot read(const Source &source = Source{}) {
    Snapshot snapshot;
    snapshot.status = shared::detail::readMultiValues(source.root, source.subkey, source.requireProtected,
        snapshot.error, snapshot.rejected, snapshot.unsupported,
        [&](const std::wstring &name, const std::vector<std::wstring> &lines) {
            Entry entry;
            const auto result = parse(name, lines, entry);
            if (result != Parse::Ok) return result;
            if (source.requireProtected && !protectedDirectory(entry.renderer.parent_path())) return Parse::Invalid;
            snapshot.entries.push_back(std::move(entry));
            return Parse::Ok;
        });
    if (snapshot.status != Status::Ready) snapshot.entries.clear();
    return snapshot;
}

// Highest install sequence whose payload range covers `payloadVersion`.
// A tie is ambiguous and approves nothing.
inline std::optional<Entry> select(const Snapshot &snapshot, uint16_t payloadVersion = PayloadVersion) {
    if (snapshot.status != Status::Ready) return std::nullopt;
    const Entry *best = nullptr;
    bool tie = false;
    for (const auto &entry : snapshot.entries) {
        if (payloadVersion < entry.minPayload || payloadVersion > entry.maxPayload) continue;
        if (!best || entry.sequence > best->sequence) { best = &entry; tie = false; }
        else if (entry.sequence == best->sequence) tie = true;
    }
    if (!best || tie) return std::nullopt;
    return *best;
}
} // namespace harukoto::renderer::release
