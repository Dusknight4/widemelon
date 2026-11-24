<#
.SYNOPSIS
  A worked example: two to four linked Delta PSMD consoles from boot to walking in town together, with a
  screenshot at each step. It's the path tested at full speed on 2026-11-15 (v1-alpha2, the test save).

.DESCRIPTION
  Uses rig_pair.ps1 and rig_cmd.ps1 beside it. Console A hosts, the others join. Screenshots land in
  <Root>\shots\<console>\demo_*.png and their paths are printed. The consoles are left running at the end, in
  town, for you to carry on (.\rig_cmd.ps1 ...) or stop (.\rig_pair.ps1 -Down).
  Frame counts are at full speed (60 frames = 1 second). Loading screens take real time, so on a slow PC add
  frames to the waits ("*420" -> "*600") if a screenshot shows a step was missed.

.EXAMPLE
  .\psmd_town_demo.ps1 -Cia ..\delta_psmd_v1-alpha2.cia -Save ..\handoff_v1_alpha\save_test_postgame_RF2_Lucario_Lv60
  .\psmd_town_demo.ps1 -Consoles 3 -Cia ... -Save ...
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Cia,
    [Parameter(Mandatory = $true)][string]$Save,
    [ValidateRange(2, 4)][int]$Consoles = 2,
    # a rough link from the start, as rig_pair.ps1's options of the same names (default: clean)
    [double]$Loss = 0,
    [double]$Burst = 1,
    [int]$Delay = 0,
    [int]$Jitter = 0,
    # an Azahar room (Create Room / Direct Connect) instead of the link: console A opens it, all join
    [switch]$Room,
    [string]$Root = (Join-Path $env:LOCALAPPDATA "MultiMelonRig")
)
$ErrorActionPreference = "Stop"
$pair = Join-Path $PSScriptRoot "rig_pair.ps1"
$cmd = Join-Path $PSScriptRoot "rig_cmd.ps1"
$names = @("A", "B", "C", "D")[0..($Consoles - 1)]
$joiners = $names | Where-Object { $_ -ne "A" }

function Send([string]$c, [string]$keys, [string]$shot) {
    if ($shot) { & $cmd -I $c -Keys $keys -Shot $shot -Root $Root -TimeoutSec 240 }
    else { & $cmd -I $c -Keys $keys -Root $Root | Out-Null }
}

& $pair -Up -Consoles $Consoles -Cia $Cia -Save $Save -Root $Root -Loss $Loss -Burst $Burst -Delay $Delay -Jitter $Jitter -Room:$Room

# boot (title screen after ~25 s), Start, then the Main Menu: the gear icon (Multiplayer), Local Wireless.
# (the book icon left of the gear is Delete Save: never press A on it)
$toMenu = "*1500,start*3,*420,down*3,*20,right*3,*20,a*3,*240,a*3,*360"
foreach ($c in $joiners) { Send $c $toMenu }
Send "A" $toMenu "demo_1_host_or_join"

# A hosts (Host is the second item); the others join one second apart
Send "A" "down*3,*20,a*3,*120"
$wait = 60
foreach ($c in $joiners) { Send $c "*$wait,a*3,*600"; $wait += 60 }
foreach ($c in $joiners) { Send $c "*1" "demo_2_joined" }

# Check link on all: the host's "Players: N (sent, received)" line
foreach ($c in $joiners) { Send $c "down*3,*20,a*3,*300" }
Send "A" "down*3,*20,a*3,*300" "demo_3_check_link"
foreach ($c in $joiners) { Send $c "*1" "demo_3_check_link" }

# Start game: the host, then each joiner picks it too
Send "A" "a*3,*900"
foreach ($c in $joiners) { Send $c "*60,a*3,*1200" }
foreach ($c in $names) { Send $c "*1" "demo_4_town" }

# the first joiner walks right; the host should see it move
Send "B" "right*45,*90" "demo_5_walked"
Send "A" "*1" "demo_5_host_sees_walk"

& $pair -Status -Root $Root
"Done: the consoles are in town and still running. Stop them with .\rig_pair.ps1 -Down"
