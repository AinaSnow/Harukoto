"""Derive sentence-composition statistics from openly licensed sentence corpora.

Development-time tool. It reads sentence collections (Leipzig Corpora
Collection archives, Tatoeba exports), segments every sentence with the
project's own compiled vocabulary and counts words and adjacent word pairs.
Only the resulting statistics are written to the repository; no sentence text
is kept. See DataSources/SentenceCorpora.

Two tables are produced:

- collocations.tsv.gz: adjacent word pairs with a discounted pointwise mutual
  information score. "^" and "$" stand for the start and end of a phrase
  (line start/end or punctuation), so sentence-final particles and typical
  openers carry evidence too.
- word-priors.tsv.gz: how frequent each word is in running text, on the same
  scale as the lexicon's own weights. The lexicon compiler turns the gap
  between the two into a correction that applies only while composing
  sentences; standalone homophone order is not affected.

Each --pool is one register (for example formal web/news text, or short
conversational sentences). Pair scores are estimated inside each pool and the
larger one is kept, so a small conversational pool is not drowned out by tens
of millions of formal tokens. Word frequencies are mixed with --spoken-share.
"""
import argparse
import bz2
import gzip
import hashlib
import json
import math
import multiprocessing
import re
import statistics
import tarfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HAN = re.compile('[一-鿿]+')
PIECE = re.compile('([一-鿿]+)')
SPACED = re.compile('(?<=[一-鿿]) (?=[一-鿿])')
# A phrase boundary is counted only next to these, or at the ends of a line;
# digits, Latin text and unknown characters break a run without being one.
STOPS = frozenset('，。！？；：、,.!?;:…「」『』“”"()（）《》')
BEGIN, END = '^', '$'
MAX_WORD = 8
VOCABULARY = {}


def vocabulary(lexicon):
    """Simplified word -> decoder score, from the compiled pinyin table."""
    words = {}
    with (lexicon / 'native-pinyin.tsv').open(encoding='utf-8') as source:
        next(source)
        for line in source:
            if line.startswith('# context'):
                break
            fields = line.rstrip('\n').split('\t')
            if len(fields) != 6:
                raise ValueError('expected a version 2 pinyin table')
            word, weight = fields[3], int(fields[5])
            if len(word) <= MAX_WORD and HAN.fullmatch(word):
                words[word] = max(words.get(word, 0), weight)
    # The decoder's own word score: fewer, more frequent words win.
    return {word: weight / 700.0 - 19.0 for word, weight in words.items()}


def segment(run):
    """Best-scoring split of a run in which every character is a known word."""
    best = [0.0] + [-math.inf] * len(run)
    back = [0] * (len(run) + 1)
    for end in range(1, len(run) + 1):
        for start in range(max(0, end - MAX_WORD), end):
            score = VOCABULARY.get(run[start:end])
            if score is not None and best[start] + score > best[end]:
                best[end], back[end] = best[start] + score, start
    words, end = [], len(run)
    while end:
        words.append(run[back[end]:end])
        end = back[end]
    return words[::-1]


def start(words):
    global VOCABULARY
    VOCABULARY = words


def count(lines):
    single, pairs, runs, skipped = Counter(), Counter(), 0, 0
    for line in lines:
        # The sentence is the last field (Leipzig: id, text; Tatoeba: id, language, text).
        parts = PIECE.split(line.rstrip('\r\n').rsplit('\t', 1)[-1].strip())
        # parts alternates: other text, Han text, other text, ...
        for index in range(1, len(parts), 2):
            han, before, after = parts[index], parts[index - 1], parts[index + 1]
            opens = not before or before[-1] in STOPS
            closes = not after or after[0] in STOPS
            # A character outside the vocabulary (mostly traditional text in
            # these corpora) ends the run; nothing is counted across it.
            run = ''
            for position, character in enumerate(han + '\0'):
                if character in VOCABULARY:
                    run += character
                    continue
                skipped += character != '\0'
                if run and len(run) <= 200:
                    words = segment(run)
                    runs += 1
                    single.update(words)
                    pairs.update(a + '\t' + b for a, b in zip(words, words[1:]))
                    if opens and position == len(run):
                        pairs[BEGIN + '\t' + words[0]] += 1
                        single[BEGIN] += 1
                    if closes and character == '\0':
                        pairs[words[-1] + '\t' + END] += 1
                        single[END] += 1
                run = ''
    return single, pairs, runs, skipped


