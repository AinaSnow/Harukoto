#pragma once
#include <string>
#include <utility>
#include <vector>

// CN-07 batch 6: numbers, dates and times typed after a "v" (no pinyin
// syllable starts with v). Pure formatting, UTF-8 out, no clock or Windows
// dependency so every case can be tested directly.
//
//   v + digits [. digits]  ->  一百二十三, 壹佰贰拾叁, 壹佰贰拾叁元整, 一二三, 123, 1,234
//   vrq / vsj / vxq        ->  the date, the time, the weekday (local time)
//
// Candidates are generated, never learned, and never touch the dictionaries.
namespace quick_input {

// One generated candidate: the text to commit and a short label shown on
// the candidate's reading line.
struct Item { std::string text, label; };

constexpr size_t MaxNumber = 24;        // Characters after the v.
constexpr size_t MaxChineseDigits = 16; // Integer digits that still have units (千万亿).

// Whether c may extend the number typed so far: digits up to MaxNumber, and
// one decimal point after at least one digit.
inline bool accepts(const std::string &number, char c) {
    if (number.size() >= MaxNumber) return false;
    if (c >= '0' && c <= '9') return true;
    return c == '.' && !number.empty() && number.find('.') == std::string::npos;
}

namespace detail {
inline const char *const lowerDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
inline const char *const upperDigits[] = {"零", "壹", "贰", "叁", "肆", "伍", "陆", "柒", "捌", "玖"};
inline const char *const lowerUnits[] = {"", "十", "百", "千"};
inline const char *const upperUnits[] = {"", "拾", "佰", "仟"};
inline const char *const groupUnits[] = {"", "万", "亿", "万亿"};

inline std::string trimZeros(const std::string &digits) {
    const auto first = digits.find_first_not_of('0');
    return first == std::string::npos ? std::string("0") : digits.substr(first);
}
} // namespace detail

// A non-negative integer of at most MaxChineseDigits digits in Chinese
// numerals: 0 零, 10 十 (壹拾 in capitals), 101 一百零一, 1010 一千零一十,
// 100010 十万零一十, 100000000 一亿. Empty for anything else.
inline std::string chineseInteger(const std::string &digits, bool upper) {
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return {};
    const std::string value = detail::trimZeros(digits);
    if (value.size() > MaxChineseDigits) return {};
    const auto *names = upper ? detail::upperDigits : detail::lowerDigits;
    const auto *units = upper ? detail::upperUnits : detail::lowerUnits;
    if (value == "0") return names[0];
    std::string result;
    bool zero = false; // A zero waits until a non-zero digit follows it.
    const size_t groups = (value.size() + 3) / 4;
    const std::string padded = std::string(groups * 4 - value.size(), '0') + value;
    for (size_t group = 0; group < groups; ++group) {
        const std::string part = padded.substr(group * 4, 4);
        if (part == "0000") { if (!result.empty()) zero = true; continue; }
        for (size_t i = 0; i < 4; ++i) {
            const int digit = part[i] - '0';
            if (!digit) { if (!result.empty()) zero = true; continue; }
            if (zero) { result += names[0]; zero = false; }
            result += names[digit];
            result += units[3 - i];
        }
        result += detail::groupUnits[groups - 1 - group];
    }
    // 一十二 is read 十二 at the start of a number; capitals keep 壹拾.
    if (!upper && result.compare(0, 6, "一十") == 0) result.erase(0, 3);
    return result;
}

// Digit by digit: 2026 二〇二六 (year style, zero as 〇) or 一二三.
inline std::string chineseDigits(const std::string &digits, bool yearZero = false) {
    std::string result;
    for (char c : digits) {
        if (c >= '0' && c <= '9') result += c == '0' && yearZero ? "〇" : detail::lowerDigits[c - '0'];
        else if (c == '.') result += "点";
        else return {};
    }
    return result;
}

// 1234567.5 -> 1,234,567.5
inline std::string grouped(const std::string &number) {
    const auto point = number.find('.');
    const std::string integer = number.substr(0, point), rest = point == std::string::npos ? std::string() : number.substr(point);
    std::string result;
    for (size_t i = 0; i < integer.size(); ++i) {
        if (i && (integer.size() - i) % 3 == 0) result += ',';
        result += integer[i];
    }
    return result + rest;
}

// Amount in capitals to the fen, following the usual rules: 元整 / 角整,
// 零 for a missing 角 before 分, no 元 part below one yuan. Empty when there
// are more than two decimals or the integer is too long.
inline std::string chineseAmount(const std::string &number) {
    const auto point = number.find('.');
    const std::string integer = number.substr(0, point);
    const std::string decimals = point == std::string::npos ? std::string() : number.substr(point + 1);
    if (decimals.size() > 2) return {};
    const std::string yuan = chineseInteger(integer, true);
    if (yuan.empty()) return {};
    const int jiao = decimals.size() > 0 ? decimals[0] - '0' : 0, fen = decimals.size() > 1 ? decimals[1] - '0' : 0;
    const bool whole = detail::trimZeros(integer) != "0";
    std::string result = whole || (!jiao && !fen) ? yuan + "元" : std::string();
    if (!jiao && !fen) return result + "整";
    if (jiao) result += std::string(detail::upperDigits[jiao]) + "角";
    else if (whole) result += "零";
    if (fen) result += std::string(detail::upperDigits[fen]) + "分";
    else result += "整";
    return result;
}

// Every form of a typed number, most common first. A trailing decimal point
// is ignored until a decimal follows it.
inline std::vector<Item> numberCandidates(std::string number) {
    if (!number.empty() && number.back() == '.') number.pop_back();
    if (number.empty() || number.find_first_not_of("0123456789.") != std::string::npos) return {};
    const auto point = number.find('.');
    const std::string integer = number.substr(0, point), decimals = point == std::string::npos ? std::string() : number.substr(point + 1);
    std::vector<Item> items;
    const auto add = [&](std::string text, const char *label) {
        if (text.empty()) return;
        for (const auto &item : items) if (item.text == text) return;
        items.push_back({std::move(text), label});
    };
    const auto decimal = [&](bool upper) {
        if (decimals.empty()) return std::string();
        std::string result = "点";
        for (char c : decimals) result += (upper ? detail::upperDigits : detail::lowerDigits)[c - '0'];
        return result;
    };
    const std::string lower = chineseInteger(integer, false), upper = chineseInteger(integer, true);
    if (!lower.empty()) add(lower + decimal(false), "中文数字");
    if (!upper.empty()) add(upper + decimal(true), "大写");
    add(chineseAmount(number), "金额");
    add(chineseDigits(number), "逐位");
    add(number, "数字");
    if (integer.size() > 3) add(grouped(number), "千分位");
    return items;
}

// A calendar moment in local time.
struct Moment { int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0; };

// 0 = Sunday, for the Gregorian calendar (Sakamoto's method).
inline int weekday(int year, int month, int day) {
    static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) year -= 1;
    return (year + year / 4 - year / 100 + year / 400 + offsets[(month + 11) % 12] + day) % 7;
}

