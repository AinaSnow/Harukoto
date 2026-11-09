#pragma once
#include "JapaneseConjugation.h"
#include "MixedComposition.h"

// Phase 2 supports whole-input explicit triggers only. No partial reading is consumed.
inline void appendConjugationWords(const JapaneseConjugations &table, std::vector<JapaneseWord> &words) {
    for (const auto &entry : table.entries())
        for (const auto &trigger : entry.triggers)
            words.push_back({trigger.second, entry.text, 5000, entry.reading, trigger.first, ""});
}

inline const JapaneseConjugationEntry *conjugationFor(const JapaneseConjugations &table,
        const WindowsCandidate &choice, const std::string &input) {
    if (!choice.japanese || choice.coreIndex) return nullptr;
    const auto split = choice.annotation.find(" · ");
    const auto reading = choice.annotation.substr(0, split);
    const auto matches = table.findWord(choice.text, reading);
    const JapaneseConjugationEntry *result = nullptr;
    for (const auto *entry : matches) {
        bool triggered = false;
        for (const auto &trigger : entry->triggers) if (trigger.second == input) triggered = true;
        if (triggered) {
            if (result) return nullptr; // Do not guess between ambiguous lexical entries.
            result = entry;
        }
    }
    return result;
}

class ConjugationPanel {
public:
    bool active() const { return !original.empty(); }
    bool open(const JapaneseConjugations &table, std::vector<WindowsCandidate> &choices,
              const std::string &input, size_t &page, size_t &highlighted, bool &expanded) {
        if (active()) return false;
        const size_t index = page * 7 + highlighted;
        if (index >= choices.size()) return false;
        const auto *entry = conjugationFor(table, choices[index], input);
        if (!entry) return false;
        std::vector<WindowsCandidate> forms;
        for (const auto &form : entry->forms)
            forms.push_back({form.text, {}, true, false, form.reading + " · " + form.name});
        if (forms.empty()) return false;
        original = std::move(choices); savedPage = page; savedHighlight = highlighted; savedExpanded = expanded;
        choices = std::move(forms); page = highlighted = 0; expanded = true;
        return true;
    }
    void close(std::vector<WindowsCandidate> &choices, size_t &page, size_t &highlighted, bool &expanded) {
        if (!active()) return;
        choices = std::move(original); original.clear();
        page = savedPage; highlighted = savedHighlight; expanded = savedExpanded;
    }
    void clear() { original.clear(); }
private:
    std::vector<WindowsCandidate> original;
    size_t savedPage = 0, savedHighlight = 0;
    bool savedExpanded = false;
};
