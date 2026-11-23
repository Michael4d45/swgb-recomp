<#
.SYNOPSIS
  Set up a git worktree of swgb-recomp so an agent can build and run it
  while other agents do the same in theirs.

.DESCRIPTION
  A worktree has none of the ignored inputs (game\, work\, src\recomp\gen\)
  and rebuilding the lifted C takes 30+ minutes. This script, run from
  anywhere inside the worktree:

    1. Builds against a shared, prebuilt swgb_gen.lib and the shared
       src\recomp\gen headers (CMake's SWGB_GEN_LIB and GEN_DIR), in a build
       directory of the worktree's own: host builds take seconds.
    2. Gives the worktree a game folder of its own under $Root\games\<name>:
       hard links to the shared install (no extra space; the game only reads
       them) except what the game writes (player profiles, SaveGame,
       Scenario), which are real copies.
    3. Prints the command line to run it: always --multi, so the game's
       one-copy check (a named mutex) does not end another agent's run.

  The shared inputs live in the main checkout and are never changed while
  agents work: $Root\swgb-recomp\work\Battlegrounds.exe,
  $Root\swgb-recomp\src\recomp\gen, $Root\shared\swgb_gen.lib.

  A track that re-lifts the base game builds its own swgb_gen instead:
  pass -OwnGen, and point it at its own gen directory.

  -CloneCampaigns: the base host links the shared library as usual, and the
  expansion's host (swgbrecomp_cc.exe) compiles the worktree's own
  src\recomp\gen_cc (run_lift.py --build cc --all; a full build of the
  lifted C, 25+ minutes) and runs the worktree's work\battlegrounds_x1.exe.

.EXAMPLE
  powershell -File tools\worktree_env.ps1 -Name widescreen -Build
#>
param(
    [Parameter(Mandatory)] [string] $Name,
    [string] $Root = 'A:\Games\star',
    [switch] $Build,
    [switch] $OwnGen,
    [switch] $CloneCampaigns,
    [switch] $FreshGame
)
$ErrorActionPreference = 'Stop'
$wt = (git -C $PSScriptRoot rev-parse --show-toplevel).Trim() -replace '/', '\'
$main = Join-Path $Root 'swgb-recomp'
$sharedGame = Join-Path $main 'game'
$sharedLib = Join-Path $Root 'shared\swgb_gen.lib'
$genDir = Join-Path $main 'src\recomp\gen'
$buildDir = Join-Path $Root "builds\$Name"
$gameDir = Join-Path $Root "games\$Name"
$exe = Join-Path $main 'work\Battlegrounds.exe'

if ($wt -ieq $main) { throw "run this in a worktree, not the main checkout ($main)" }
foreach ($p in $sharedGame, $genDir, $exe) { if (-not (Test-Path $p)) { throw "missing shared input: $p" } }
if (-not $OwnGen -and -not (Test-Path $sharedLib)) { throw "missing $sharedLib (build the main checkout, then copy win-recomp\swgb_gen.lib there)" }

# --- the game folder ---------------------------------------------------------
$writable = @('SaveGame', 'Scenario')                  # directories the game writes
$writableFiles = '^player\.nf'                         # files in the root it rewrites
if ($FreshGame -and (Test-Path -LiteralPath $gameDir)) { [System.IO.Directory]::Delete($gameDir, $true) }
if (-not (Test-Path -LiteralPath $gameDir)) {
    Write-Host "game folder: $gameDir (hard links + copies of what the game writes)"
    # The Win32 call itself: New-Item's wildcard handling trips on file names
    # with brackets ("[EF] ..."), and Windows PowerShell has no literal form.
    if (-not ('WtEnv.Link' -as [type])) {
        Add-Type -Namespace WtEnv -Name Link -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
public static extern bool CreateHardLink(string newFile, string existing, System.IntPtr sa);
'@
    }
    Get-ChildItem -LiteralPath $sharedGame -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($sharedGame.Length + 1)
        $dst = Join-Path $gameDir $rel
        [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($dst))
        $top = $rel.Split('\')[0]
        if ($writable -contains $top -or ($rel -notmatch '\\' -and $_.Name -match $writableFiles)) {
            [System.IO.File]::Copy($_.FullName, $dst, $true)
        } elseif (-not [WtEnv.Link]::CreateHardLink($dst, $_.FullName, [IntPtr]::Zero)) {
            throw "hard link failed ($([Runtime.InteropServices.Marshal]::GetLastWin32Error())): $dst"
        }
    }
}

# --- the build ---------------------------------------------------------------
$pc = (Join-Path $Root 'pcrecomp') -replace '\\', '/'
$cmakeArgs = "-DPCRECOMP=$pc"
if (-not $OwnGen) {
    $cmakeArgs += " -DGEN_DIR=$($genDir -replace '\\','/') -DSWGB_GEN_LIB=$($sharedLib -replace '\\','/')"
}
$env:PCRECOMP = Join-Path $Root 'pcrecomp'
$env:BUILD_DIR = $buildDir
$env:CMAKE_ARGS = $cmakeArgs
Write-Host "build dir : $buildDir"
Write-Host "CMAKE_ARGS: $cmakeArgs"
if ($Build) {
    Push-Location $wt
    try { cmd /c .\build.cmd; if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" } }
    finally { Pop-Location }
}

$run = "`"$buildDir\swgbrecomp.exe`" --run --multi --exe `"$exe`" --game `"$gameDir`""
if ($CloneCampaigns) {
    $run = "`"$buildDir\swgbrecomp_cc.exe`" --run --multi --exe `"$wt\work\battlegrounds_x1.exe`" --game `"$gameDir`""
}
Write-Host ""
Write-Host "run (headless):  $run --headless --watchdog 60"
Write-Host "rebuild:         powershell -File tools\worktree_env.ps1 -Name $Name -Build$(if ($CloneCampaigns) { ' -CloneCampaigns' })"
