# Builds tests/self-check.cpp with Windhawk's compiler and runs it.
$wh = 'C:\Program Files\Windhawk\Compiler'
$exe = Join-Path $env:TEMP 'et-self-check.exe'
$args = @('-x', 'c++', '-std=c++23', '-target', 'x86_64-w64-mingw32', '-O0',
    '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-D_WIN32_IE=0x0A00',
    '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD',
    '-DWH_MOD_ID=L\"explorer-terminal\"', '-DWH_MOD_VERSION=L\"test\"',
    '-include', 'windhawk_api.h', '-Wno-pragma-pack', '-Wno-pragma-system-header-outside-header',
    '-I', "$wh\include", (Join-Path $PSScriptRoot 'self-check.cpp'), '-o', $exe,
    '-lcomctl32', '-lshlwapi', '-luuid', '-lole32', '-loleaut32', '-lgdi32', '-ladvapi32',
    '-static', '-static-libgcc', '-static-libstdc++')
& "$wh\bin\clang++.exe" @args
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$env:PATH = "$wh\x86_64-w64-mingw32\bin;$wh\bin;$env:PATH"  # libc++, libunwind, winpthread
& $exe
exit $LASTEXITCODE
