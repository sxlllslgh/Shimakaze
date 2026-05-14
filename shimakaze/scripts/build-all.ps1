param(
    [string]$Workflow = "Shimakaze Build",
    [switch]$Watch
)

$ErrorActionPreference = "Stop"

gh workflow run $Workflow

if ($Watch) {
    Start-Sleep -Seconds 3
    gh run watch
}
