# SPDX-License-Identifier: GPL-2.0-only
# PowerShell 7: ./tools/xvdtool-streaming/build.ps1 -BuildTests
# Run the built CLI with: extract <readable-system.xvd> -o <new-output-directory>
[CmdletBinding()]
param(
    [string]$ArchivePath,
    [string]$OutputDirectory,
    [string]$CompilerPath,
    [switch]$BuildTests
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 required' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $ArchivePath) { $ArchivePath = Join-Path $repo 'build-xvd-streaming/win-x64-v1.2.1.zip' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'build-xvd-streaming/repro-adapted' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Build output must stay inside this checkout'
}
if (Test-Path -LiteralPath $output) { throw 'Build output already exists; choose a new directory' }
foreach ($supplied in @($OutputDirectory, $ArchivePath)) {
    if (($supplied -split '[\\/]') -contains '..') { throw 'Parent traversal is not accepted' }
    for ($cursor = [IO.Path]::GetFullPath($supplied); $cursor; $cursor = [IO.Path]::GetDirectoryName($cursor)) {
        if ((Test-Path -LiteralPath $cursor) -and
            ((Get-Item -LiteralPath $cursor).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Linked build/archive paths are not accepted'
        }
    }
}
$zipHash = '5ABC7C298BF9C42DD72A4C7133614F962BF798F61EA50A702B88EF3AC2D26647'
if ((Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash -ne $zipHash) {
    throw 'Release archive does not match the reviewed v1.2.1 Windows x64 asset'
}
if (-not $CompilerPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $CompilerPath = @(& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild/Current/Bin/Roslyn/csc.exe') | Select-Object -First 1
}
if (-not $CompilerPath -or -not (Test-Path -LiteralPath $CompilerPath)) { throw 'Installed Roslyn csc.exe required' }
$dotnet = Join-Path $env:ProgramFiles 'dotnet/dotnet.exe'
$runtimeRoot = Join-Path $env:ProgramFiles 'dotnet/shared/Microsoft.NETCore.App'
$runtime = Get-ChildItem -LiteralPath $runtimeRoot -Directory |
    Where-Object Name -Match '^8\.0\.\d+$' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $runtime -or -not (Test-Path -LiteralPath $dotnet)) { throw 'Installed .NET 8 runtime required; nothing is installed automatically' }
$parent = Split-Path $output -Parent
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$stage = Join-Path $parent ('.xvdtool-build-' + [guid]::NewGuid().ToString('N'))
$src = Join-Path $stage 'src'
$bin = Join-Path $stage 'bin'
New-Item -ItemType Directory -Path $src, $bin | Out-Null
$commit = '2fbf765aa6f8a3ecceee03be0de64a92bc06c569'
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'upstream-sources.json') -Raw | ConvertFrom-Json
foreach ($entry in $manifest) {
    if ($entry.path -notmatch '^XvdTool\.Streaming/(Commands/|Crypto/)?[A-Za-z0-9.]+\.cs$') { throw 'Unsafe source manifest path' }
    $target = Join-Path $src $entry.path
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Invoke-WebRequest -Uri ('https://raw.githubusercontent.com/LukeFZ/XvdTool.Streaming/' + $commit + '/' + $entry.path) -OutFile $target -TimeoutSec 30
    $bytes = [IO.File]::ReadAllBytes($target)
    $prefix = [Text.Encoding]::ASCII.GetBytes('blob ' + $bytes.Length + [char]0)
    $sha = [Security.Cryptography.SHA1]::Create()
    try { $hash = [Convert]::ToHexString($sha.ComputeHash([byte[]]($prefix + $bytes))).ToLowerInvariant() }
    finally { $sha.Dispose() }
    if ($hash -ne $entry.git_blob_sha1) { throw ('Upstream source hash mismatch: ' + $entry.path) }
}
$relative = [IO.Path]::GetRelativePath($repo, $src).Replace('\', '/')
$patch = Join-Path $stage 'integration.patch'
[IO.File]::WriteAllText($patch,
    ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'integration.patch'))).Replace("`r`n", "`n"),
    [Text.UTF8Encoding]::new($false))
