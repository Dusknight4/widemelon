<#
.SYNOPSIS
  Two to four headless MultiMelon 3DS consoles (rig3ds.exe), linked on 127.0.0.1 by MultiMelon's "Wifi as Local
  Multiplayer", each with its own copy of a save, driven by command files (rig_cmd.ps1). No window opens and
  nothing takes the keyboard. They draw with the graphics card (full speed); -Software uses the slow software
  renderer instead.

.DESCRIPTION
  -Up      installs the CIA into console A (the first time, and again whenever -Cia is another file or a newer
           build; -Reinstall forces it), gives the others the same game, puts a fresh copy of the save into each,
           and starts them. -Consoles 3 or 4 adds C and D. Console A listens on port 7701, B on 7702, C 7703,
           D 7704. -SaveB / -SaveC / -SaveD give that console another save than -Save.
           -Speed: percent of a 3DS's speed (default 100; 0 = as fast as the PC can).
           A rough link, as real wireless can be (each console, on what it sends; default all off):
           -Loss percent of datagrams lost, -Burst how many datagrams a loss burst lasts on average (default 1),
           -Delay and -Jitter ms each frame arrives late (delay + 0..jitter, in order unless -Reorder),
           -Dup percent of frames sent twice, -Seed for the dice. rig_cmd.ps1 -Link changes it while running.
  -Status  the last progress line of each console (frames run and frames a second, consoles heard, frames
           sent/received).
  -Down    quits them all.
  Everything lives under -Root (default: %LOCALAPPDATA%\MultiMelonRig, outside OneDrive: the game is ~2 GB).
  A\, B\, ... are the consoles (their log is A\log\azahar_log.txt), shots\A, shots\B, ... the screenshots, A.cmd,
  B.cmd, ... the command files.
  The source saves are only read, never changed.

.EXAMPLE
  .\rig_pair.ps1 -Up -Cia ..\..\delta_psmd_mp33.cia -Save ..\..\handoff_v1_alpha\save_test_postgame_RF2_Lucario_Lv60
  .\rig_cmd.ps1 -I A -Keys "start*3,*25" -Shot a01
  .\rig_pair.ps1 -Status
  .\rig_pair.ps1 -Down
#>
[CmdletBinding()]
param(
    [switch]$Up,
    [switch]$Down,
    [switch]$Status,
    [switch]$Reinstall,
    [string]$Cia,
    [string]$Save,
    [string]$SaveB,
    [string]$SaveC,
    [string]$SaveD,
    [ValidateRange(2, 4)][int]$Consoles = 2,
    [switch]$Software,
    [ValidateRange(0, 1000)][int]$Speed = 100,
    [ValidateRange(0, 100)][double]$Loss = 0,
    [ValidateRange(1, 1000)][double]$Burst = 1,
    [ValidateRange(0, 60000)][int]$Delay = 0,
    [ValidateRange(0, 60000)][int]$Jitter = 0,
    [ValidateRange(0, 100)][double]$Dup = 0,
    [switch]$Reorder,
    [int]$Seed = 0,
    [string]$Root = (Join-Path $env:LOCALAPPDATA "MultiMelonRig"),
    [int]$Minutes = 120
)
$ErrorActionPreference = "Stop"
$Rig = Join-Path $PSScriptRoot "rig3ds.exe"
$Ids = "Nintendo 3DS\00000000000000000000000000000000\00000000000000000000000000000000"
$Ports = @{ A = 7701; B = 7702; C = 7703; D = 7704 }
$All = @("A", "B", "C", "D")

# the title id of a CIA (from its TMD), as 16 hex digits
function Get-CiaTitleId([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try {
        $br = New-Object IO.BinaryReader($fs)
        $hdr = [int64]$br.ReadUInt32(); [void]$br.ReadUInt32()
        $cert = [int64]$br.ReadUInt32(); $tik = [int64]$br.ReadUInt32()
        $a64 = { param($x) [int64](([int64]$x + 63) -band (-bnot [int64]63)) }
        $tmd = & $a64 ((& $a64 ((& $a64 $hdr) + $cert)) + $tik)
        $fs.Position = $tmd
        $b = $br.ReadBytes(4)
        $sigType = ([int64]$b[0] -shl 24) -bor ([int64]$b[1] -shl 16) -bor ([int64]$b[2] -shl 8) -bor [int64]$b[3]
        $sigSize = switch ($sigType) { 0x10000 { 0x23C } 0x10003 { 0x23C } 0x10001 { 0x13C } 0x10004 { 0x13C } default { 0x7C } }
        $fs.Position = $tmd + 4 + $sigSize + 0x4C
        return (($br.ReadBytes(8) | ForEach-Object { $_.ToString("X2") }) -join "")
    } finally { $fs.Close() }
}

function Get-RigProcesses {
    $full = (Resolve-Path $Rig).Path
    Get-Process rig3ds -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $full }
}

