param([string]$VcpkgRoot = 'D:\vcpkg')
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
function Invoke-Checked { param([scriptblock]$Action) & $Action; if ($LASTEXITCODE -ne 0) { throw "Command failed: $LASTEXITCODE" } }
New-Item -ItemType Directory -Force (Join-Path $ProjectRoot 'third_party') | Out-Null
$ProtocolPath = Join-Path $ProjectRoot 'third_party/protocol'
if (-not (Test-Path -LiteralPath $ProtocolPath)) {
    Invoke-Checked { git clone https://github.com/livekit/protocol.git $ProtocolPath }
    Invoke-Checked { git -C $ProtocolPath checkout f09f87aab29b }
}
$ProtobufPath = Join-Path $ProjectRoot 'third_party/protobuf'
if (-not (Test-Path -LiteralPath $ProtobufPath)) {
    Invoke-Checked { git clone --branch v21.12 --depth 1 https://github.com/protocolbuffers/protobuf.git $ProtobufPath }
}
Invoke-Checked { & (Join-Path $VcpkgRoot 'vcpkg.exe') install boost-beast:x64-windows-static nlohmann-json:x64-windows-static openssl:x64-windows-static libvpx:x64-windows-static dav1d:x64-windows-static }
Write-Host "Dependencies ready. Set `$env:VCPKG_ROOT='$VcpkgRoot', then cmake --preset windows-x64."