& git -C $repo apply --unidiff-zero --check ('--directory=' + $relative) $patch
if ($LASTEXITCODE -ne 0) { throw 'Upstream patch check failed' }
& git -C $repo apply --unidiff-zero ('--directory=' + $relative) $patch
if ($LASTEXITCODE -ne 0) { throw 'Upstream patch failed' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'SystemOsExtractor.cs') -Destination (Join-Path $src 'XvdTool.Streaming/SystemOsExtractor.cs')
$globals = 'System', 'System.Collections.Generic', 'System.IO', 'System.Linq', 'System.Net.Http', 'System.Threading', 'System.Threading.Tasks'
($globals | ForEach-Object { 'global using ' + $_ + ';' }) | Set-Content (Join-Path $src 'XvdTool.Streaming/GlobalUsings.cs')
$unpack = Join-Path $stage 'release'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($ArchivePath))
try {
    foreach ($entry in $zip.Entries) {
        if ($entry.FullName -notmatch '^win-x64/[A-Za-z0-9.]+$' -or $entry.Length -gt 16MB) {
            throw 'Unexpected release archive member'
        }
    }
} finally { $zip.Dispose() }
[IO.Compression.ZipFile]::ExtractToDirectory([IO.Path]::GetFullPath($ArchivePath), $unpack)
$release = Join-Path $unpack 'win-x64'
$references = @()
foreach ($dll in Get-ChildItem -LiteralPath $runtime.FullName -Filter '*.dll') {
    try { [Reflection.AssemblyName]::GetAssemblyName($dll.FullName) | Out-Null; $references += '/reference:"' + $dll.FullName + '"' } catch {}
}
$references += Get-ChildItem -LiteralPath $release -Filter '*.dll' | Where-Object Name -NE 'XvdTool.Streaming.dll' |
    ForEach-Object { '/reference:"' + $_.FullName + '"' }
$options = @('/nologo', '/target:exe', '/langversion:preview', '/nullable:enable', '/optimize+', '/nostdlib+')
$arguments = $options + $references + @('/out:"' + (Join-Path $bin 'XvdTool.Streaming.dll') + '"')
$arguments += Get-ChildItem -LiteralPath $src -Filter '*.cs' -Recurse | ForEach-Object { '"' + $_.FullName + '"' }
$rsp = Join-Path $stage 'compile.rsp'
$arguments | Set-Content $rsp
& $CompilerPath ('@' + $rsp) *> (Join-Path $stage 'build.log')
if ($LASTEXITCODE -ne 0) { throw ('Compilation failed; inspect ' + $stage + '/build.log') }
Get-ChildItem -LiteralPath $release -File | Where-Object Name -NotIn @('XvdTool.Streaming.dll', 'XvdTool.Streaming.pdb') |
    Copy-Item -Destination $bin
if ($BuildTests) {
    $fixture = Join-Path $repo 'tests/xvdtool_systemos_fixture.cs'
    ($options + $references + @('/out:"' + (Join-Path $bin 'OwnedXvdFixture.dll') + '"', '"' + $fixture + '"')) | Set-Content $rsp
    & $CompilerPath ('@' + $rsp) *> (Join-Path $stage 'fixture-build.log')
    if ($LASTEXITCODE -ne 0) { throw 'Fixture compilation failed' }
    Copy-Item (Join-Path $bin 'XvdTool.Streaming.runtimeconfig.json') (Join-Path $bin 'OwnedXvdFixture.runtimeconfig.json')
}
Copy-Item (Join-Path $PSScriptRoot 'COPYING'), (Join-Path $PSScriptRoot 'NOTICE') $stage
[ordered]@{ upstream_commit = $commit; release_sha256 = $zipHash; runtime = $runtime.Name;
    compiler = $CompilerPath; systemos_source_sha256 = (Get-FileHash (Join-Path $PSScriptRoot 'SystemOsExtractor.cs')).Hash;
    release_dependency = 'External GPL tool; not linked to xone-native runtime' } |
    ConvertTo-Json | Set-Content (Join-Path $stage 'build-provenance.json')
[IO.Directory]::Move($stage, $output)
Write-Output ('Built external extractor: ' + (Join-Path $output 'bin/XvdTool.Streaming.exe'))
