#pragma once
#include <windows.h>
#include "JapanesePackages.h"
#include <atomic>
#include <memory>

// The worker owns immutable inputs and never calls COM or touches Service.
// It holds a module reference so service teardown does not wait for disk IO,
// and the DLL cannot unload while its worker code is still executing.
class ThemeReload {
    friend struct ThemeReloadTests;
    struct Job {
        std::filesystem::path root;
        std::shared_ptr<const std::vector<JapaneseWord>> base;
        std::string previous;
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
            const auto current = generation(job.root);
            if (current.empty()) {
                // No theme configuration on a fresh installation is normal.
                job.failed = !job.previous.empty() || std::filesystem::exists(job.root / "CURRENT");
            } else if (current != job.previous) {
                auto next = readJapanesePackageSnapshot(job.root);
                if (!next) job.failed = true;
                else if (next->generation == current && generation(job.root) == current) {
                    auto merged = *job.base;
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
    ThemeReload(std::filesystem::path root, std::vector<JapaneseWord> base, std::string active)
        : root_(std::move(root)), base_(std::make_shared<const std::vector<JapaneseWord>>(std::move(base))), active_(std::move(active)) {}
    // Called only from the owning TSF thread, before handling an actual key.
    // Queries (OnTestKeyDown) must never load or publish a dictionary.
    bool poll(bool boundary, std::vector<JapaneseWord> &words, ULONGLONG now = GetTickCount64()) {
        bool changed = false;
        if (job_ && job_->done.load(std::memory_order_acquire) && boundary) {
            failed_ = job_->failed;
            if (job_->result) {
                words.swap(job_->result->words);
                active_ = job_->result->generation;
                changed = true;
            }
            job_.reset();
        }
        if (!job_ && now >= nextPoll_) {
            nextPoll_ = now + 1000;
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&worker), &module)) return changed;
            try {
                auto job = std::make_shared<Job>();
                job->root = root_; job->base = base_; job->previous = active_;
                auto start = std::make_unique<Start>(Start{job, module});
                HANDLE thread = CreateThread(nullptr, 0, worker, start.get(), 0, nullptr);
                if (thread) { start.release(); CloseHandle(thread); job_ = std::move(job); }
                else { FreeLibrary(module); failed_ = true; }
            } catch (...) { FreeLibrary(module); failed_ = true; }
        }
        return changed;
    }
    bool failed() const { return failed_; }
    const std::string &activeGeneration() const { return active_; }
private:
    std::filesystem::path root_;
    std::shared_ptr<const std::vector<JapaneseWord>> base_;
    std::shared_ptr<Job> job_;
    std::string active_;
    ULONGLONG nextPoll_ = 0;
    bool failed_ = false;
};
