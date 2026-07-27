param(
    [Parameter(Mandatory = $true)]
    [string]$Binary,

    [Parameter(Mandatory = $true)]
    [ValidateSet("8664", "AA64")]
    [string]$Machine
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Binary -PathType Leaf)) {
    throw "Windows binary does not exist: $Binary"
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere -PathType Leaf)) {
    throw "vswhere.exe was not found"
}

$installations = @(& $vswhere -latest -products * -property installationPath)
if ($installations.Count -eq 0) {
    throw "Visual Studio installation was not found"
}

$hostArchitecture = switch ($env:PROCESSOR_ARCHITECTURE) {
    "AMD64" { "x64" }
    "ARM64" { "arm64" }
    default { throw "unsupported Windows host architecture: $env:PROCESSOR_ARCHITECTURE" }
}

$toolRoot = Join-Path $installations[0] "VC\Tools\MSVC"
$dumpbin = Get-ChildItem -Path $toolRoot -Filter dumpbin.exe -File -Recurse |
    Where-Object {
        $_.FullName -match "\\bin\\Host$hostArchitecture\\$hostArchitecture\\dumpbin\.exe$"
    } |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if ($null -eq $dumpbin) {
    throw "native dumpbin.exe was not found under $toolRoot"
}

$headers = (& $dumpbin.FullName /headers $Binary | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /headers failed"
}
if ($headers -notmatch "$Machine machine") {
    throw "binary has the wrong Windows machine type"
}

$dependencies = (& $dumpbin.FullName /dependents $Binary | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /dependents failed"
}
if ($dependencies -match "VCRUNTIME|MSVCP") {
    throw "MSVC runtime is dynamically linked"
}
