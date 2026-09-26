# Refresh the Binance UM Futures (USDS-M) documentation in this folder.
#
# Source: developers.binance.com exposes official markdown for product docs
# through the Agent Native endpoints (see /en/docs/llms.txt). The host is not
# directly reachable from networks where its DNS is polluted, so requests go
# through a local proxy (default Clash mixed port 7897). Adjust $proxy or
# remove it if you have direct access.
#
# The per-endpoint REST/WS API reference pages are rendered client-side from
# OpenAPI specs and have no markdown endpoint; the llms.txt API Reference
# section (endpoint list) and the official Skills Hub SKILL.md are fetched
# instead. Update api-endpoint-index.md / api-endpoint-reference-skill.md
# manually when Binance changes those.
#
# Usage: pwsh -File refresh-docs.ps1   (run from the repository root)

$ErrorActionPreference = 'Stop'
$proxy  = 'http://127.0.0.1:7897'
$dir    = $PSScriptRoot
$fetched = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd')
$base   = 'https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures'

$pages = [ordered]@{
  'introduction.md'        = "$base/Introduction.md"
  'quick-start.md'         = "$base/quick-start.md"
  'change-log.md'          = "$base/change-log.md"
  'general-info.md'        = "$base/general-info.md"
  'websocket-api-general-info.md' = "$base/websocket-api-general-info.md"
  'user-data-streams.md'   = "$base/user-data-streams.md"
  'common-definition.md'   = "$base/common-definition.md"
  'error-code.md'          = "$base/error-code.md"
  'websocket-market-streams/Live-Subscribing-Unsubscribing-to-streams.md' = "$base/websocket-market-streams/Live-Subscribing-Unsubscribing-to-streams.md"
  'websocket-market-streams/How-to-manage-a-local-order-book-correctly.md' = "$base/websocket-market-streams/How-to-manage-a-local-order-book-correctly.md"
  'websocket-market-streams/Important-WebSocket-Change-Notice.md' = "$base/websocket-market-streams/Important-WebSocket-Change-Notice.md"
  'faq/stp-faq.md'         = "$base/faq/stp-faq.md"
}

foreach ($name in $pages.Keys) {
  $url = $pages[$name]
  $dest = Join-Path $dir $name
  New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
  $code = & curl.exe -sS --max-time 120 --proxy $proxy -o "$dest" -w '%{http_code}' $url
  $size = 0; if (Test-Path $dest) { $size = (Get-Item $dest).Length }
  if ($code -ne '200' -or $size -lt 64) { Remove-Item $dest -Force -ErrorAction SilentlyContinue; Write-Host "FAIL $name http $code size $size"; continue }
  $body = Get-Content $dest -Raw
  if ($body -notmatch '^<!-- Source:') {
    $h = "<!-- Source: $url`n     Fetched: $fetched -->`n"
    Set-Content -Path $dest -Value ($h + $body) -Encoding UTF8 -NoNewline
  }
  Write-Host ("OK   {0}  {1} bytes" -f $name, $size)
  Start-Sleep -Milliseconds 800
}
