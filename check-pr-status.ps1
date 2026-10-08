#Requires -Version 5.1
<#
.SYNOPSIS
    Reports the current state of a pull request on GitHub.

.DESCRIPTION
    Watches a pull request and prints its state, labels, review decision and
    checks. Intended to be run by hand or on a schedule, for example from the DSH
    task board, so that new reviewer activity is noticed without watching the page.

    Defaults to the upstream contribution at m417z/my-windhawk-mods#95. Point it
    at another pull request with -Upstream and -PullRequest, for example the
    withdrawn catalog submission:

        .\check-pr-status.ps1 -Upstream ramensoftware/windhawk-mods -PullRequest 6021

    Each target keeps its own state file, so runs do not interfere.

    Exit codes:
        0  no change since the last recorded state
        1  something changed (or this is the first run), details printed
        2  the check itself failed
#>
[CmdletBinding()]
param(
    [int]$PullRequest = 95,
    [string]$Upstream = 'm417z/my-windhawk-mods',

    # File that remembers the previous state between runs.
    [string]$StateFile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $StateFile) {
    $slug = ($Upstream -replace '[^A-Za-z0-9]+', '-')
    $StateFile = Join-Path $PSScriptRoot ".pr-monitor-$slug-$PullRequest.json"
}

$ghCommand = Get-Command gh -ErrorAction SilentlyContinue
$gh = if ($ghCommand) { $ghCommand.Source } else { $null }
if (-not $gh) {
    foreach ($candidate in @(
            (Join-Path $env:ProgramFiles 'GitHub CLI\gh.exe'),
            (Join-Path $env:LOCALAPPDATA 'Programs\GitHub CLI\gh.exe')
        )) {
        if (Test-Path -LiteralPath $candidate) { $gh = $candidate; break }
    }
}
if (-not $gh) {
    Write-Error 'GitHub CLI (gh) was not found.'
    exit 2
}

function Invoke-Gh {
    param([string[]]$Arguments)

    $output = & $gh @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "gh $($Arguments -join ' ') failed: $output"
    }
    return ($output | Out-String).Trim()
}

try {
    $prJson = Invoke-Gh @(
        'pr', 'view', $PullRequest, '--repo', $Upstream, '--json',
        'state,title,labels,reviewDecision,mergeable,comments,url'
    )
    $pr = $prJson | ConvertFrom-Json

    # "no checks reported on ..." is not an error: a repository without CI, or a
    # pull request whose checks have not started, reports it with a non-zero exit.
    # This call is deliberately not routed through Invoke-Gh, and the error
    # preference is relaxed around it because gh writes that message to stderr,
    # which 'Stop' would turn into a terminating error.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $checksRaw = & $gh pr checks $PullRequest --repo $Upstream 2>&1
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    $checks = @()
    foreach ($line in @($checksRaw)) {
        $text = "$line"
        if ($text -match '^\s*(\S+)\s+(pass|fail|pending|skipping|queued|in_progress|cancelled)\b') {
            $checks += [pscustomobject]@{ Name = $Matches[1]; Result = $Matches[2] }
        }
    }
    $checksUnavailable = "$($checksRaw -join ' ')" -match 'no checks reported'
} catch {
    Write-Host "Check failed: $($_.Exception.Message)"
    exit 2
}

$commentSummary = @()
foreach ($comment in @($pr.comments)) {
    $commentSummary += [pscustomobject]@{
        Author = $comment.author.login
        Id     = "$($comment.id)"
        At     = "$($comment.createdAt)"
        Body   = ($comment.body -replace '\s+', ' ').Trim()
    }
}
$externalComments = @($commentSummary | Where-Object { $_.Author -ne 'caa-siu-cat' })

$snapshot = [pscustomobject]@{
    State          = $pr.state
    Labels         = @($pr.labels | ForEach-Object { $_.name }) -join ','
    ReviewDecision = "$($pr.reviewDecision)"
    Mergeable      = "$($pr.mergeable)"
    Checks         = @($checks | ForEach-Object { "$($_.Name)=$($_.Result)" }) -join ';'
    CommentIds     = @($commentSummary | ForEach-Object { $_.Id }) -join ','
    ExternalCount  = $externalComments.Count
}
$snapshotJson = $snapshot | ConvertTo-Json -Compress

Write-Host "PR #$PullRequest - $($pr.title)"
Write-Host "URL          : $($pr.url)"
Write-Host "State        : $($pr.state)  mergeable=$($pr.mergeable)  review=$($snapshot.ReviewDecision)"
Write-Host "Labels       : $($snapshot.Labels)"
Write-Host "Checks       :"
if ($checks.Count -eq 0) {
    if ($checksUnavailable) {
        Write-Host '  (none configured for this branch)'
    } else {
        Write-Host '  (none reported)'
    }
} else {
    foreach ($check in $checks) {
        $mark = if ($check.Result -eq 'pass') { '  ok  ' } elseif ($check.Result -in @('fail', 'cancelled')) { ' FAIL ' } else { ' wait ' }
        Write-Host "$mark $($check.Name) : $($check.Result)"
    }
}
Write-Host "Comments     : $($commentSummary.Count) total, $($externalComments.Count) from reviewers"

$previous = $null
if (Test-Path -LiteralPath $StateFile) {
    try { $previous = (Get-Content -LiteralPath $StateFile -Raw | ConvertFrom-Json) } catch { $previous = $null }
}

$changed = $false
if (-not $previous) {
    Write-Host ''
    Write-Host 'First run: recording the current state.'
    $changed = $true
} else {
    if ($previous.State -ne $snapshot.State) {
        Write-Host ''
        Write-Host "CHANGE: state $($previous.State) -> $($snapshot.State)"
        $changed = $true
    }
    if ($previous.Labels -ne $snapshot.Labels) {
        Write-Host ''
        Write-Host "CHANGE: labels '$($previous.Labels)' -> '$($snapshot.Labels)'"
        $changed = $true
    }
    if ($previous.ReviewDecision -ne $snapshot.ReviewDecision) {
        Write-Host ''
        Write-Host "CHANGE: review decision '$($previous.ReviewDecision)' -> '$($snapshot.ReviewDecision)'"
        $changed = $true
    }
    if ($previous.Checks -ne $snapshot.Checks) {
        Write-Host ''
        Write-Host "CHANGE: checks updated"
        $changed = $true
    }
    if ([int]$previous.ExternalCount -ne $externalComments.Count) {
        Write-Host ''
        Write-Host "CHANGE: reviewer comments $($previous.ExternalCount) -> $($externalComments.Count)"
        $changed = $true
    }
}

$snapshotJson | Set-Content -LiteralPath $StateFile -Encoding UTF8

if ($externalComments.Count -gt 0) {
    Write-Host ''
    Write-Host 'Reviewer comments:'
    foreach ($comment in $externalComments) {
        Write-Host "--- $($comment.Author) ---"
        $body = $comment.Body
        if ($body.Length -gt 700) { $body = $body.Substring(0, 700) + ' [...]' }
        Write-Host $body
    }
}

Write-Host ''
if ($changed) {
    Write-Host 'RESULT: attention needed.'
    exit 1
}
Write-Host 'RESULT: no change.'
exit 0
