"""第二阶段活用原型：显式词性，表记和读音同步；尚未接入 TSF。"""
from dataclasses import dataclass


@dataclass(frozen=True)
class Form:
    name: str
    text: str
    reading: str


# 词尾：未然、连用、假定、意向、て、た。词性必须明确，不猜测「る」。
GODAN = {
    "う": ("わ", "い", "え", "お", "って", "った"),
    "く": ("か", "き", "け", "こ", "いて", "いた"),
    "ぐ": ("が", "ぎ", "げ", "ご", "いで", "いだ"),
    "す": ("さ", "し", "せ", "そ", "して", "した"),
    "つ": ("た", "ち", "て", "と", "って", "った"),
    "ぬ": ("な", "に", "ね", "の", "んで", "んだ"),
    "ぶ": ("ば", "び", "べ", "ぼ", "んで", "んだ"),
    "む": ("ま", "み", "め", "も", "んで", "んだ"),
    "る": ("ら", "り", "れ", "ろ", "って", "った"),
}


def _forms(word: str, pos: str, irregular: str) -> dict[str, str]:
    result = {"辞书形": word}
    if pos == "ichidan":
        if not word.endswith("る"):
            raise ValueError("一段动词必须以る结尾")
        stem = word[:-1]
        endings = {"ます形": "ます", "て形": "て", "た形": "た", "ない形": "ない",
                   "可能形": "られる", "被动形": "られる", "使役形": "させる",
                   "意向形": "よう", "ば形": "れば"}
    elif pos == "godan":
        if word[-1:] not in GODAN:
            raise ValueError("五段动词词尾不合法")
        stem = word[:-1]
        a, i, e, o, te, ta = GODAN[word[-1]]
        if irregular == "iku":
            if word[-1] != "く":
                raise ValueError("行く例外必须以く结尾")
            te, ta = "って", "った"
        endings = {"ます形": i + "ます", "て形": te, "た形": ta, "ない形": a + "ない",
                   "可能形": e + "る", "被动形": a + "れる", "使役形": a + "せる",
                   "意向形": o + "う", "ば形": e + "ば"}
        if irregular == "aru":
            if not word.endswith(("ある", "有る", "在る")):
                raise ValueError("ある例外需要ある／有る／在る词尾")
            prefix = word[:-2]
            result["ない形"] = prefix + "ない"
    elif pos == "suru":
        if not word.endswith("する"):
            raise ValueError("する动词必须以する结尾")
        stem = word[:-2]
        endings = {"ます形": "します", "て形": "して", "た形": "した", "ない形": "しない",
                   "可能形": "できる", "被动形": "される", "使役形": "させる",
                   "意向形": "しよう", "ば形": "すれば"}
    elif pos == "kuru":
        if word.endswith("来る"):
            stem = word[:-1]
            endings = {"ます形": "ます", "て形": "て", "た形": "た", "ない形": "ない",
                       "可能形": "られる", "被动形": "られる", "使役形": "させる",
                       "意向形": "よう", "ば形": "れば"}
        elif word.endswith("くる"):
            stem = word[:-2]
            endings = {"ます形": "きます", "て形": "きて", "た形": "きた", "ない形": "こない",
                       "可能形": "こられる", "被动形": "こられる", "使役形": "こさせる",
                       "意向形": "こよう", "ば形": "くれば"}
        else:
            raise ValueError("来る动词必须以来る／くる结尾")
    elif pos == "i-adjective":
        if not word.endswith("い"):
            raise ValueError("い形容词必须以い结尾")
        stem = word[:-1]
        if irregular == "ii":
            if word.endswith("いい"):
                stem = word[:-2] + "よ"
            elif word.endswith(("良い", "よい")):
                stem = word[:-1]
            else:
                raise ValueError("いい例外需要いい／良い／よい词尾")
        endings = {"丁宁形": "いです", "て形": "くて", "た形": "かった", "ない形": "くない", "ば形": "ければ"}
        result["丁宁形"] = word + "です"
    elif pos == "na-adjective":
        stem = word
        endings = {"丁宁形": "です", "连体形": "な", "て形": "で", "た形": "だった", "ない形": "ではない", "条件形": "なら"}
    else:
        raise ValueError("未知词性；请显式指定一段、五段、する、来る或形容词")
    for name, ending in endings.items():
        result.setdefault(name, stem + ending)
    if irregular == "aru":
        # 存在义「ある」不机械生成可能、被动和使役。
        for name in ("可能形", "被动形", "使役形"):
            result.pop(name, None)
    return result


def conjugate(text: str, reading: str, pos: str, *, irregular: str = "") -> list[Form]:
    """na-adjective 输入词干（静か），其余输入辞书形；例外须显式指定。"""
    if not text or not reading or any(c.isspace() for c in text + reading):
        raise ValueError("表记和读音不能为空或包含空白")
    if any(not ("ぁ" <= c <= "ゖ" or c == "ー") for c in reading):
        raise ValueError("读音必须使用平假名")
    allowed = {"": None, "iku": "godan", "aru": "godan", "ii": "i-adjective"}
    if irregular not in allowed or (irregular and allowed[irregular] != pos):
        raise ValueError("例外类型与词性不匹配")
    if pos == "godan" and text[-1] != reading[-1]:
        raise ValueError("表记和读音的五段词尾不一致")
    if irregular == "iku" and reading not in ("いく", "ゆく"):
        raise ValueError("iku 例外仅支持行く／ゆく；复合词须另行审核")
    if irregular == "aru" and reading != "ある":
        raise ValueError("aru 例外仅支持存在义ある")
    # 这些读音必须指定例外，避免静默产生已知错误形式。
    if pos == "godan" and reading in ("いく", "ゆく") and irregular != "iku":
        raise ValueError("行く／ゆく需要显式指定 iku 例外")
    if pos == "godan" and reading == "ある" and irregular != "aru":
        raise ValueError("ある需要显式指定 aru 例外")
    if pos == "i-adjective" and reading == "いい" and irregular != "ii":
        raise ValueError("いい需要显式指定 ii 例外")
    text_forms = _forms(text, pos, irregular)
    reading_forms = _forms(reading, pos, irregular)
    return [Form(name, value, reading_forms[name]) for name, value in text_forms.items()]
