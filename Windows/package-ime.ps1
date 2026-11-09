param(
    [string]$Compiler,
    [switch]$MSVC,
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.]+)?$')][string]$Version = '0.4.4-alpha.8'
)

$ErrorActionPreference = 'Stop'
if ($MSVC -and $Compiler) { throw 'Choose -MSVC or -Compiler, not both.' }
if (!$MSVC -and !$Compiler) { throw 'Specify -MSVC or the LLVM-MinGW -Compiler path.' }
$root = Split-Path $PSScriptRoot
$build = Join-Path $root '.build/windows'
$output = Join-Path $build 'dist'
New-Item -ItemType Directory -Force $output | Out-Null
$output = (Resolve-Path -LiteralPath $output).Path
$stage = Join-Path $output ([guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage 'payload'
$licenses = Join-Path $stage 'Licenses'
New-Item -ItemType Directory -Force $payload,$licenses | Out-Null

try {
    foreach ($name in 'MeowpuutTip.dll','native-pinyin.tsv','japanese.tsv','language-evidence.sqlite','japanese-conjugations.bin') {
        Copy-Item -LiteralPath (Join-Path $build "ime/$name") -Destination $payload
    }
    Copy-Item -LiteralPath (Join-Path $build 'MeowpuutGlassPreview.exe') -Destination $payload
    foreach ($name in 'dictionary-manager.pyw','japanese_packages.py','japanese_dictionary.py','ff14-manifest.json','ff14-words.txt','ff14-reviewed-source.json') {
        Copy-Item -LiteralPath (Join-Path $build $name) -Destination $payload
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Dictionaries.cmd') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install.cmd') -Destination $stage
    foreach ($name in 'PowerShellHost.ps1','InstallSupport.ps1','InputProfile.ps1','Settings.cmd','set-candidate-appearance.ps1') {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $stage
    }
    New-Item -ItemType Directory -Force (Join-Path $stage 'Windows'),(Join-Path $stage 'scripts'),(Join-Path $stage 'docs') | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'import-japanese-dictionary.py') -Destination (Join-Path $stage 'Windows')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'manage-japanese-packages.py') -Destination (Join-Path $stage 'Windows')
    Copy-Item -LiteralPath (Join-Path $root 'scripts/japanese_dictionary.py') -Destination (Join-Path $stage 'scripts')
    Copy-Item -LiteralPath (Join-Path $root 'scripts/japanese_packages.py') -Destination (Join-Path $stage 'scripts')
    $example = Join-Path $stage 'Resources/DictionaryPackages/example'
    New-Item -ItemType Directory -Force $example | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'Resources/DictionaryPackages/example/manifest.json'),(Join-Path $root 'Resources/DictionaryPackages/example/words.txt') -Destination $example
    $ff14 = Join-Path $stage 'Resources/DictionaryPackages/ff14/jobs'
    New-Item -ItemType Directory -Force $ff14 | Out-Null
    foreach ($name in 'manifest.json','ff14-words.txt','reviewed-source.json') {
        Copy-Item -LiteralPath (Join-Path $root "Resources/DictionaryPackages/ff14/jobs/$name") -Destination $ff14
    }
    foreach ($name in 'coverage.json','source-checks.json') {
        Copy-Item -LiteralPath (Join-Path $root "Resources/DictionaryPackages/ff14/$name") -Destination $ff14
    }
    foreach ($name in 'USER_GUIDE.md','DICTIONARY_PACKAGES.md','FF14_TERMS.md','FF14_DICTIONARY.md','JAPANESE_INPUT_ROADMAP.md','DEVELOPMENT_STATUS.md','DEVELOPMENT_CHANGELOG.md','WINDOWS_VALIDATION.md','BRAND.md') {
        Copy-Item -LiteralPath (Join-Path $root "docs/$name") -Destination (Join-Path $stage 'docs')
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install-ime.ps1') -Destination (Join-Path $stage 'Install.ps1')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md'),(Join-Path $root 'LICENSE'),(Join-Path $root 'THIRD_PARTY.md') -Destination $stage
    $readme = Join-Path $stage 'README.md'
    (Get-Content -LiteralPath $readme -Raw).Replace('(../docs/','(docs/') | Set-Content -LiteralPath $readme -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/McBopomofo/LICENSE.txt') -Destination (Join-Path $licenses 'McBopomofo-MIT.txt')
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/PinyinPro/LICENSE') -Destination (Join-Path $licenses 'PinyinPro-MIT.txt')
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/UniDic/SOURCE.json') -Destination (Join-Path $licenses 'UniDic-SOURCE.json')
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/McBopomofo/SOURCE.json') -Destination (Join-Path $licenses 'McBopomofo-SOURCE.json')
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/PinyinPro/SOURCE.json') -Destination (Join-Path $licenses 'PinyinPro-SOURCE.json')
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/UniDic/LICENSE.txt') -Destination (Join-Path $licenses 'UniDic-derived-BSD.txt')
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/UniDic/UNIDIC-BSD.txt') -Destination (Join-Path $licenses 'UniDic-BSD.txt')
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/UniDic/UNIDIC-COPYING.txt') -Destination (Join-Path $licenses 'UniDic-COPYING.txt')
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/MimoNormalization/ICU-LICENSE.txt') -Destination (Join-Path $licenses 'ICU-LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/MimoNormalization/ICU-Hans_Hant.txt') -Destination (Join-Path $licenses 'ICU-Hans_Hant.txt')
    Copy-Item -LiteralPath (Join-Path $root 'DataSources/MimoNormalization/SOURCE.json') -Destination (Join-Path $licenses 'MimoNormalization-SOURCE.json')
    if (!$MSVC) {
        $toolchain = Split-Path (Split-Path (Resolve-Path -LiteralPath $Compiler).Path)
        Copy-Item -LiteralPath (Join-Path $toolchain 'LICENSE.TXT') -Destination (Join-Path $licenses 'LLVM-MinGW.txt')
        $mingw = Join-Path $toolchain 'x86_64-w64-mingw32/share/mingw32'
        foreach ($name in 'COPYING.MinGW-w64.txt','COPYING.MinGW-w64-runtime.txt','COPYING.winpthreads.txt') {
            Copy-Item -LiteralPath (Join-Path $mingw $name) -Destination $licenses
        }
    }
    $revision = & git -C $root rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source revision.' }
    $dirty = [bool](& git -C $root status --porcelain)
    $chain = if ($MSVC) { 'MSVC (static runtime)' } else { 'LLVM-MinGW' }
    @(
        '# 构建信息', '', "版本：$Version", "源提交：$revision",
        "包含未提交修改：$dirty", "工具链：$chain",
        '验证：打包时只检查安装文件完整性；功能测试请参阅仓库 docs/DEVELOPMENT_STATUS.md。',
        '构建与打包前请使用同一工具链重新生成全部二进制。本包不代表已完成跨应用兼容性验收。'
    ) | Set-Content -LiteralPath (Join-Path $stage 'BUILD-INFO.md') -Encoding utf8
    & (Join-Path $stage 'Install.ps1') -Prebuilt -VerifyOnly | Out-Null
    $zip = Join-Path $output "Harukoto-$Version-windows-x64.zip"
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
    $ff14Manifest = Get-Content -LiteralPath (Join-Path $ff14 'manifest.json') -Raw | ConvertFrom-Json
    if ($ff14Manifest.version -notmatch '^[0-9]+[.][0-9]+[.][0-9]+$') { throw 'Invalid bundled FF14 package version.' }
    $ff14Zip = Join-Path $output ("Harukoto-FF14-" + $ff14Manifest.version + '.zip')
    Compress-Archive -Path (Join-Path $ff14 '*') -DestinationPath $ff14Zip -Force
    ((Get-FileHash -LiteralPath $ff14Zip).Hash.ToLowerInvariant() + '  ' + (Split-Path $ff14Zip -Leaf)) | Set-Content -LiteralPath (Join-Path $output 'FF14-SHA256SUMS.txt') -Encoding ascii
    $sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ascii -Value "$sha  $(Split-Path $zip -Leaf)"
    Write-Output $zip
} finally {
    $resolvedStage = (Resolve-Path -LiteralPath $stage).Path
    if ([IO.Path]::GetDirectoryName($resolvedStage) -eq $output) {
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
}