inline std::string two(int value) { return (value < 10 ? "0" : "") + std::to_string(value); }

// The exact triggers. Checked against the bundled and installed data: no
// pinyin reading starts with v, and none of the FF14 English triggers that
// do (1,221 in the current snapshot) is or starts with vrq, vsj or vxq.
inline bool trigger(const std::string &input) { return input == "vrq" || input == "vsj" || input == "vxq"; }

inline std::vector<Item> dateCandidates(const std::string &input, const Moment &now) {
    std::vector<Item> items;
    if (now.year < 1 || now.month < 1 || now.month > 12 || now.day < 1 || now.day > 31) return items;
    static const char *const names[] = {"日", "一", "二", "三", "四", "五", "六"};
    const std::string week = names[weekday(now.year, now.month, now.day)];
    const std::string year = std::to_string(now.year), month = std::to_string(now.month), day = std::to_string(now.day);
    if (input == "vrq") {
        items.push_back({year + "年" + month + "月" + day + "日", "日期"});
        items.push_back({year + "-" + two(now.month) + "-" + two(now.day), "日期"});
        items.push_back({year + "/" + two(now.month) + "/" + two(now.day), "日期"});
        items.push_back({month + "月" + day + "日", "日期"});
        items.push_back({chineseDigits(year, true) + "年" + chineseInteger(month, false) + "月" + chineseInteger(day, false) + "日", "日期"});
        items.push_back({year + "年" + month + "月" + day + "日 星期" + week, "日期"});
    } else if (input == "vsj") {
        const int twelve = now.hour <= 12 ? now.hour : now.hour - 12; // 凌晨0点, 中午12点, 下午2点
        const char *part = now.hour < 6 ? "凌晨" : now.hour < 12 ? "上午" : now.hour < 13 ? "中午" : now.hour < 18 ? "下午" : "晚上";
        items.push_back({two(now.hour) + ":" + two(now.minute), "时间"});
        items.push_back({two(now.hour) + ":" + two(now.minute) + ":" + two(now.second), "时间"});
        items.push_back({std::string(part) + std::to_string(twelve) + "点" + (now.minute ? two(now.minute) + "分" : std::string()), "时间"});
        items.push_back({std::to_string(now.hour) + "时" + two(now.minute) + "分", "时间"});
        items.push_back({year + "-" + two(now.month) + "-" + two(now.day) + " " + two(now.hour) + ":" + two(now.minute), "日期时间"});
    } else if (input == "vxq") {
        items.push_back({"星期" + week, "星期"});
        items.push_back({"周" + week, "星期"});
        items.push_back({std::string("礼拜") + (week == "日" ? "天" : week), "星期"});
    }
    return items;
}

} // namespace quick_input
