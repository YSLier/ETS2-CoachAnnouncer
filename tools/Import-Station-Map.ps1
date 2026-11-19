param(
    [string]$InputJson,
    [string]$OutputDirectory = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'CoachAnnouncer')
)

$ErrorActionPreference = 'Stop'
$revision = '731cea845d3968e7e14a42105b2e7fd3142a5d14'
$uri = "https://raw.githubusercontent.com/Nethercap/truck-companion/$revision/docs/data/pois-ets2.json"
$sha256 = '9ec573ada367adf08c24549d6db52b204ec656204e01e9d9346d3d83030710be'
$temporaryDownload = $null
$temporaryOutput = $null

function Test-Coordinate($value) {
    return ($value -is [int] -or $value -is [long] -or $value -is [double]) -and
        -not [double]::IsNaN($value) -and [Math]::Abs([double]$value) -lt 10000000
}

try {
    if (-not $InputJson) {
        $localFile = Join-Path $PSScriptRoot 'pois-ets2.json'
        if ([IO.File]::Exists($localFile)) { $InputJson = $localFile }
    }
    $downloaded = -not $InputJson
    if ($downloaded) {
        $temporaryDownload = [IO.Path]::GetTempFileName()
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        Write-Host 'Downloading the pinned TruckDash ETS2 station map...'
        Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $temporaryDownload -TimeoutSec 30
        if ((Get-FileHash -LiteralPath $temporaryDownload -Algorithm SHA256).Hash -ne $sha256) {
            throw 'The downloaded map did not match its expected SHA-256.'
        }
        $InputJson = $temporaryDownload
    }

    $file = Get-Item -LiteralPath $InputJson
    if ($file.PSIsContainer -or $file.Length -gt 8MB) { throw 'Expected a POI JSON file up to 8 MiB.' }
    $map = Get-Content -LiteralPath $file.FullName -Encoding UTF8 -Raw | ConvertFrom-Json
    $rows = New-Object 'System.Collections.Generic.List[string]'
    $rows.Add('# coach-stations-v1 ets2-1.62')
    $tables = [ordered]@{ line = 'busStations'; tourist = 'busTouristStops'; viewpoint = 'viewpoints' }
    $counts = @{}
    $culture = [Globalization.CultureInfo]::InvariantCulture
    foreach ($kind in $tables.Keys) {
        $count = 0
        $table = $map.($tables[$kind])
        if ($table -is [pscustomobject]) {
            foreach ($entry in ($table.PSObject.Properties | Sort-Object Name)) {
                $id = $entry.Name
                $coordinates = $entry.Value
                if ($id -cnotmatch '^[a-z0-9_.-]{1,96}$' -or $coordinates -isnot [array] -or $coordinates.Count -lt 2) {
                    continue
                }
                $x, $z = $coordinates[0], $coordinates[1]
                if (-not (Test-Coordinate $x) -or -not (Test-Coordinate $z) -or ($x -eq 0 -and $z -eq 0)) {
                    continue
                }
                $rows.Add(("{0}`t{1}`t{2}`t{3}" -f $kind, $id, ([double]$x).ToString('G17', $culture), ([double]$z).ToString('G17', $culture)))
                $count++
            }
        }
        $counts[$kind] = $count
    }
    if ($counts.line -lt 1 -or $rows.Count -gt 4097) { throw 'No valid line stations, or too many POIs.' }
    if ($downloaded -and ($counts.line -ne 223 -or $counts.tourist -ne 56 -or $counts.viewpoint -ne 137)) {
        throw 'Unexpected station counts in the pinned map.'
    }

    [void][IO.Directory]::CreateDirectory($OutputDirectory)
    $output = Join-Path $OutputDirectory 'station-map-ets2-1.62.tsv'
    $temporaryOutput = Join-Path $OutputDirectory ([guid]::NewGuid().ToString() + '.tmp')
    [IO.File]::WriteAllLines($temporaryOutput, $rows, [Text.UTF8Encoding]::new($false))
    if ([IO.File]::Exists($output)) {
        [IO.File]::Replace($temporaryOutput, $output, [NullString]::Value)
    } else {
        [IO.File]::Move($temporaryOutput, $output)
    }
    Write-Host ("Imported {0} line stations, {1} tourist stops and {2} viewpoints." -f $counts.line, $counts.tourist, $counts.viewpoint)
    Write-Host $output
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    if ($downloaded) {
        [Console]::Error.WriteLine('You can also save the pinned JSON next to this script as pois-ets2.json and run the importer again.')
        [Console]::Error.WriteLine($uri)
    }
    exit 1
} finally {
    if ($temporaryDownload) { [IO.File]::Delete($temporaryDownload) }
    if ($temporaryOutput) { [IO.File]::Delete($temporaryOutput) }
}
