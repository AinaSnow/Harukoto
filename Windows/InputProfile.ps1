$script:HarukotoProfile = '{961C0ADA-4DC6-488F-9563-4FC144EFAE27}{8FDE097D-3513-486A-9AB5-2D1B2C67C359}'
$script:HarukotoTip = '0804:' + $script:HarukotoProfile
$script:HarukotoLegacyTip = '0404:' + $script:HarukotoProfile

function Update-HarukotoLanguageList {
    param([Parameter(Mandatory)]$List, [switch]$Remove, [scriptblock]$CreateLanguage = { (New-WinUserLanguageList 'zh-Hans-CN')[0] })
    $changed = $false
    foreach ($item in $List) {
        foreach ($tip in @($script:HarukotoLegacyTip, $script:HarukotoTip)) {
            if (($Remove -or $tip -eq $script:HarukotoLegacyTip -or $item.LanguageTag -ne 'zh-Hans-CN') -and $item.InputMethodTips.Contains($tip)) {
                $item.InputMethodTips.Remove($tip) | Out-Null
                $changed = $true
            }
        }
    }
    if (!$Remove) {
        $language = $List | Where-Object LanguageTag -eq 'zh-Hans-CN' | Select-Object -First 1
        if (!$language) { $language = & $CreateLanguage; $List.Add($language) | Out-Null; $changed = $true }
        if (!$language.InputMethodTips.Contains($script:HarukotoTip)) { $language.InputMethodTips.Add($script:HarukotoTip) | Out-Null; $changed = $true }
    }
    return $changed
}