if ($Down) {
    foreach ($c in $All) {
        $f = Join-Path $Root "$c.cmd"
        if (Test-Path $f) { Add-Content -Path $f -Value "quit" -Encoding ASCII }
    }
    for ($i = 0; $i -lt 20 -and (Get-RigProcesses); $i++) { Start-Sleep -Milliseconds 500 }
    Get-RigProcesses | Stop-Process -Force
    "consoles stopped"
    return
}

if ($Status) {
    foreach ($c in $All) {
        $out = Join-Path $Root "$c.out"
        if (Test-Path $out) { "${c}: $(Get-Content $out -Tail 1)" }
    }
    return
}

if (-not $Up) { Get-Help $MyInvocation.MyCommand.Path -Detailed; return }
if (-not $Cia -or -not (Test-Path $Cia)) { throw "-Cia: the game's .cia" }
if (-not $Save -or -not (Test-Path $Save)) { throw "-Save: a folder with the save's files (as Azahar keeps them)" }
if (Get-RigProcesses) { throw "consoles are running: .\rig_pair.ps1 -Down first" }
$Cia = (Resolve-Path $Cia).Path
$Names = $All[0..($Consoles - 1)]
$Saves = @{}
foreach ($c in $Names) {
    $s = switch ($c) { "B" { $SaveB } "C" { $SaveC } "D" { $SaveD } default { "" } }
    if (-not $s) { $s = $Save }
    if (-not (Test-Path $s)) { throw "save folder not found: $s" }
    $Saves[$c] = (Resolve-Path $s).Path
}
New-Item -ItemType Directory -Force -Path $Root | Out-Null
$Root = (Resolve-Path $Root).Path
# (consoles left from a bigger set before: their command files and progress go, so -Status and -Down see these)
foreach ($c in $All) {
    if ($Names -notcontains $c) { foreach ($x in "$c.cmd", "$c.out", "$c.err") { Remove-Item (Join-Path $Root $x) -ErrorAction SilentlyContinue } }
}

$tid = Get-CiaTitleId $Cia
$low = $tid.Substring(8).ToLower()
"title $tid"
$Titles = @{}
foreach ($c in $All) { $Titles[$c] = Join-Path $Root "$c\sdmc\$Ids\title\00040000\$low" }
$titleA = $Titles["A"]

# install into A (the first time, or when the CIA is another build than the one installed: same title, so one
# replaces the other), then give the others the same game: hard links, no second copy
$item = Get-Item $Cia
$stamp = "$Cia|$($item.Length)|$($item.LastWriteTimeUtc.Ticks)"
$stampFile = Join-Path $Root "installed.txt"
$installed = if (Test-Path $stampFile) { (Get-Content $stampFile -Raw).Trim() } else { "" }
if ($Reinstall -or $installed -ne $stamp -or -not (Test-Path (Join-Path $titleA "content"))) {
    foreach ($t in $Titles.Values) { if (Test-Path (Join-Path $t "content")) { Remove-Item -Recurse -Force (Join-Path $t "content") } }
    "installing $($item.Name) into console A (a minute or two)..."
    $env:RIG_NO_LINK = "1"; $env:RIG_FAST_EXIT = "1"; $env:RIG_RENDERER = "software"
    & $Rig (Join-Path $Root "A") $Cia 1 | Out-File -Encoding ASCII (Join-Path $Root "A_install.log")
    Remove-Item Env:RIG_NO_LINK, Env:RIG_FAST_EXIT, Env:RIG_RENDERER
    if (-not (Test-Path (Join-Path $titleA "content"))) { throw "install failed: see A_install.log" }
    Set-Content -Path $stampFile -Value $stamp -Encoding UTF8
}
foreach ($c in $Names | Where-Object { $_ -ne "A" }) {
    $t = $Titles[$c]
    New-Item -ItemType Directory -Force -Path (Join-Path $t "content") | Out-Null
    foreach ($f in Get-ChildItem (Join-Path $titleA "content") -File) {
        $dst = Join-Path $t "content\$($f.Name)"
        if (Test-Path $dst) { continue }
        try { New-Item -ItemType HardLink -Path $dst -Target $f.FullName | Out-Null }
        catch { Copy-Item $f.FullName $dst }
    }
    if (-not (Test-Path (Join-Path $Root "$c\nand"))) { Copy-Item -Recurse (Join-Path $Root "A\nand") (Join-Path $Root "$c\nand") }
}

