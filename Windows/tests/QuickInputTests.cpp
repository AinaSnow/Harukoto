// CN-07 batch 6: numbers, dates and times after a "v"; CN-07 symbols.
#include "../QuickInput.h"
#include <cassert>
#include <iostream>
#include <set>

using namespace quick_input;

static std::vector<std::string> texts(const std::vector<Item> &items) {
    std::vector<std::string> result;
    for (const auto &item : items) result.push_back(item.text);
    return result;
}

int main() {
    // Chinese numerals.
    const std::pair<const char *, const char *> lower[] = {
        {"0", "零"}, {"7", "七"}, {"10", "十"}, {"12", "十二"}, {"20", "二十"}, {"100", "一百"}, {"101", "一百零一"},
        {"110", "一百一十"}, {"111", "一百一十一"}, {"1001", "一千零一"}, {"1010", "一千零一十"}, {"1100", "一千一百"},
        {"10000", "一万"}, {"10010", "一万零一十"}, {"100010", "十万零一十"}, {"110000", "十一万"}, {"10000001", "一千万零一"},
        {"100000000", "一亿"}, {"100010000", "一亿零一万"}, {"1000000000000", "一万亿"}, {"2026", "二千零二十六"},
        {"9999999999999999", "九千九百九十九万亿九千九百九十九亿九千九百九十九万九千九百九十九"}, {"007", "七"}, {"0000", "零"}};
    for (const auto &[digits, expected] : lower) assert(chineseInteger(digits, false) == expected);
    assert(chineseInteger("10", true) == "壹拾" && chineseInteger("123", true) == "壹佰贰拾叁");
    assert(chineseInteger("100010", true) == "壹拾万零壹拾" && chineseInteger("0", true) == "零");
    assert(chineseInteger("10000000000000000", false).empty()); // 17 digits: no units left
    assert(chineseInteger("", false).empty() && chineseInteger("1.5", false).empty() && chineseInteger("a", false).empty());
    // Digit by digit, years.
    assert(chineseDigits("2026", true) == "二〇二六" && chineseDigits("2026") == "二零二六" && chineseDigits("1.05") == "一点零五");
    // Thousands.
    assert(grouped("123") == "123" && grouped("1234") == "1,234" && grouped("1234567.89") == "1,234,567.89");
    // Amounts.
    const std::pair<const char *, const char *> amounts[] = {
        {"0", "零元整"}, {"1", "壹元整"}, {"10", "壹拾元整"}, {"123", "壹佰贰拾叁元整"}, {"100.5", "壹佰元伍角整"},
        {"100.50", "壹佰元伍角整"}, {"1.05", "壹元零伍分"}, {"12.34", "壹拾贰元叁角肆分"}, {"0.5", "伍角整"}, {"0.05", "伍分"},
        {"0.00", "零元整"}, {"10000", "壹万元整"}, {"1001.1", "壹仟零壹元壹角整"}};
    for (const auto &[number, expected] : amounts) assert(chineseAmount(number) == expected);
    assert(chineseAmount("1.234").empty() && chineseAmount("10000000000000000").empty());
    // Typing.
    assert(accepts("", '1') && accepts("", '0') && !accepts("", '.') && accepts("1", '.') && !accepts("1.5", '.'));
    assert(!accepts(std::string(MaxNumber, '9'), '9') && !accepts("1", 'a'));
    // Every form of a number, in order.
    using Texts = std::vector<std::string>;
    assert((texts(numberCandidates("123")) == Texts{"一百二十三", "壹佰贰拾叁", "壹佰贰拾叁元整", "一二三", "123"}));
    assert((texts(numberCandidates("1234.5")) ==
        Texts{"一千二百三十四点五", "壹仟贰佰叁拾肆点伍", "壹仟贰佰叁拾肆元伍角整", "一二三四点五", "1234.5", "1,234.5"}));
    assert((texts(numberCandidates("0")) == Texts{"零", "零元整", "0"}));
    assert((texts(numberCandidates("12.")) == texts(numberCandidates("12")))); // a trailing point waits for a decimal
    const auto labels = numberCandidates("10.123");
    assert(labels.size() == 4 && labels[0].text == "十点一二三" && labels[0].label == "中文数字" && labels[1].label == "大写");
    assert(labels[2].label == "逐位" && labels[3].text == "10.123"); // no amount below the fen
    const auto huge = texts(numberCandidates("12345678901234567"));
    assert((huge == Texts{"一二三四五六七八九零一二三四五六七", "12345678901234567", "12,345,678,901,234,567"}));
    assert(numberCandidates("").empty() && numberCandidates("1a").empty());
    // Weekdays.
    assert(weekday(2026, 11, 20) == 5 && weekday(2000, 1, 1) == 6 && weekday(2024, 2, 29) == 4 && weekday(2026, 3, 1) == 0);
    // Dates and times from a fixed moment.
    const Moment afternoon{2026, 11, 20, 14, 30, 5};
    assert(trigger("vrq") && trigger("vsj") && trigger("vxq") && !trigger("rq") && !trigger("v") && !trigger("vrqa"));
    assert((texts(dateCandidates("vrq", afternoon)) == Texts{"2026年11月20日", "2026-11-20", "2026/11/20", "11月20日",
        "二〇二六年十一月二十日", "2026年11月20日 星期五"}));
    assert((texts(dateCandidates("vsj", afternoon)) == Texts{"14:30", "14:30:05", "下午2点30分", "14时30分", "2026-11-20 14:30"}));
    assert((texts(dateCandidates("vxq", afternoon)) == Texts{"星期五", "周五", "礼拜五"}));
    assert(dateCandidates("vrq", afternoon)[0].label == "日期");
    const Moment morning{2027, 1, 3, 9, 5, 0}, noon{2027, 1, 3, 12, 0, 0}, night{2027, 1, 3, 0, 7, 0}, evening{2027, 1, 3, 20, 0, 0};
    assert(texts(dateCandidates("vsj", morning))[2] == "上午9点05分" && texts(dateCandidates("vsj", noon))[2] == "中午12点");
    assert(texts(dateCandidates("vsj", night))[2] == "凌晨0点07分" && texts(dateCandidates("vsj", evening))[2] == "晚上8点");
    assert(texts(dateCandidates("vrq", morning))[1] == "2027-01-03" && texts(dateCandidates("vrq", morning))[4] == "二〇二七年一月三日");
    assert((texts(dateCandidates("vxq", morning)) == Texts{"星期日", "周日", "礼拜天"}));
    assert(dateCandidates("vrq", Moment{}).empty() && dateCandidates("rq", afternoon).empty());
    // Symbols: small curated lists, distinct codes that are not date triggers.
    const auto &categories = symbolCategories();
    assert(categories.size() == 9);
    std::set<std::string> codes;
    size_t total = 0;
    for (const auto &category : categories) {
        const std::string code = category.code;
        assert(code.size() == 3 && code[0] == 'v' && code != "vfh" && !trigger(code) && codes.insert(code).second);
        assert(std::string(category.name).size() > 0 && category.symbols.size() >= 10 && category.symbols.size() <= 45);
        std::set<std::string> seen;
        for (const char *symbol : category.symbols) {
            const std::string text = symbol;
            assert(!text.empty() && text.size() <= 6 && text.find_first_of(" \t\r\n") == std::string::npos);
            assert(seen.insert(text).second); // no duplicate within a category
        }
        assert(symbolTrigger(code));
        const auto items = symbolCandidates(code);
        assert(items.size() == category.symbols.size() && items.front().label == category.name);
        total += items.size();
    }
    assert(total >= 150 && total <= 260);
    // The first symbols of each list.
    const std::pair<const char *, Texts> heads[] = {
        {"vbd", {"，", "。", "、", "；", "："}}, {"vsx", {"±", "×", "÷", "≈", "≠", "≤", "≥", "∞", "√", "°"}},
        {"vjt", {"←", "→", "↑", "↓"}}, {"vxh", {"①", "②", "③", "④", "⑤", "⑥", "⑦"}}, {"vdw", {"℃", "℉", "‰", "￥"}},
        {"vtx", {"★", "☆", "○", "●", "◎", "◇", "◆", "□", "■", "△", "▲"}}, {"vxl", {"α", "β", "γ"}},
        {"vkh", {"「", "」", "『", "』", "【", "】"}}, {"vts", {"※", "§", "№", "™", "©", "®"}}};
    for (const auto &[code, expected] : heads) {
        const auto all = texts(symbolCandidates(code));
        assert(all.size() >= expected.size() && std::equal(expected.begin(), expected.end(), all.begin()));
    }
    // vfh: every category in order, each symbol once, labelled with its code.
    const auto everything = symbolCandidates("vfh");
    assert(symbolTrigger("vfh") && everything.front().text == "，" && everything.front().label == "标点 · vbd");
    std::set<std::string> distinct;
    for (const auto &item : everything) assert(distinct.insert(item.text).second);
    for (const auto &category : categories)
        for (const char *symbol : category.symbols) assert(distinct.count(symbol));
    assert(everything.size() < total && everything.back().text == categories.back().symbols.back());
    assert(std::find_if(everything.begin(), everything.end(), [](const Item &i) { return i.text == "①"; })->label == "序号 · vxh");
    // Only exact triggers.
    for (const char *other : {"", "v", "vb", "vbdd", "bd", "fh", "vrq", "vfhx", "vbd1", "VBD"})
        assert(!symbolTrigger(other) && symbolCandidates(other).empty());
    std::cout << "PASS: Chinese numerals, capitals, amounts, digits, grouping, typing limits, dates, times, weekdays and symbols\n";
}
