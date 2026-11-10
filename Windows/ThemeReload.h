#pragma once
#include <windows.h>
#include "JapanesePackages.h"
#include "PersonalDictionary.h"
#include "BaseDictionary.h"
#include <atomic>
#include <memory>

// The worker owns immutable inputs and never calls COM or touches Service.
// It holds a module reference so service teardown does not wait for disk IO,
// and the DLL cannot unload while its worker code is still executing.
class ThemeReload {
    friend struct ThemeReloadTests;
    struct Job {
        std::filesystem::path baseRoot;
        std::string previousBase, baseGeneration;
        std::unique_ptr<base_dictionary::Snapshot> preparedBase;
        std::shared_ptr<const std::vector<JapaneseWord>> preparedWords;
        std::filesystem::path root;
        std::shared_ptr<const std::vector<JapaneseWord>> base;
        std::string previous;
        std::filesystem::path personalPath;
        std::shared_ptr<const PersonalDictionarySnapshot> previousPersonal, personal;
        std::optional<JapanesePackageSnapshot> result;
        bool failed = false;
        std::atomic<bool> done{false};
    };
    struct Start { std::shared_ptr<Job> job; HMODULE module; };
    static std::string generation(const std::filesystem::path &root) {
        std::ifstream file(root / "CURRENT", std::ios::binary);
        char buffer[33]{}; file.read(buffer, sizeof(buffer));
        if (file.gcount() != 32 || !std::all_of(buffer, buffer + 32, [](char c) {
            return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
        })) return {};
        return std::string(buffer, 32);
    }
    static void load(Job &job) noexcept {
        try {
            if (!job.baseRoot.empty()) {
                if (std::filesystem::exists(job.baseRoot / "STATE.json")) {
                    const auto generation = base_dictionary::currentGeneration(job.baseRoot);
                    if (generation != job.previousBase) {
                        job.preparedBase = base_dictionary::loadCurrent(job.baseRoot);
                        job.baseGeneration = job.preparedBase->generation();
                        job.preparedWords = std::make_shared<const std::vector<JapaneseWord>>(job.preparedBase->words());
                    }
                } else if (!job.previousBase.empty()) throw std::runtime_error("base state disappeared");
            }
            const auto current = generation(job.root);
            job.personal = readPersonalDictionary(job.personalPath, !job.previousPersonal || !job.previousPersonal->present, job.previousPersonal);
            if (!job.personal) { job.failed = true; job.done.store(true, std::memory_order_release); return; }
            const bool personalChanged = !job.previousPersonal ||
                job.personal->present != job.previousPersonal->present || job.personal->bytes != job.previousPersonal->bytes;
            if (current.empty()) {
                // No theme configuration on a fresh installation is normal.
                job.failed = !job.previous.empty() || std::filesystem::exists(job.root / "CURRENT");
            }
            if (!job.failed && (current != job.previous || personalChanged || job.preparedBase)) {
                auto next = current.empty() ? std::optional<JapanesePackageSnapshot>(JapanesePackageSnapshot{}) : readJapanesePackageSnapshot(job.root);
                if (!next) job.failed = true;
                else if (next->generation == current && generation(job.root) == current) {
                    auto merged = job.preparedWords ? *job.preparedWords : *job.base;
                    merged.insert(merged.end(), job.personal->words.begin(), job.personal->words.end());
                    merged.insert(merged.end(), std::make_move_iterator(next->words.begin()), std::make_move_iterator(next->words.end()));
                    std::stable_sort(merged.begin(), merged.end(), [](const auto &a, const auto &b) { return a.weight > b.weight; });
                    next->words = std::move(merged);
                    job.result = std::move(next);
                } // A concurrent publication is retried on the next poll.
            }
        } catch (...) { job.failed = true; }
        job.done.store(true, std::memory_order_release);
    }
    static DWORD WINAPI worker(void *raw) {
        HMODULE module;
        {
            std::unique_ptr<Start> start(static_cast<Start *>(raw));
            module = start->module;
            load(*start->job);
        } // Destroy all C++ objects before releasing the DLL reference.
        FreeLibraryAndExitThread(module, 0);
    }
public:
    ThemeReload(std::filesystem::path root, std::vector<JapaneseWord> base, std::string active,
                std::filesystem::path personalPath = {}, std::shared_ptr<const PersonalDictionarySnapshot> personal = {},
                std::filesystem::path baseRoot = {})
        : root_(std::move(root)), base_(std::make_shared<const std::vector<JapaneseWord>>(std::move(base))), active_(std::move(active)),
          personalPath_(std::move(personalPath)), personal_(personal ? std::move(personal) : std::make_shared<PersonalDictionarySnapshot>()),
          baseRoot_(std::move(baseRoot)) {}
    // Called only from the owning TSF thread, before handling an actual key.
    // Queries (OnTestKeyDown) must never load or publish a dictionary.
    bool poll(bool boundary, std::vector<JapaneseWord> &words, ULONGLONG now = GetTickCount64()) {
        return poll(boundary, words, [](auto &) { return false; }, now);
    }
    // adopt must prepare all sessions before changing service state. On success
    // it swaps ownership without throwing; words and all generation markers
    // below then publish as one operation on the owning TSF thread.
    template<class Adopt>
    bool poll(bool boundary, std::vector<JapaneseWord> &words, Adopt adopt, ULONGLONG now) {
        bool changed = false;
        if (job_ && job_->done.load(std::memory_order_acquire) && boundary) {
            failed_ = job_->failed;
            try {
                if (!failed_ && job_->result) {
                    if (job_->preparedBase) {
                        if (!adopt(job_->preparedBase)) throw std::runtime_error("base session replacement failed");
                        base_.swap(job_->preparedWords);
                        activeBase_.swap(job_->baseGeneration);
                    }
                    words.swap(job_->result->words);
                    active_.swap(job_->result->generation);
                    personal_ = job_->personal;
                    changed = true;
                }
            } catch (...) { failed_ = true; }
            job_.reset();
        }
        if (!job_ && now >= nextPoll_) {
            nextPoll_ = now + 1000;
            HMODULE workerModule = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&worker), &workerModule)) return changed;
            try {
                auto job = std::make_shared<Job>();
                job->root = root_; job->base = base_; job->previous = active_;
                job->personalPath = personalPath_; job->previousPersonal = personal_;
                job->baseRoot = baseRoot_; job->previousBase = activeBase_;
                auto start = std::make_unique<Start>(Start{job, workerModule});
                HANDLE thread = CreateThread(nullptr, 0, worker, start.get(), 0, nullptr);
                if (thread) { start.release(); CloseHandle(thread); job_ = std::move(job); }
                else { FreeLibrary(workerModule); failed_ = true; }
            } catch (...) { FreeLibrary(workerModule); failed_ = true; }
        }
        return changed;
    }
    bool failed() const { return failed_; }
    const std::string &activeGeneration() const { return active_; }
    const std::string &activeBaseGeneration() const { return activeBase_; }
    // Only read worker results after its release/acquire handoff. This is an
    // observation from a real key, not a promise that an idle process refreshed.
    std::string diagnosticFields() const {
        std::string phase = failed_ ? "failed" : "idle", pending;
        if (job_) {
            if (!job_->done.load(std::memory_order_acquire)) phase = "loading";
            else if (job_->failed) phase = "failed";
            else if (job_->result) { phase = "waiting_boundary"; pending = job_->baseGeneration; }
            else phase = "idle";
        }
        using base_dictionary::Metadata;
        return "\"base_generation\":" + Metadata::quote(activeBase_) +
            ",\"theme_generation\":" + Metadata::quote(active_) +
            ",\"pending_base_generation\":" + Metadata::quote(pending) +
            ",\"phase\":" + Metadata::quote(phase) +
            ",\"last_apply_failed\":" + (failed_ ? "true" : "false");
    }
private:
    std::filesystem::path root_;
    std::shared_ptr<const std::vector<JapaneseWord>> base_;
    std::shared_ptr<Job> job_;
    std::string active_;
    std::filesystem::path personalPath_;
    std::shared_ptr<const PersonalDictionarySnapshot> personal_;
    std::filesystem::path baseRoot_;
    std::string activeBase_;
    ULONGLONG nextPoll_ = 0;
    bool failed_ = false;
};