# a fresh copy of its save in each
foreach ($c in $Names) {
    $t = $Titles[$c]
    $src = $Saves[$c]
    $data = Join-Path $t "data\00000001"
    if (Test-Path $data) { Remove-Item -Recurse -Force $data }
    New-Item -ItemType Directory -Force -Path $data | Out-Null
    Get-ChildItem $src -File | Where-Object { $_.Name -ne "00000001.metadata" } | Copy-Item -Destination $data
    $meta = Join-Path $t "data\00000001.metadata"
    $srcMeta = Join-Path $src "00000001.metadata"
    if (Test-Path $srcMeta) { Copy-Item $srcMeta $meta -Force }
    elseif (-not (Test-Path $meta)) {
        # the save's format info as Delta PSMD makes it (size 0x40000, 8 folders, 8 files); other games: their own
        [IO.File]::WriteAllBytes($meta, [byte[]](0, 0, 4, 0, 8, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0, 0))
    }
}

# start them, hidden, each with its command file; each says hello to the others' ports
foreach ($c in $Names) {
    $cmd = Join-Path $Root "$c.cmd"
    Set-Content -Path $cmd -Value $null -Encoding ASCII
    $env:RIG_LINK_PORT = "$($Ports[$c])"
    $env:RIG_LINK_PEERS = (($Names | Where-Object { $_ -ne $c } | ForEach-Object { "127.0.0.1:$($Ports[$_])" }) -join ",")
    $env:RIG_LINK_LOOPBACK = "1"
    $env:RIG_NO_INSTALL = "1"
    $env:RIG_FAST_EXIT = "1"
    $env:RIG_CMD = $cmd
    $env:RIG_SHOTS = Join-Path $Root "shots\$c"
    $env:RIG_SPEED = "$Speed"
    if ($Software) { $env:RIG_RENDERER = "software" } else { Remove-Item Env:RIG_RENDERER -ErrorAction SilentlyContinue }
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $env:RIG_LINK_LOSS = $Loss.ToString($inv); $env:RIG_LINK_BURST = $Burst.ToString($inv)
    $env:RIG_LINK_DELAY_MS = "$Delay"; $env:RIG_LINK_JITTER_MS = "$Jitter"; $env:RIG_LINK_DUP = $Dup.ToString($inv)
    $env:RIG_LINK_REORDER = if ($Reorder) { "1" } else { "0" }
    # (each console its own dice, but repeatable with -Seed)
    $env:RIG_LINK_SEED = if ($Seed) { "$($Seed + [array]::IndexOf($All, $c))" } else { "0" }
    $argLine = "`"$(Join-Path $Root $c)`" `"$Cia`" $($Minutes * 60)"
    $p = Start-Process -FilePath $Rig -ArgumentList $argLine -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $Root "$c.out") -RedirectStandardError (Join-Path $Root "$c.err")
    "console $c started (process $($p.Id), port $($Ports[$c]))"
}
foreach ($v in "RIG_LINK_PORT", "RIG_LINK_PEERS", "RIG_LINK_LOOPBACK", "RIG_NO_INSTALL", "RIG_FAST_EXIT", "RIG_CMD", "RIG_SHOTS",
             "RIG_SPEED", "RIG_RENDERER", "RIG_LINK_LOSS", "RIG_LINK_BURST", "RIG_LINK_DELAY_MS", "RIG_LINK_JITTER_MS",
             "RIG_LINK_DUP", "RIG_LINK_REORDER", "RIG_LINK_SEED") {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}
"$($Names.Count) consoles booting. Check the renderer line in A.out; drive them with .\rig_cmd.ps1; see README.md."