def lines_of(archive):
    if archive.name.endswith('.bz2'):
        with bz2.open(archive, 'rt', encoding='utf-8', errors='replace') as source:
            yield from source
        return
    with tarfile.open(archive, 'r:gz') as tar:
        members = [m for m in tar.getmembers() if m.name.endswith('-sentences.txt')]
        if len(members) != 1:
            raise ValueError(f'{archive.name}: expected exactly one sentences file')
        for raw in tar.extractfile(members[0]):
            yield raw.decode('utf-8', 'replace')


def sentences(archive, batch=20000):
    """Batches of lines. Some collections ship pre-tokenised text with a space
    between words; those spaces are removed so that this project's own
    segmentation decides the word boundaries."""
    lines, spaced = [], None
    for line in lines_of(archive):
        lines.append(line)
        if len(lines) == batch:
            if spaced is None:
                text = ''.join(lines[:2000])
                spaced = len(SPACED.findall(text)) * 4 > len(HAN.findall(text))
            yield [SPACED.sub('', item) for item in lines] if spaced else lines
            lines = []
    if lines:
        if spaced is None:
            text = ''.join(lines[:2000])
            spaced = len(SPACED.findall(text)) * 4 > len(HAN.findall(text))
        yield [SPACED.sub('', item) for item in lines] if spaced else lines


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def tokens_of(single):
    return sum(value for word, value in single.items() if word not in (BEGIN, END))


def select(single, pairs, minimum, floor):
    """Positive, count-discounted pointwise mutual information, in hundredths of a nat."""
    total = tokens_of(single)
    rows = {}
    for key, joint in pairs.items():
        if joint < minimum:
            continue
        left, right = key.split('\t')
        pmi = math.log(joint * total / (single[left] * single[right]))
        # Rare pairs are unreliable; shrink them towards no association.
        score = pmi * joint / (joint + 3)
        if score >= floor:
            rows[(left, right)] = (min(999, round(score * 100)), joint)
    return rows


