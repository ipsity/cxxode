param(
    [string]$Mingw = 'D:/MSYS2/ucrt64',
    [string]$JsonPrefix = $Mingw,
    [string]$Scratch = (Join-Path $env:TEMP 'lay-code-p4-deps')
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$mingwPath = (Resolve-Path -LiteralPath $Mingw).Path
$jsonPath = (Resolve-Path -LiteralPath $JsonPrefix).Path
$env:Path = (Join-Path $mingwPath 'bin') + ';' + $env:Path

function Invoke-Native {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program exited $LASTEXITCODE"
    }
}

$version = '8.23.0'
$expectedHash = '8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5'
$scratchPath = (New-Item -ItemType Directory -Force -Path $Scratch).FullName
$archive = Join-Path $scratchPath "curl-$version.tar.xz"
if (!(Test-Path -LiteralPath $archive)) {
    Invoke-Native "$env:SystemRoot/System32/curl.exe" @(
        '-fsSL', '--retry', '2', '-o', $archive, "https://curl.se/download/curl-$version.tar.xz"
    )
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $expectedHash) {
    throw 'The downloaded curl release does not match the pinned SHA-256.'
}
$curlSource = Join-Path $scratchPath "curl-$version"
if (!(Test-Path -LiteralPath (Join-Path $curlSource 'CMakeLists.txt'))) {
    Invoke-Native "$env:SystemRoot/System32/tar.exe" @('-xf', $archive, '-C', $scratchPath)
}
$curlBuild = Join-Path $scratchPath 'curl-build'
$curlInstall = Join-Path $scratchPath 'curl-install'
$features = @(
    '-DBUILD_SHARED_LIBS=ON', '-DBUILD_STATIC_LIBS=OFF', '-DBUILD_CURL_EXE=OFF', '-DBUILD_TESTING=OFF',
    '-DBUILD_EXAMPLES=OFF', '-DBUILD_LIBCURL_DOCS=OFF', '-DBUILD_MISC_DOCS=OFF', '-DCURL_BUILD_EVERYTHING=OFF',
    '-DCURL_USE_SCHANNEL=ON', '-DCURL_USE_OPENSSL=OFF', '-DCURL_USE_LIBPSL=OFF', '-DCURL_ZLIB=OFF',
    '-DCURL_BROTLI=OFF', '-DCURL_ZSTD=OFF', '-DUSE_NGHTTP2=OFF', '-DUSE_NGTCP2=OFF',
    '-DCURL_USE_LIBSSH2=OFF', '-DUSE_LIBIDN2=OFF',
    '-DENABLE_ARES=OFF', '-DENABLE_THREADED_RESOLVER=ON'
)
foreach ($protocol in @('DICT', 'FILE', 'FTP', 'GOPHER', 'IMAP', 'IPFS', 'LDAP', 'MQTT', 'POP3', 'RTSP',
                       'SMTP', 'TELNET', 'TFTP')) {
    $features += "-DCURL_DISABLE_$protocol=ON"
}
Invoke-Native cmake (@('-S', $curlSource, '-B', $curlBuild, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                        "-DCMAKE_INSTALL_PREFIX=$curlInstall") + $features)
Invoke-Native cmake @('--build', $curlBuild)
Invoke-Native cmake @('--install', $curlBuild)

$env:Path = (Join-Path $curlInstall 'bin') + ';' + $env:Path
$buildPath = Join-Path $repo 'build/windows-rc1'
Push-Location -LiteralPath $repo
try {
    Invoke-Native cmake @('--preset', 'readonly-release', '-B', $buildPath,
        "-DCMAKE_PREFIX_PATH=$curlInstall;$jsonPath", "-DSQLite3_INCLUDE_DIR=$mingwPath/include",
        "-DSQLite3_LIBRARY=$mingwPath/lib/libsqlite3.dll.a")
    Invoke-Native cmake @('--build', $buildPath, '-j', '6')
    Invoke-Native ctest @('--test-dir', $buildPath, '--output-on-failure')
} finally {
    Pop-Location
}
