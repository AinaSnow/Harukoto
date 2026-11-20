#requires -Version 7.0
# 选词记忆（CN-06）管理：查看、导出、清除、关闭与开启。只处理当前用户的数据。
# 同时覆盖选词记录（selection-memory.tsv）和个人词频（selection-memory-words.tsv，CN-06c），
# 以及手动添加的个人词（selection-memory-user.tsv，DICT-02b）：添加、修订、删除、导出、导入、回退。
[CmdletBinding(DefaultParameterSetName = 'Show')]
param(
    [Parameter(ParameterSetName = 'Show')][switch]$Show,
    [Parameter(ParameterSetName = 'Show')][Parameter(ParameterSetName = 'ListWords')][ValidateRange(1, 100000)][int]$Top = 50,
    [Parameter(ParameterSetName = 'Export', Mandatory)][string]$Export,
    [Parameter(ParameterSetName = 'Clear', Mandatory)][switch]$Clear,
    [Parameter(ParameterSetName = 'Disable', Mandatory)][switch]$Disable,
    [Parameter(ParameterSetName = 'Enable', Mandatory)][switch]$Enable,
    # DICT-02b：个人词。读音可写 zhang san、zhang'san、zhangsan、zhang1 san1 或 zhāng sān。
    [Parameter(ParameterSetName = 'ListWords', Mandatory)][switch]$ListWords,
    [Parameter(ParameterSetName = 'AddWord', Mandatory)][string]$AddWord,
    [Parameter(ParameterSetName = 'ReviseWord', Mandatory)][string]$ReviseWord,
    [Parameter(ParameterSetName = 'RemoveWord', Mandatory)][string]$RemoveWord,
    [Parameter(ParameterSetName = 'AddWord', Mandatory)][Parameter(ParameterSetName = 'ReviseWord', Mandatory)]
    [Parameter(ParameterSetName = 'RemoveWord', Mandatory)][string]$Text,
    [Parameter(ParameterSetName = 'AddWord')][Parameter(ParameterSetName = 'ReviseWord')][AllowEmptyString()][string]$Traditional,
    [Parameter(ParameterSetName = 'ReviseWord')][string]$NewReading,
    [Parameter(ParameterSetName = 'ReviseWord')][string]$NewText,
    [Parameter(ParameterSetName = 'ExportWords', Mandatory)][string]$ExportWords,
    [Parameter(ParameterSetName = 'ImportWords', Mandatory)][string]$ImportWords,
    [Parameter(ParameterSetName = 'ImportWords')][switch]$Replace,
    [Parameter(ParameterSetName = 'UndoWords', Mandatory)][switch]$UndoWords,
    [Parameter(ParameterSetName = 'ClearWords', Mandatory)][switch]$ClearWords,
    # 仅供测试覆盖默认位置。个人词频和个人词文件固定在选词记录旁边。
    [string]$Path = (Join-Path $env:LOCALAPPDATA 'Meowpuut/selection-memory.tsv'),
    [string]$RegistryPath = 'HKCU:\Software\Meowpuut\Input',
    # 仅供测试固定“现在”（自 1970 年起的整小时数）。
    [long]$Hour = 0
)
$ErrorActionPreference = 'Stop'
$WordsPath = Join-Path (Split-Path -LiteralPath $Path) ([IO.Path]::GetFileNameWithoutExtension($Path) + '-words' + [IO.Path]::GetExtension($Path))
$UserPath = Join-Path (Split-Path -LiteralPath $Path) ([IO.Path]::GetFileNameWithoutExtension($Path) + '-user' + [IO.Path]::GetExtension($Path))
# 回退用的上一版本。输入法不读这个文件。
$UserPreviousPath = Join-Path (Split-Path -LiteralPath $Path) ([IO.Path]::GetFileNameWithoutExtension($Path) + '-user.previous' + [IO.Path]::GetExtension($Path))
if ($Hour -le 0) { $Hour = [long][Math]::Floor([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() / 3600) }

# ---- DICT-02b：个人词 ----
# 与输入法相同的限制（SelectionMemory.h 的 UserWordModel，核心 mpn_personal_add_word）。
$MaxUserWords = 10000
$MaxReadingBytes = 128
$MaxTextBytes = 192
$MaxSyllables = 16
# 拼音音节表（ü 写作 v），与词库里有单字的音节一致。
$Syllables = [Collections.Generic.HashSet[string]]::new([string[]](@(
    'a ai an ang ao ba bai ban bang bao bei ben beng bi bian biao bie bin bing bo bu ca cai can cang cao ce cen ceng cha chai chan chang chao che chen cheng chi chong chou chu chua chuai chuan chuang chui chun chuo ci cong cou cu cuan cui cun cuo' -split ' ') +
    @('da dai dan dang dao de dei den deng di dia dian diao die ding diu dong dou du duan dui dun duo e ei en eng er fa fan fang fei fen feng fo fou fu ga gai gan gang gao ge gei gen geng gong gou gu gua guai guan guang gui gun guo ha hai han hang hao he hei hen heng hong hou hu hua huai huan huang hui hun huo' -split ' ') +
    @('ji jia jian jiang jiao jie jin jing jiong jiu ju juan jue jun ka kai kan kang kao ke kei ken keng kong kou ku kua kuai kuan kuang kui kun kuo la lai lan lang lao le lei leng li lia lian liang liao lie lin ling liu long lou lu luan lue lun luo lv lve ma mai man mang mao me mei men meng mi mian miao mie min ming miu mo mou mu' -split ' ') +
    @('na nai nan nang nao ne nei nen neng ni nian niang niao nie nin ning niu nong nou nu nuan nue nun nuo nv nve o ou pa pai pan pang pao pei pen peng pi pia pian piao pie pin ping po pou pu qi qia qian qiang qiao qie qin qing qiong qiu qu quan que qun ran rang rao re ren reng ri rong rou ru ruan rui run ruo' -split ' ') +
    @('sa sai san sang sao se sen seng sha shai shan shang shao she shei shen sheng shi shou shu shua shuai shuan shuang shui shun shuo si song sou su suan sui sun suo ta tai tan tang tao te teng ti tian tiao tie ting tong tou tu tuan tui tun tuo wa wai wan wang wei wen weng wo wu' -split ' ') +
    @('xi xia xian xiang xiao xie xin xing xiong xiu xu xuan xue xun ya yan yang yao ye yi yin ying yo yong you yu yuan yue yun za zai zan zang zao ze zei zen zeng zha zhai zhan zhang zhao zhe zhei zhen zheng zhi zhong zhou zhu zhua zhuai zhuan zhuang zhui zhun zhuo zi zong zou zu zuan zui zun zuo' -split ' ')))
$ToneMarks = @{
    [char]0x0101 = 'a1'; [char]0x00E1 = 'a2'; [char]0x01CE = 'a3'; [char]0x00E0 = 'a4'
    [char]0x0113 = 'e1'; [char]0x00E9 = 'e2'; [char]0x011B = 'e3'; [char]0x00E8 = 'e4'
    [char]0x012B = 'i1'; [char]0x00ED = 'i2'; [char]0x01D0 = 'i3'; [char]0x00EC = 'i4'
    [char]0x014D = 'o1'; [char]0x00F3 = 'o2'; [char]0x01D2 = 'o3'; [char]0x00F2 = 'o4'
    [char]0x016B = 'u1'; [char]0x00FA = 'u2'; [char]0x01D4 = 'u3'; [char]0x00F9 = 'u4'
    [char]0x01D6 = 'v1'; [char]0x01D8 = 'v2'; [char]0x01DA = 'v3'; [char]0x01DC = 'v4'; [char]0x00FC = 'v0'
}

# 把用户写的读音整理成 zhang'san 这样以撇号分隔的音节，可带声调数字。
# 一段连写的字母拆成最少的音节，个数相同时第一个音节取最长（xian 是一个音节，
# 想要 xi'an 请写撇号或空格）。返回 $null 并在 $script:ReadingError 里说明原因。
function ConvertTo-Reading([string]$Value) {
    $script:ReadingError = $null
    $chunks = [Collections.Generic.List[object]]::new()
    $current = $null
    foreach ($c in $Value.Trim().ToLowerInvariant().Replace('u:', 'v').ToCharArray()) {
        if ($c -ge [char]'a' -and $c -le [char]'z') {
            if ($null -eq $current) { $current = @{ Letters = ''; Tones = @{}; End = 0 } }
            $current.Letters += $c
        } elseif ($ToneMarks.ContainsKey($c)) {
            if ($null -eq $current) { $current = @{ Letters = ''; Tones = @{}; End = 0 } }
            $mark = $ToneMarks[$c]
            $current.Letters += $mark[0]
            if ($mark[1] -ne [char]'0') { $current.Tones[$current.Letters.Length - 1] = [string]$mark[1] }
        } elseif ($c -ge [char]'1' -and $c -le [char]'5') {
            if ($null -eq $current) { $script:ReadingError = '声调数字前面没有拼音'; return $null }
            $current.End = [string]$c
            $chunks.Add($current); $current = $null
        } elseif ($c -eq [char]' ' -or $c -eq [char]"'" -or $c -eq [char]'-' -or $c -eq [char]0x2019) {
            if ($null -ne $current) { $chunks.Add($current); $current = $null }
        } else {
            $script:ReadingError = "读音里有不认识的字符「$c」"; return $null
        }
    }
    if ($null -ne $current) { $chunks.Add($current) }
    if ($chunks.Count -eq 0) { $script:ReadingError = '读音是空的'; return $null }
    $parts = [Collections.Generic.List[string]]::new()
    foreach ($chunk in $chunks) {
        $letters = $chunk.Letters
        $n = $letters.Length
        # 从后往前：best[i] 为 letters[i..] 最少能拆成几个音节。
        $best = [int[]]::new($n + 1); $next = [int[]]::new($n + 1)
        for ($i = 0; $i -lt $n; $i++) { $best[$i] = [int]::MaxValue }
        for ($i = $n - 1; $i -ge 0; $i--) {
            for ($size = [Math]::Min(6, $n - $i); $size -ge 1; $size--) {
                if ($best[$i + $size] -eq [int]::MaxValue -or $best[$i + $size] + 1 -ge $best[$i]) { continue }
                if ($Syllables.Contains($letters.Substring($i, $size))) { $best[$i] = $best[$i + $size] + 1; $next[$i] = $size }
            }
        }
        if ($best[0] -eq [int]::MaxValue) { $script:ReadingError = "「$letters」拆不成拼音音节"; return $null }
        for ($i = 0; $i -lt $n; $i += $next[$i]) {
            $end = $i + $next[$i]
            $tones = @($chunk.Tones.Keys | Where-Object { $_ -ge $i -and $_ -lt $end } | ForEach-Object { $chunk.Tones[$_] })
            if ($end -eq $n -and $chunk.End) { $tones += $chunk.End }
            $tones = @($tones | Select-Object -Unique)
            if ($tones.Count -gt 1) { $script:ReadingError = "音节 $($letters.Substring($i, $next[$i])) 有两个声调"; return $null }
            $parts.Add($letters.Substring($i, $next[$i]) + $(if ($tones.Count) { $tones[0] } else { '' }))
        }
    }
    if ($parts.Count -gt $MaxSyllables) { $script:ReadingError = "最多 $MaxSyllables 个音节"; return $null }
    $reading = $parts -join "'"
    if ([Text.Encoding]::UTF8.GetByteCount($reading) -gt $MaxReadingBytes) { $script:ReadingError = '读音太长'; return $null }
    return $reading
}

# 读音去掉声调和撇号，是个人词的“身份”（和简体文字一起）。
function Get-ReadingKey([string]$Reading) { return ($Reading -replace "[1-5']", '') }

function Test-WordText([string]$Value, [switch]$Optional) {
    if (!$Value) { return [bool]$Optional }
    if ($Value -match '[\x00-\x1f\x7f-\x9f]') { return $false }
    # Lone surrogates do not survive UTF-8.
    $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
    return $bytes.Length -le $MaxTextBytes -and [Text.Encoding]::UTF8.GetString($bytes) -ceq $Value
}

# 与输入法相同的读音格式检查：字母、紧跟字母的声调数字、不连续的撇号。
function Test-StoredReading([string]$Value) {
    return $Value -cmatch "^[a-z1-5']{1,$MaxReadingBytes}$" -and $Value -cmatch '[a-z]' -and $Value -cnotmatch "(^|[^a-z])[1-5]|(^|')'"
}

# 与输入法相同的读法：坏行逐行跳过；同一读音（去声调）和文字的后一行更新前一行。
function Read-UserWords([string]$File = $UserPath, [switch]$Strict) {
    $table = [Collections.Specialized.OrderedDictionary]::new([StringComparer]::Ordinal)
    $number = 0
    foreach ($line in Read-Lines $File) {
        ++$number
        if ($number -eq 1 -and $line.Length -and $line[0] -eq [char]0xFEFF) { $line = $line.Substring(1) }
        if (!$line -or $line.StartsWith('#')) { continue }
        $fields = $line.Split("`t")
        $reading = $fields[0]
        $valid = $fields.Count -ge 2 -and $fields.Count -le 3 -and (Test-StoredReading $reading) -and (Test-WordText $fields[1]) -and
            ($fields.Count -lt 3 -or (Test-WordText $fields[2] -Optional))
        if (!$valid) {
            if ($Strict) { throw "第 $number 行格式不对：$line" }
            continue
        }
        $key = (Get-ReadingKey $reading) + "`t" + $fields[1]
        $entry = [pscustomobject]@{ Reading = $reading; Text = $fields[1]; Traditional = $(if ($fields.Count -eq 3) { $fields[2] } else { '' }) }
        if ($table.Contains($key)) { $table[$key].Reading = $entry.Reading; $table[$key].Traditional = $entry.Traditional }
        elseif ($table.Count -lt $MaxUserWords) { $table[$key] = $entry }
        elseif ($Strict) { throw "个人词最多 $MaxUserWords 个" }
    }
    return $table
}

function Format-UserWords($Table) {
    $builder = [Text.StringBuilder]::new()
    [void]$builder.Append("# 晴词个人词表（DICT-02b）。每行：读音<TAB>简体[<TAB>繁体]。请用 manage-selection-memory.ps1 修改。`n")
    foreach ($entry in $Table.Values) {
        [void]$builder.Append($entry.Reading).Append("`t").Append($entry.Text)
        if ($entry.Traditional) { [void]$builder.Append("`t").Append($entry.Traditional) }
        [void]$builder.Append("`n")
    }
    return $builder.ToString()
}

# 原子替换。输入法每隔约 2 秒短暂打开表文件读取（不到 1 毫秒），那一刻替换会被拒绝，稍后重试。
function Move-Replacing([string]$Source, [string]$Target) {
    for ($attempt = 1; ; $attempt++) {
        try { [IO.File]::Move($Source, $Target, $true); return }
        catch [UnauthorizedAccessException], [IO.IOException] {
            if ($attempt -ge 50) { Remove-Item -LiteralPath $Source -ErrorAction SilentlyContinue; throw }
            Start-Sleep -Milliseconds 100
        }
    }
}

# 整表写入：先把当前版本存为上一版本（供 -UndoWords 回退），再写临时文件并原子替换，
# 输入法看到的总是完整的一份。各应用在约 2 秒内的下一次按键时采用新表。
function Save-UserWords($Table) {
    $folder = Split-Path -LiteralPath $UserPath
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $encoding = [Text.UTF8Encoding]::new($false)
    $previous = if (Test-Path -LiteralPath $UserPath) { [IO.File]::ReadAllText($UserPath, $encoding) } else { '' }
    $temporary = Join-Path $folder ('.selection-memory-user-' + [guid]::NewGuid().ToString('N') + '.tmp')
    [IO.File]::WriteAllText($temporary, $previous, $encoding)
    Move-Replacing $temporary $UserPreviousPath
    [IO.File]::WriteAllText($temporary, (Format-UserWords $Table), $encoding)
    Move-Replacing $temporary $UserPath
}

function Resolve-WordArguments([string]$ReadingText, [string]$WordText) {
    $reading = ConvertTo-Reading $ReadingText
    if (!$reading) { throw "读音不对：$ReadingText（$script:ReadingError）" }
    if (!(Test-WordText $WordText)) { throw "文字不对：不能为空、不能有控制字符，最多 $MaxTextBytes 字节（UTF-8）" }
    return $reading
}

function Read-Lines([string]$File) {
    if (!(Test-Path -LiteralPath $File)) { return }
    $stream = [IO.File]::Open($File, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite,Delete')
    try {
        $reader = [IO.StreamReader]::new($stream, [Text.UTF8Encoding]::new($false))
        while ($null -ne ($line = $reader.ReadLine())) { $line }
    } finally { $stream.Dispose() }
}

function Read-Memory {
    # 与输入服务相同的规则：逐行累计，次数封顶为 8，坏行跳过。
    $entries = [ordered]@{}
    $order = 0
    foreach ($line in Read-Lines $Path) {
        $fields = $line.Split("`t")
        # CN-07 第五批：“-”行是在候选中忘记的选词，删除之前的记录（之后再选会重新记住）。
        if ($fields.Count -eq 4 -and $fields[0] -eq '-' -and $fields[1] -in 's', 't') {
            $entries.Remove($fields[1] + "`t" + $fields[2] + "`t" + $fields[3])
            continue
        }
        if ($fields.Count -ne 4 -or $fields[0] -notmatch '^[0-9]{1,2}$' -or [int]$fields[0] -lt 1 -or
            $fields[1] -notin 's', 't' -or $fields[2] -notmatch "^[a-z']{1,128}$" -or !$fields[3]) { continue }
        $key = $fields[1] + "`t" + $fields[2] + "`t" + $fields[3]
        $count = [int]$fields[0]
        if ($entries.Contains($key)) { $count += $entries[$key].Count }
        $entries[$key] = [pscustomobject]@{
            Script = if ($fields[1] -eq 's') { '简体' } else { '繁体' }
            Reading = $fields[2]; Text = $fields[3]; Count = [Math]::Min(8, $count); Order = ++$order
        }
    }
    return $entries
}

function Read-Words {
    # 个人词频，规则同输入服务（SelectionMemory.h 的 WordModel）：每次确认加 1、
    # 封顶 20；同读音的其他词减半；每 14 天减半；低于 0.05 即遗忘。
    $groups = @{}
    $order = 0
    $age = {
        param($entry, [long]$at)
        if ($at -gt $entry.Hour) { $entry.Weight *= [Math]::Pow(2, - ($at - $entry.Hour) / 336.0); $entry.Hour = $at }
    }
    foreach ($line in Read-Lines $WordsPath) {
        $fields = $line.Split("`t")
        if ($fields.Count -ne 5 -or $fields[1] -notin 's', 't' -or $fields[2] -notmatch '^[a-z]{1,128}$' -or !$fields[3] -or
            $fields[3] -match '[\x00-\x1f\x7f]' -or $fields[4] -notmatch '^[0-9]{1,9}$' -or [long]$fields[4] -lt 1) { continue }
        $at = [long]$fields[4]
        $group = $fields[1] + "`t" + $fields[2]
        if ($fields[0] -eq '-') {
            # CN-07 第五批：在候选中忘记的词，权重清零。
            if ($groups.ContainsKey($group)) {
                $groups[$group].Remove($fields[3])
                if ($groups[$group].Count -eq 0) { $groups.Remove($group) }
            }
            continue
        }
        if ($fields[0] -eq '+') { $amount = 1.0; $selection = $true }
        elseif ($fields[0] -match '^=([0-9]{1,9})$' -and [long]$Matches[1] -ge 1 -and [long]$Matches[1] -le 2000) { $amount = [long]$Matches[1] / 100.0; $selection = $false }
        else { continue }
        $group = $fields[1] + "`t" + $fields[2]
        if (!$groups.ContainsKey($group)) { $groups[$group] = [ordered]@{} }
        $texts = $groups[$group]
        if ($selection) {
            foreach ($other in @($texts.Keys)) {
                if ($other -ceq $fields[3]) { continue }
                & $age $texts[$other] $at
                $texts[$other].Weight /= 2
            }
        }
        if (!$texts.Contains($fields[3])) {
            if ($texts.Count -ge 8) {
                $weakest = $texts.Keys | Sort-Object { $e = $texts[$_]; $e.Weight * [Math]::Pow(2, - [Math]::Max(0, $at - $e.Hour) / 336.0) } | Select-Object -First 1
                $texts.Remove($weakest)
            }
            $texts[$fields[3]] = [pscustomobject]@{
                Script = if ($fields[1] -eq 's') { '简体' } else { '繁体' }
                Reading = $fields[2]; Text = $fields[3]; Weight = 0.0; Hour = $at; Order = 0
            }
        }
        $entry = $texts[$fields[3]]
        & $age $entry $at
        $entry.Weight = [Math]::Min(20.0, $entry.Weight + $amount)
        $entry.Order = ++$order
        foreach ($other in @($texts.Keys)) { if ($texts[$other].Weight -lt 0.05) { $texts.Remove($other) } }
        if ($texts.Count -eq 0) { $groups.Remove($group) }
    }
    $result = foreach ($texts in $groups.Values) {
        foreach ($entry in $texts.Values) {
            $weight = $entry.Weight * [Math]::Pow(2, - [Math]::Max(0, $Hour - $entry.Hour) / 336.0)
            if ($weight -lt 0.05) { continue }
            [pscustomobject]@{
                Script = $entry.Script; Reading = $entry.Reading; Text = $entry.Text
                Weight = [Math]::Round($weight, 2); Bonus = [Math]::Round([Math]::Min(3.0, [Math]::Log(1 + $weight)), 2)
                LastUsed = [DateTimeOffset]::FromUnixTimeSeconds($entry.Hour * 3600).ToLocalTime().ToString('yyyy-MM-dd HH:00')
                Order = $entry.Order
            }
        }
    }
    return @($result)
}

function Get-Enabled {
    $value = Get-ItemProperty -LiteralPath $RegistryPath -Name SelectionMemory -ErrorAction SilentlyContinue
    return !$value -or $value.SelectionMemory -ne 0
}

switch ($PSCmdlet.ParameterSetName) {
    'Disable' {
        New-Item -Path $RegistryPath -Force | Out-Null
        Set-ItemProperty -LiteralPath $RegistryPath -Name SelectionMemory -Type DWord -Value 0
        Write-Output '选词记忆已关闭：不再记录，也不再调整候选顺序（含个人词频）。已有记录保留，可用 -Clear 清除。下次按键时生效。'
    }
    'Enable' {
        New-Item -Path $RegistryPath -Force | Out-Null
        Set-ItemProperty -LiteralPath $RegistryPath -Name SelectionMemory -Type DWord -Value 1
        Write-Output '选词记忆已开启。下次按键时生效。'
    }
    'Clear' {
        foreach ($file in $Path, $WordsPath) {
            if (Test-Path -LiteralPath $file) {
                # 清空而不是删除：正在输入的应用下次查看时发现文件变短，会一并清空内存里的记录。
                $stream = [IO.File]::Open($file, [IO.FileMode]::Truncate, [IO.FileAccess]::Write, [IO.FileShare]'ReadWrite,Delete')
                $stream.Dispose()
            }
        }
        Write-Output '选词记忆和个人词频已清除。手动添加的个人词不受影响（用 -ClearWords 清除）。各应用会在几秒内的下一次按键时生效。'
    }
    'ListWords' {
        $table = Read-UserWords
        Write-Output ('个人词：' + $UserPath)
        Write-Output ('词数：' + $table.Count + ' 个')
        @($table.Values) | Select-Object -First $Top | ForEach-Object {
            '{0} → {1}{2}' -f $_.Reading, $_.Text, $(if ($_.Traditional) { '（繁体 ' + $_.Traditional + '）' } else { '' })
        }
    }
    'AddWord' {
        $reading = Resolve-WordArguments $AddWord $Text
        if (!(Test-WordText $Traditional -Optional)) { throw '繁体文字不对：不能有控制字符，最多 192 字节（UTF-8）' }
        $table = Read-UserWords
        $key = (Get-ReadingKey $reading) + "`t" + $Text
        $existing = $table.Contains($key)
        if (!$existing -and $table.Count -ge $MaxUserWords) { throw "个人词最多 $MaxUserWords 个" }
        $entry = if ($existing) { $table[$key] } else { [pscustomobject]@{ Reading = $reading; Text = $Text; Traditional = '' } }
        $entry.Reading = $reading
        if ($PSBoundParameters.ContainsKey('Traditional')) { $entry.Traditional = $Traditional }
        $table[$key] = $entry
        Save-UserWords $table
        Write-Output ($(if ($existing) { '已更新' } else { '已添加' }) + "个人词：$reading → $Text" +
            $(if ($entry.Traditional) { "（繁体 $($entry.Traditional)）" } else { '' }) + '。各应用会在几秒内的下一次按键时生效。')
    }
    'ReviseWord' {
        $reading = Resolve-WordArguments $ReviseWord $Text
        $table = Read-UserWords
        $key = (Get-ReadingKey $reading) + "`t" + $Text
        if (!$table.Contains($key)) { throw "没有这个个人词：$reading → $Text" }
        $entry = $table[$key]
        $newReading = if ($NewReading) { Resolve-WordArguments $NewReading $Text } else { $entry.Reading }
        $newText = if ($PSBoundParameters.ContainsKey('NewText')) { $NewText } else { $entry.Text }
        if (!(Test-WordText $newText)) { throw "新文字不对：不能为空、不能有控制字符，最多 $MaxTextBytes 字节（UTF-8）" }
        $newTraditional = if ($PSBoundParameters.ContainsKey('Traditional')) { $Traditional } elseif ($newText -cne $entry.Text) { '' } else { $entry.Traditional }
        if (!(Test-WordText $newTraditional -Optional)) { throw '繁体文字不对：不能有控制字符，最多 192 字节（UTF-8）' }
        $newKey = (Get-ReadingKey $newReading) + "`t" + $newText
        if ($newKey -cne $key -and $table.Contains($newKey)) { throw "已经有这个个人词：$newReading → $newText（请先删除其中一个）" }
        # 修订保留原来的位置（同读音多个个人词时的先后顺序）。
        $revised = [Collections.Specialized.OrderedDictionary]::new([StringComparer]::Ordinal)
        foreach ($item in $table.GetEnumerator()) {
            if ($item.Key -ceq $key) { $revised[$newKey] = [pscustomobject]@{ Reading = $newReading; Text = $newText; Traditional = $newTraditional } }
            else { $revised[$item.Key] = $item.Value }
        }
        Save-UserWords $revised
        Write-Output "已修订个人词：$reading → $Text 改为 $newReading → $newText$(if ($newTraditional) { "（繁体 $newTraditional）" })。各应用会在几秒内的下一次按键时生效。"
    }
    'RemoveWord' {
        $reading = Resolve-WordArguments $RemoveWord $Text
        $table = Read-UserWords
        $key = (Get-ReadingKey $reading) + "`t" + $Text
        if (!$table.Contains($key)) { throw "没有这个个人词：$reading → $Text" }
        $table.Remove($key)
        Save-UserWords $table
        Write-Output "已删除个人词：$reading → $Text。可用 -UndoWords 撤回。各应用会在几秒内的下一次按键时生效。"
    }
    'ExportWords' {
        $table = Read-UserWords
        [IO.File]::WriteAllText($ExportWords, (Format-UserWords $table), [Text.UTF8Encoding]::new($false))
        Write-Output ('已导出 ' + $table.Count + ' 个个人词到 ' + $ExportWords + '（可用 -ImportWords 导入）')
    }
    'ImportWords' {
        if (!(Test-Path -LiteralPath $ImportWords)) { throw "找不到文件：$ImportWords" }
        # 任何一行不对都不导入，个人词保持原样。
        $incoming = Read-UserWords -File $ImportWords -Strict
        foreach ($entry in $incoming.Values) {
            $canonical = ConvertTo-Reading $entry.Reading
            if (!$canonical) { throw "读音不对：$($entry.Reading)（$script:ReadingError）" }
            $entry.Reading = $canonical
        }
        $table = if ($Replace) { [Collections.Specialized.OrderedDictionary]::new([StringComparer]::Ordinal) } else { Read-UserWords }
        $added = 0; $updated = 0
        foreach ($item in $incoming.GetEnumerator()) {
            if ($table.Contains($item.Key)) { $table[$item.Key].Reading = $item.Value.Reading; $table[$item.Key].Traditional = $item.Value.Traditional; ++$updated }
            else { $table[$item.Key] = $item.Value; ++$added }
        }
        if ($table.Count -gt $MaxUserWords) { throw "导入后超过 $MaxUserWords 个个人词，未导入" }
        Save-UserWords $table
        Write-Output ("已导入：新增 $added 个，更新 $updated 个" + $(if ($Replace) { '（替换原有全部个人词）' } else { '' }) + "，现在共 $($table.Count) 个。可用 -UndoWords 撤回。")
    }
    'UndoWords' {
        # 回退：当前版本与上一版本交换。再执行一次就换回来。
        if (!(Test-Path -LiteralPath $UserPreviousPath)) { throw '没有可回退的上一版本' }
        $folder = Split-Path -LiteralPath $UserPath
        $encoding = [Text.UTF8Encoding]::new($false)
        $current = if (Test-Path -LiteralPath $UserPath) { [IO.File]::ReadAllText($UserPath, $encoding) } else { '' }
        $previous = [IO.File]::ReadAllText($UserPreviousPath, $encoding)
        $temporary = Join-Path $folder ('.selection-memory-user-' + [guid]::NewGuid().ToString('N') + '.tmp')
        [IO.File]::WriteAllText($temporary, $previous, $encoding)
        Move-Replacing $temporary $UserPath
        [IO.File]::WriteAllText($temporary, $current, $encoding)
        Move-Replacing $temporary $UserPreviousPath
        Write-Output ('已回退到上一版本，现在共 ' + (Read-UserWords).Count + ' 个个人词。再执行一次 -UndoWords 可以换回。各应用会在几秒内的下一次按键时生效。')
    }
    'ClearWords' {
        Save-UserWords ([Collections.Specialized.OrderedDictionary]::new([StringComparer]::Ordinal))
        Write-Output '个人词已全部清除。可用 -UndoWords 撤回。各应用会在几秒内的下一次按键时生效。'
    }
    'Export' {
        $rows = @((Read-Memory).Values | Sort-Object Order | Select-Object Script, Reading, Text, Count, @{ n = 'Kind'; e = { '选词' } })
        $rows += @(Read-Words | Sort-Object Order | Select-Object Script, Reading, Text, @{ n = 'Count'; e = { $_.Weight } }, @{ n = 'Kind'; e = { '个人词频' } })
        $rows | Export-Csv -LiteralPath $Export -Encoding utf8BOM
        Write-Output ('已导出 ' + $rows.Count + ' 条到 ' + $Export)
    }
    default {
        $entries = @((Read-Memory).Values)
        $words = Read-Words
        Write-Output ('状态：' + $(if (Get-Enabled) { '开启' } else { '关闭' }))
        Write-Output ('文件：' + $Path)
        Write-Output ('记录：' + $entries.Count + ' 条')
        $entries | Sort-Object Order -Descending | Select-Object -First $Top |
            ForEach-Object { '{0}  {1} → {2}  （{3} 次）' -f $_.Script, $_.Reading, $_.Text, $_.Count }
        Write-Output ('个人词频：' + $WordsPath)
        Write-Output ('词数：' + $words.Count + ' 个')
        $words | Sort-Object Order -Descending | Select-Object -First $Top |
            ForEach-Object { '{0}  {1} → {2}  （权重 {3}，加分 {4}，最近 {5}）' -f $_.Script, $_.Reading, $_.Text, $_.Weight, $_.Bonus, $_.LastUsed }
        $user = Read-UserWords
        Write-Output ('个人词（手动添加，不受开关影响）：' + $UserPath)
        Write-Output ('个人词数：' + $user.Count + ' 个')
        @($user.Values) | Select-Object -First $Top | ForEach-Object {
            '{0} → {1}{2}' -f $_.Reading, $_.Text, $(if ($_.Traditional) { '（繁体 ' + $_.Traditional + '）' } else { '' })
        }
    }
}
