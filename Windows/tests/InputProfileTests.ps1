$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../InputProfile.ps1')
function Language($tag, $tips) {
    $list = [Collections.Generic.List[string]]::new()
    foreach ($tip in $tips) { $list.Add($tip) }
    return [pscustomobject]@{ LanguageTag = $tag; InputMethodTips = $list }
}
$list = [Collections.Generic.List[object]]::new()
$list.Add((Language 'zh-Hant-TW' @($script:HarukotoLegacyTip, 'other-tw')))
$list.Add((Language 'ja' @('other-ja')))
if (!(Update-HarukotoLanguageList -List $list -CreateLanguage { Language 'zh-Hans-CN' @('microsoft-pinyin') })) { throw 'Migration not detected' }
if ($list.Count -ne 3 -or $list[0].InputMethodTips.Count -ne 1 -or $list[0].InputMethodTips[0] -ne 'other-tw') { throw 'Other language tips changed' }
if (!$list[2].InputMethodTips.Contains($script:HarukotoTip)) { throw 'Simplified Chinese profile missing' }
if (Update-HarukotoLanguageList -List $list) { throw 'Repeated migration must be idempotent' }
if (!(Update-HarukotoLanguageList -List $list -Remove)) { throw 'Uninstall did not remove profile' }
if ($list[2].InputMethodTips.Count -ne 1 -or $list[2].InputMethodTips[0] -ne 'microsoft-pinyin') { throw 'Microsoft Pinyin changed' }
if (Update-HarukotoLanguageList -List $list -Remove) { throw 'Repeated uninstall must be idempotent' }
Write-Output 'PASS: old profile migration, preserving other languages/IMEs, repeat install and uninstall.'
