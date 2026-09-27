#requires -Version 7
<#
.SYNOPSIS
    Post-merge closeout for the Windows worktree and WSL verification workflow.

.DESCRIPTION
    Runs after the user approves a merge and the agent has fast-forwarded
    master and pushed it to origin. Verifies master with cold Debug and Release
    builds in WSL, fast-forwards the WSL main repo, then cleans up the task
    worktree, branches, and verification build directory.

.PARAMETER Task
    The task/branch name that was just merged into master.
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Task
)

$ErrorActionPreference = 'Stop'
$env:WSL_UTF8 = '1'

$CloneRoot = 'D:\wsl_codebase\jev-quant'
$WslBuildDir = '\\wsl.localhost\Ubuntu\home\wangyuk\build\jev-quant\master'
$WslMainRepo = '/home/wangyuk/coding_projects/quant_dev_projects/jev-quant'

function Assert-LastExit([string]$What) {
    if ($LASTEXITCODE -ne 0) {
        throw "$What failed with exit code $LASTEXITCODE."
    }
}

New-Item -ItemType Directory -Force -Path $WslBuildDir | Out-Null
robocopy $CloneRoot $WslBuildDir /MIR /XD .git .worktrees build tmp /XF .git /NFL /NDL /NJH /NJS /NP
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with exit code $LASTEXITCODE." }

wsl.exe -d Ubuntu --cd /home/wangyuk/build/jev-quant/master --exec bash -lc 'cmake --preset ci-gcc-debug && cmake --build --preset ci-gcc-debug && ctest --preset test-ci-gcc-debug && cmake --preset ci-gcc-release && cmake --build --preset ci-gcc-release && ctest --preset test-ci-gcc-release'
Assert-LastExit 'WSL build/test'

wsl.exe -d Ubuntu --cd $WslMainRepo --exec bash -c 'git fetch /mnt/d/wsl_codebase/jev-quant master && git merge --ff-only FETCH_HEAD'
Assert-LastExit 'WSL main repo fast-forward'

$worktreePath = Join-Path $CloneRoot ".worktrees\$Task"
if (Test-Path -LiteralPath $worktreePath) {
    git -C $CloneRoot worktree remove ".worktrees/$Task" --force
    Assert-LastExit 'git worktree remove'
}
git -C $CloneRoot branch -d $Task
Assert-LastExit 'git branch -d'
git -C $CloneRoot push origin --delete $Task
Assert-LastExit 'git push origin --delete'
wsl.exe -d Ubuntu --exec bash -lc 'rm -rf -- /home/wangyuk/build/jev-quant/master'
Assert-LastExit 'WSL verification build cleanup'

Write-Host "Merge closeout for '$Task' complete."
