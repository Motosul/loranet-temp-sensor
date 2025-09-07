# Serve the ../www folder on port 8000
$ErrorActionPreference = "Stop"
$dir = Join-Path $PSScriptRoot "..\www" | Resolve-Path
Write-Host "Serving $dir on http://0.0.0.0:8000"
python -m http.server 8000 --directory "$dir"
