"""Build the shared, pinned Meowpuut lexicon on Windows."""
import runpy
import sys
from pathlib import Path

scripts = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(scripts))
compiler = runpy.run_path(str(scripts / "compile-native-lexicon.py"))
compiler["main"](sys.argv[1])

# Keep the shared three-column native table compatible with the other platforms.
# The TSF adapter consumes an enriched Windows-only table instead.
from japanese_dictionary import parse_word_list, serialize
output = Path(sys.argv[1])
native = (output / 'native-japanese.tsv').read_text(encoding='utf-8')
bridge = parse_word_list(scripts.parent / 'Resources/Lexicon/japanese-bridge.txt')
(output / 'japanese.tsv').write_text(native + serialize(bridge), encoding='utf-8')
