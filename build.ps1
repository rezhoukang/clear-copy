param([switch]$Debug)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    foreach ($tool in @('gcc', 'windres', 'python')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "Missing build tool: $tool. Install MinGW-w64 and Python, then add them to PATH."
        }
    }
    New-Item -ItemType Directory -Force -Path build, dist | Out-Null
    & python tools/make_icon.py
    if ($LASTEXITCODE -ne 0) { throw 'Icon generation failed.' }
    Push-Location src
    # Resolve gcc via PATH: this windres version mishandles its default
    # absolute preprocessor path when MinGW lives under Program Files.
    try { & windres --preprocessor gcc --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED -c 65001 app.rc -O coff -o ../build/app.res.o }
    finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
    $flags = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-municode', '-mwindows', '-static-libgcc', '-finput-charset=UTF-8', '-fexec-charset=UTF-8')
    if ($Debug) { $flags += @('-O0', '-g') } else { $flags += @('-Os', '-s', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections') }
    & gcc @flags src/main.c build/app.res.o -o dist/clear-copy.exe -luser32 -lshell32 -ladvapi32 -lcomctl32 -lgdi32
    if ($LASTEXITCODE -ne 0) { throw 'C compilation failed. Close Clear Copy before rebuilding.' }
    Write-Output "Built: $PSScriptRoot\dist\clear-copy.exe ($((Get-Item dist/clear-copy.exe).Length) bytes)"
} finally { Pop-Location }