def write(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with gzip.GzipFile(path, 'wb', mtime=0) as raw:
        raw.write(f'# {header}由 scripts/build-collocations.py 生成。\n'.encode())
        for row in rows:
            raw.write(('\t'.join(map(str, row)) + '\n').encode())


def main():
    parser = argparse.ArgumentParser(description='从署名许可语料统计相邻词搭配与词频')
    parser.add_argument('--pool', action='append', nargs='+', type=Path, required=True, metavar='ARCHIVE',
                        help='同一语体的一组语料；可重复给出多组，最后一组视为口语')
    parser.add_argument('--lexicon', type=Path, default=ROOT / '.build/windows/ime')
    parser.add_argument('--output', type=Path, default=ROOT / 'Resources/Lexicon/collocations.tsv.gz')
    parser.add_argument('--priors', type=Path, default=ROOT / 'Resources/Lexicon/word-priors.tsv.gz')
    parser.add_argument('--report', type=Path, default=ROOT / 'DataSources/SentenceCorpora/collocations-report.json')
    parser.add_argument('--limit', type=int, default=1000000, help='保留的搭配数量')
    parser.add_argument('--floor', type=float, default=0.3, help='保留搭配的最低分数（奈特）')
    parser.add_argument('--spoken-share', type=float, default=0.3, help='词频中最后一组语料所占比例')
    parser.add_argument('--evidence', type=int, default=20, help='写入词频所需的最少出现次数')
    parser.add_argument('--exclude-modulus', type=int, nargs='*', default=[],
                        help='按组给出 N：跳过 crc32 能被 N 整除的行，留作开发集，仅用于调参')
    parser.add_argument('--processes', type=int, default=max(1, multiprocessing.cpu_count() - 2))
    args = parser.parse_args()
    if len(args.pool) < 2:
        parser.error('需要至少两组语料：书面语在前，口语在后')
    if args.exclude_modulus and len(args.exclude_modulus) != len(args.pool):
        parser.error('--exclude-modulus 的数量必须与 --pool 相同')
    words = vocabulary(args.lexicon)
    merged, pools, singles = {}, [], []
    with multiprocessing.Pool(args.processes, start, (words,)) as pool:
        for number, archives in enumerate(args.pool):
            modulus = args.exclude_modulus[number] if args.exclude_modulus else 0
            single, pairs, runs, skipped, pruned = Counter(), Counter(), 0, 0, False
            for archive in archives:
                batches = sentences(archive)
                if modulus:
                    import zlib
                    batches = ([line for line in batch if zlib.crc32(line.encode()) % modulus] for batch in batches)
                for one, two, seen, lost in pool.imap_unordered(count, batches):
                    single.update(one)
                    pairs.update(two)
                    runs += seen
                    skipped += lost
                    if len(pairs) > 12_000_000:
                        # Bound memory: a pair seen once so far is unlikely to reach the minimum.
                        pairs = Counter({key: value for key, value in pairs.items() if value > 1})
                        pruned = True
            tokens = tokens_of(single)
            # A small pool cannot support the same count threshold as a large one.
            minimum = 3 if tokens >= 10_000_000 else 2
            rows = select(single, pairs, minimum, args.floor)
            for key, (score, joint) in rows.items():
                if key not in merged or merged[key][0] < score:
                    merged[key] = (score, joint)
            singles.append((single, tokens))
            pools.append(dict(archives={a.name: dict(sha256=sha(a), bytes=a.stat().st_size) for a in archives},
                              runs=runs, characters_outside_vocabulary=skipped, tokens=tokens,
                              phrase_starts=single[BEGIN], phrase_ends=single[END], excluded_modulus=modulus,
                              distinct_pairs=len(pairs), singletons_pruned=pruned, minimum=minimum, selected=len(rows)))
            print([a.name for a in archives], 'tokens', tokens, 'selected', len(rows), flush=True)
    # Keep the strongest, best-attested pairs when the table must be cut.
    kept = sorted(merged.items(), key=lambda row: -row[1][0] * math.log1p(row[1][1]))[:args.limit]
    kept.sort(key=lambda row: (row[0][0].encode(), row[0][1].encode()))
    write(args.output, '相邻词搭配：简体左词<TAB>简体右词<TAB>分数（百分之一奈特，折扣后的点互信息）；^ 与 $ 表示短语的开头和结尾。',
          [(left, right, score) for (left, right), (score, _) in kept])

    # Word frequency on the lexicon's scale. The lexicon's scores sit a roughly
    # constant distance below corpus log-probability; measure that distance on
    # well-attested words of the largest pool instead of assuming it.
    formal = [item for item in singles[:-1]]
    formal_single, formal_tokens = Counter(), 0
    for single, tokens in formal:
        formal_single.update(single)
        formal_tokens += tokens
    spoken_single, spoken_tokens = singles[-1]
    offset = statistics.mean(math.log(n / formal_tokens) - words[word]
                             for word, n in formal_single.items() if n >= 50 and word in words)
    priors = []
    for word in words:
        seen = formal_single.get(word, 0) + spoken_single.get(word, 0)
        if seen < args.evidence:
            continue
        mixed = (args.spoken_share * spoken_single.get(word, 0) / spoken_tokens +
                 (1 - args.spoken_share) * formal_single.get(word, 0) / formal_tokens)
        priors.append((word, round((math.log(mixed) - offset) * 100), seen))
    priors.sort(key=lambda row: row[0].encode())
    write(args.priors, '连续文本中的词频：简体词<TAB>分数（百分之一奈特，与词库权重除以 700 减 19 同一刻度）<TAB>出现次数。', priors)

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(dict(
        vocabulary=len(words), pinyin_table_sha256=sha(args.lexicon / 'native-pinyin.tsv'), pools=pools,
        candidates=len(merged), limit=args.limit, floor=args.floor, written=len(kept),
        boundary_pairs=sum(1 for (left, right), _ in kept if left == BEGIN or right == END),
        output_sha256=sha(args.output), spoken_share=args.spoken_share, evidence=args.evidence,
        lexicon_offset=round(offset, 4), priors_written=len(priors), priors_sha256=sha(args.priors),
        note='搭配分数在每组语料内分别估计，同一搭配取较大值；singletons_pruned 为真时该组丢弃过只出现一次的搭配，低频计数略偏低。'
             'excluded_modulus 非零表示该次运行留出了开发句子，正式数据应为 0。'),
        ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('written', len(kept), args.output, len(priors), args.priors, 'offset', round(offset, 4))


if __name__ == '__main__':
    main()
