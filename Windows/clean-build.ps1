param([switch]$Apply)
# 默认预览；只清理固定生成目录及旧发行包，保留最新两个 alpha 版本。
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot))
$build = Join-Path $repo '.build'
if (!(Test-Path -LiteralPath $build)) { return }
$boundary = $build + [IO.Path]::DirectorySeparatorChar
$targets = [Collections.Generic.List[string]]::new()
$dist = Join-Path $build 'windows/dist'
if (Test-Path -LiteralPath $dist) {
    $files = @(Get-ChildItem -LiteralPath $dist -File)
    $alphas = @($files | Where-Object { $_.Name -match '^Harukoto-0[.]4[.]4-alpha[.](\d+)-(Setup[.]exe|windows-x64[.]zip)$' } | ForEach-Object {
        [pscustomobject]@{ File=$_; Version=[int]([regex]::Match($_.Name,'alpha[.](\d+)').Groups[1].Value) }
    })
    $keep = @($alphas.Version | Sort-Object -Unique -Descending | Select-Object -First 2)
    foreach ($item in $alphas) { if ($item.Version -notin $keep) { $targets.Add($item.File.FullName) } }
    $themes = @($files | Where-Object { $_.Name -match '^Harukoto-FF14-(\d+[.]\d+[.]\d+)[.]zip$' } | ForEach-Object {
        [pscustomobject]@{ File=$_; Version=[version]([regex]::Match($_.Name,'FF14-(\d+[.]\d+[.]\d+)').Groups[1].Value) }
    })
    $latestTheme = $themes.Version | Sort-Object -Descending | Select-Object -First 1
    foreach ($item in $themes) { if ($item.Version -lt $latestTheme) { $targets.Add($item.File.FullName) } }
    foreach ($file in $files) {
        if ($file.Name -match '^Meowpuut-0[.]4[.]3-japanese[.]\d+-(Setup[.]exe|windows-x64[.]zip)$' -or
            $file.Name -eq 'Harukoto-0.4.4-jp02-windows-x64.zip') { $targets.Add($file.FullName) }
    }
}
foreach ($relative in @('releases/Meowpuut-japanese-stage1-windows-x64',
                        'releases/Meowpuut-japanese-stage1-windows-x64.zip',
                        'releases/Meowpuut-japanese-stage1-windows-x64.sha256',
                        'windows/native', 'ff14-compiled-v020', 'ff14-expansion-v020',
                        'ff14-full-final', 'ff14-full-reproduce', 'ff14-full-v030')) {
    $candidate = Join-Path $build $relative
    if (Test-Path -LiteralPath $candidate) { $targets.Add($candidate) }
}
# Validate every final absolute path before any deletion. Refuse tracked files or links.
$entries = foreach ($target in $targets) {
    $full = (Resolve-Path -LiteralPath $target).Path
    if (!$full.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) { throw "路径越界：$full" }
    for ($part = Get-Item -LiteralPath $full; $part; $part = $part.Parent) {
        if ($part.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "拒绝链接路径：$full" }
        if ($part.FullName -eq $repo) { break }
        if (!$part.PSIsContainer) { $part = $part.Directory; if ($part.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "拒绝链接路径：$full" } }
    }
    $children = @(Get-ChildItem -LiteralPath $full -Recurse -Force)
    if ($children | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) { throw "目录包含链接：$full" }
    $tracked = & git -C $repo ls-files -- $full
    if ($LASTEXITCODE -ne 0 -or $tracked) { throw "无法确认目录不含受版本管理文件：$full" }
    $item = Get-Item -LiteralPath $full
    $bytes = if ($item.PSIsContainer) { ($children | Where-Object { !$_.PSIsContainer } | Measure-Object Length -Sum).Sum } else { $item.Length }
    [pscustomobject]@{ Path=$full; Bytes=[long]$bytes }
}
$entries | Select-Object Path,@{n='MiB';e={[math]::Round($_.Bytes/1MB,2)}} | Format-Table -AutoSize
$total = ($entries | Measure-Object Bytes -Sum).Sum
if ($Apply) {
    foreach ($entry in $entries) { Remove-Item -LiteralPath $entry.Path -Recurse -Force }
    foreach ($entry in $entries) { if (Test-Path -LiteralPath $entry.Path) { throw "清理未完成：$($entry.Path)" } }
}
[pscustomobject]@{ Applied=[bool]$Apply; Targets=@($entries).Count; Bytes=[long]$total; GiB=[math]::Round($total/1GB,3) }
