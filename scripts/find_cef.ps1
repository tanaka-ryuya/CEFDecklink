$json = Get-Content 'cef_index.json'  -Raw | ConvertFrom-Json
$versions = $json.windows64.versions
$stable = $versions | Where-Object { $_.channel -eq 'stable' }
# If multiple stable, pick first
if ($stable -is [array]) { $stable = $stable[0] }

$file = $stable.files | Where-Object { $_.type -eq 'minimal' } | Select-Object -First 1
$name = $file.name
$url = "https://cef-builds.spotifycdn.com/" + $name.Replace("+", "%2B")
Write-Output $url
