#requires -Version 7.0
# DICT-03: the built-in dictionary manager. Compiles tests/DictionaryManagerTests.cs
# with the manager sources and runs it in temporary folders only. With -Reader
# the input service's C++ reader follows the snapshots this manager writes; with
# -Python the output is compared byte for byte with scripts/japanese_packages.py.
param([string]$Reader, [string]$Python)
$ErrorActionPreference = 'Stop'
$windows = Split-Path $PSScriptRoot
$root = Split-Path $windows
$folder = Join-Path ([IO.Path]::GetTempPath()) ('harukoto-dictionary-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder | Out-Null
function Assert($condition, $message) { if (!$condition) { throw $message } }
try {
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe'
    $wpf = Join-Path (Split-Path $compiler) 'WPF'
    $references = 'WindowsBase','PresentationCore','PresentationFramework','System.Xaml' |
        ForEach-Object { '/reference:' + $(if ($_ -eq 'System.Xaml') { 'System.Xaml.dll' } else { Join-Path $wpf "$_.dll" }) }
    $test = Join-Path $folder 'DictionaryManagerTests.exe'
    & $compiler /nologo /target:exe /out:$test $references (Join-Path $windows 'ThemePackages.cs') (Join-Path $windows 'DictionaryManager.cs') (Join-Path $windows 'DictionaryCli.cs') (Join-Path $PSScriptRoot 'DictionaryManagerTests.cs')
    Assert ($LASTEXITCODE -eq 0) 'test build failed'
    & $test $root
    Assert ($LASTEXITCODE -eq 0) 'dictionary manager tests failed'

    function New-Package($key, $version, $words) {
        $directory = Join-Path $folder "$key-$version"
        New-Item -ItemType Directory -Force $directory | Out-Null
        [IO.File]::WriteAllText((Join-Path $directory 'words.txt'), "# 测试`n$words`n", [Text.UTF8Encoding]::new($false))
        $manifest = [ordered]@{ format = 1; id = $key; name = '测试主题'; version = [string]$version; source = '人工'; license = 'MIT'; words = 'words.txt' }
        [IO.File]::WriteAllText((Join-Path $directory 'manifest.json'), ($manifest | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
        Join-Path $directory 'manifest.json'
    }
    function Invoke-Change($store, $operation, $value) {
        & $test --change $store $operation $value
        Assert ($LASTEXITCODE -eq 0) "change $operation $value failed"
    }

    # Command line (MeowpuutGlassPreview.exe --dictionaries ...), against a temporary root.
    $cli = Join-Path $folder 'cli-store'
    function Invoke-Cli { $script:cliOutput = (& $test --dictionaries @args 2>&1) -join "`n"; $LASTEXITCODE }
    $example = Join-Path $root 'Resources/DictionaryPackages/example/manifest.json'
    Assert ((Invoke-Cli --root $cli list) -eq 0 -and $cliOutput -match '尚未导入主题包') 'list on an empty store'
    Assert ((Invoke-Cli --root $cli preview $example) -eq 0 -and $cliOutput -match '仅预览' -and !(Test-Path -LiteralPath $cli)) 'preview does not write'
    Assert ((Invoke-Cli --root $cli import $example) -eq 0 -and $cliOutput -match 'harukoto-example \| .+ \| 关闭 \| 回退：无') 'import from the command line'
    Assert ((Invoke-Cli --root $cli enable harukoto-example) -eq 0 -and $cliOutput -match '\| 启用 \|') 'enable from the command line'
    Assert ((Invoke-Cli --root $cli rollback harukoto-example) -eq 2 -and $cliOutput -match '错误：此主题包没有可回退版本') 'errors exit with 2'
    Assert ((Invoke-Cli --root $cli frobnicate x) -eq 2 -and $cliOutput -match '用法') 'unknown command shows usage'
    $personal = Join-Path $folder 'personal.tsv'
    $words = Join-Path $folder 'personal.txt'
    [IO.File]::WriteAllText($words, "# 个人`nコーヒー | english: coffee`n", [Text.UTF8Encoding]::new($false))
    Assert ((Invoke-Cli --output $personal personal $words --check) -eq 0 -and !(Test-Path -LiteralPath $personal) -and $cliOutput -match '校验通过：1 组') 'check does not write'
    Assert ((Invoke-Cli --output $personal personal $words) -eq 0 -and [IO.File]::ReadAllText($personal) -eq "coffee|コーヒー|5000|こーひー|english|`n") 'personal import'
    Assert ((Invoke-Cli --output $personal personal) -eq 2 -and [IO.File]::ReadAllText($personal).Length -gt 0) 'personal needs a file or --clear'
    Assert ((Invoke-Cli --output $personal personal --clear) -eq 0 -and [IO.File]::ReadAllText($personal).Length -eq 0) 'personal clear'

    if ($Reader) {
        # The C++ runtime reader tracks every snapshot this manager publishes.
        $store = Join-Path $folder 'reader-store'
        function Check-Count($count) {
            & $Reader $store $count
            Assert ($LASTEXITCODE -eq 0) "C++ reader expected $count rows"
        }
        Check-Count 0
        Invoke-Change $store import (New-Package example '1.0' '日本 | にほん | pinyin: riben'); Check-Count 0
        Invoke-Change $store enable example; Check-Count 1
        Invoke-Change $store import (New-Package second '1.0' '東京 | とうきょう | pinyin: dongjing')
        Invoke-Change $store enable second; Check-Count 2
        Invoke-Change $store import (New-Package example '2.0' '学校 | がっこう | pinyin: xuexiao'); Check-Count 2
        Invoke-Change $store rollback example; Check-Count 2
        Invoke-Change $store disable second; Check-Count 1
        Invoke-Change $store disable example; Check-Count 0
    }

    if ($Python -and (Test-Path -LiteralPath $Python -PathType Leaf)) {
        # Byte-for-byte parity with the Python reference implementation.
        $check = Join-Path $folder 'parity.py'
        @'
import json, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from japanese_dictionary import parse_word_list, serialize
from japanese_packages import compiled, read_state
mode = sys.argv[2]
if mode == 'words':
    expected = serialize(parse_word_list(sys.argv[3])).encode('utf-8')
    actual = Path(sys.argv[4]).read_bytes()
    if expected != actual:
        sys.exit('word list differs: ' + sys.argv[3])
else:
    store = Path(sys.argv[3])
    state = read_state(store)
    generation = (store / 'CURRENT').read_text(encoding='ascii')
    metadata = (json.dumps(state, ensure_ascii=False, sort_keys=True, indent=2) + '\n').encode('utf-8')
    if (store / generation / 'state.json').read_bytes() != metadata:
        sys.exit('state.json differs')
    if (store / generation / 'enabled.tsv').read_bytes() != compiled(state):
        sys.exit('enabled.tsv differs')
'@ | Set-Content -LiteralPath $check -Encoding utf8NoBOM
        $scripts = Join-Path $root 'scripts'
        $tricky = Join-Path $folder 'tricky.txt'
        $content = "﻿# 边界`r`n" +
            "新宿｜しんじゅく｜pinyin：Xīn Sù｜priority: 9000`r`n" +
            "  # 注释`r`n" +
            "新宿 | にいじゅく | pinyin: xin-su | pinyin: XINSU`n" +
            "ポイントカード | english: point card | english: point-card | chinese: 积分卡`n" +
            "女性 | じょせい | pinyin: nǚ xìng | pos: n`n" +
            "リムサ・ロミンサ | - | pinyin: limusa luominsa | priority: 1`n" +
            "𠮷野家 | よしのや | pinyin: jiyejia | chinese: 𠮷野家`n" +
            "書く | かく | pos: v5k | pinyin: shu`n"
        [IO.File]::WriteAllText($tricky, $content, [Text.UTF8Encoding]::new($false))
        foreach ($words in $tricky, (Join-Path $root 'Resources/Lexicon/japanese-bridge.txt'),
                (Join-Path $root 'Resources/DictionaryPackages/example/words.txt'),
                (Join-Path $root 'Resources/DictionaryPackages/ff14/full/ff14-words.txt')) {
            $out = Join-Path $folder 'serialized.tsv'
            & $test --serialize $words $out
            Assert ($LASTEXITCODE -eq 0) "C# parse failed: $words"
            & $Python -X utf8 -I $check $scripts words $words $out
            Assert ($LASTEXITCODE -eq 0) "word list parity failed: $words"
        }
        # A store written by this manager, read back by the Python tool.
        $store = Join-Path $folder 'parity-store'
        Invoke-Change $store import (Join-Path $root 'Resources/DictionaryPackages/example/manifest.json')
        Invoke-Change $store import (Join-Path $root 'Resources/DictionaryPackages/ff14/full/manifest.json')
        $trickyPackage = New-Package tricky '1.0' ((Get-Content -LiteralPath $tricky -Encoding utf8 | Select-Object -Skip 1) -join "`n")
        Invoke-Change $store import $trickyPackage
        $ids = @('tricky') + (@('example', 'ff14/full') | ForEach-Object {
            (Get-Content -LiteralPath (Join-Path $root "Resources/DictionaryPackages/$_/manifest.json") -Raw | ConvertFrom-Json).id })
        foreach ($id in $ids) { Invoke-Change $store enable $id }
        & $Python -X utf8 -I $check $scripts store $store
        Assert ($LASTEXITCODE -eq 0) 'store parity failed'
        # And the reverse: a Python change on this store is read by the manager.
        & $Python -X utf8 -I (Join-Path $windows 'manage-japanese-packages.py') --root $store disable tricky | Out-Null
        Assert ($LASTEXITCODE -eq 0) 'Python tool cannot change the store'
        Invoke-Change $store enable tricky
        & $Python -X utf8 -I $check $scripts store $store
        Assert ($LASTEXITCODE -eq 0) 'store parity failed after a Python change'
    } else {
        Write-Output 'Python not given: parity checks skipped.'
    }
    Write-Output 'dictionary manager checks passed'
} finally {
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
}
