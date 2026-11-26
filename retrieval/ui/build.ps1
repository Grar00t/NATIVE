param([string]$Distribution = '')
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\lowlevel')).Path
$wslArgs = @()
if ($Distribution) { $wslArgs += @('--distribution', $Distribution) }
$wslArgs += @('--cd', $sourceRoot, '--exec', 'make', 'windows-dll')
& wsl.exe @wslArgs
if ($LASTEXITCODE -ne 0) { throw 'Native DLL build failed.' }
foreach ($name in @('Niyah.App', 'Niyah.DalwProbe')) {
    $project = Join-Path $PSScriptRoot "$name\$name.csproj"
    & dotnet restore $project --configfile (Join-Path $PSScriptRoot 'NuGet.Config')
    if ($LASTEXITCODE -ne 0) { throw "Restore failed for $name." }
    & dotnet build $project -c Release --no-restore
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $name." }
}
