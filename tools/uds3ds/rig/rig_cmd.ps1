<#
.SYNOPSIS
  Sends a command to one of the consoles started by rig_pair.ps1, and with -Shot waits for the screenshot.

.DESCRIPTION
  -Keys   a keys script: "keys*frames,keys*frames,...". Keys: a b x y l r start select up down left right (D-pad),
          cup cdown cleft cright (Circle Pad); several at once with +; "*frames" alone = nothing pressed.
          Frames are the game's frames (60 a second with the graphics card, as on a 3DS; ~2.5 with -Software).
  -Touch  "X Y FRAMES": touch the bottom screen at X (0-319), Y (0-239) for that many frames.
  -Shot   NAME: a PNG of both screens (shots\<console>\NAME.png), after the keys/touch above have run; the script
          waits for it and prints its path.
  -Quit   stops that console.
  Commands run in order, one after another, on the console's own frames.

.EXAMPLE
  .\rig_cmd.ps1 -I A -Keys "start*3,*25" -Shot title
  .\rig_cmd.ps1 -I B -Keys "down*3,*6,right*3,*8" -Shot b05
  .\rig_cmd.ps1 -I A -Touch "160 120 6" -Shot after_touch
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet("A", "B", "C", "D")][string]$I,
    [string]$Keys,
    [string]$Touch,
    [string]$Shot,
    [switch]$Quit,
    [string]$Root = (Join-Path $env:LOCALAPPDATA "MultiMelonRig"),
    [int]$TimeoutSec = 300
)
$ErrorActionPreference = "Stop"
$cmd = Join-Path $Root "$I.cmd"
if (-not (Test-Path $cmd)) { throw "no console $I under $Root (start them with rig_pair.ps1 -Up)" }
$lines = @()
if ($Keys) { $lines += $Keys }
if ($Touch) { $lines += "touch $Touch" }
if ($Shot) { $lines += "shot $Shot" }
if ($Quit) { $lines += "quit" }
if (-not $lines) { throw "nothing to send: -Keys, -Touch, -Shot or -Quit" }
$since = Get-Date
Add-Content -Path $cmd -Value $lines -Encoding ASCII
if ($Shot) {
    $png = Join-Path $Root "shots\$I\$Shot.png"
    $deadline = $since.AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $png) -and (Get-Item $png).LastWriteTime -ge $since.AddSeconds(-1)) { $png; return }
        Start-Sleep -Milliseconds 500
    }
    throw "no shot after $TimeoutSec s (is console $I running? .\rig_pair.ps1 -Status)"
}
