<#
.SYNOPSIS
  Builds Quake3e-splitscreen (x64) with VS2019 MSBuild into build\<Configuration>\.

.DESCRIPTION
  Drives the upstream projects in code\win32\msvc2017 one by one, without
  editing them: OutDir/IntDir are redirected under build\, and the version
  define is injected through scripts\splitscreen.props
  (/p:ForceImportBeforeCppTargets).

  Upstream links the renderer as a static lib named <Cfg>-x64-renderer.lib
  (both renderer and renderervk produce that name), so the client is linked
  twice, once per renderer, in separate variant dirs.

  The two clients link SDL2 2.32.10 statically (gamepads; no SDL2.dll): the
  official SDL2 source release is downloaded once into work\sdl2-src\
  (sha256-checked, see work\sdl2-src-PROVENANCE.md) and built as a static
  library with its own VisualC project (scripts\sdl2-static.props).

  Outputs in build\<Configuration>\ (names = upstream's + "-ss", as on Linux):
    quake3e-vulkan-ss.x64.exe   client, Vulkan renderer (the one to launch)
    quake3e-ss.x64.exe          client, OpenGL renderer
    quake3e-ss.ded.x64.exe      dedicated server (no SDL)
    BUILD-INFO.txt
  Intermediates: build\<Configuration>\obj\, SDL2: work\sdl2-src\

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build.ps1
  powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Configuration Debug
  powershell -File scripts\build.ps1 -PlatformToolset v143   (CI: VS2022 runner)
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$PlatformToolset = ''
)

$ErrorActionPreference = 'Stop'

$Repo    = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ProjDir = Join-Path $Repo 'code\win32\msvc2017'
$Props   = Join-Path $Repo 'scripts\splitscreen.props'
$Out     = Join-Path $Repo "build\$Configuration"
$Obj     = Join-Path $Out 'obj'

$MSBuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $MSBuild)) {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($found) { $MSBuild = $found }
    }
}
if (-not (Test-Path $MSBuild)) { Write-Error "MSBuild.exe not found (need VS2019 with C++ workload)"; exit 1 }

$Version = (Get-Content (Join-Path $Repo 'VERSION') -TotalCount 1).Trim()
if ($Version -notmatch '^\d+(\.\d+){2,3}$') { Write-Error "Bad VERSION '$Version'"; exit 1 }

$Commit = (& git -C $Repo rev-parse --short HEAD 2>$null)
if (-not $Commit) { $Commit = 'unknown' }
$Dirty = (& git -C $Repo status --porcelain --untracked-files=no 2>$null)
if ($Dirty) { $Commit = "$Commit-dirty" }

Write-Host "== Quake3e-splitscreen $Version ($Commit) $Configuration|x64 =="

# Variant dirs (trailing backslash required by MSBuild for OutDir/IntDir)
$LibDir = Join-Path $Obj 'lib\'
$GlDir  = Join-Path $Obj 'gl\'
$VkDir  = Join-Path $Obj 'vk\'
$DedDir = Join-Path $Obj 'ded\'
foreach ($d in @($Out, $LibDir, $GlDir, $VkDir, $DedDir)) { New-Item -ItemType Directory -Force $d | Out-Null }

$Toolset = @()
if ($PlatformToolset) { $Toolset = @("/p:PlatformToolset=$PlatformToolset") }

function Invoke-Proj {
    param([string]$Project, [string]$OutDir, [string]$IntName, [string[]]$Extra = @())
    $intDir = Join-Path $Obj "int\$IntName\"
    Write-Host "-- $Project -> $OutDir"
    $msbArgs = @(
        (Join-Path $ProjDir $Project),
        '/nologo', '/m', '/v:minimal',
        "/p:Configuration=$Configuration",
        '/p:Platform=x64',
        "/p:OutDir=$OutDir",
        "/p:IntDir=$intDir",
        '/p:BuildProjectReferences=false',
        "/p:ForceImportBeforeCppTargets=$Props",
        "/p:SplitscreenVersion=$Version"
    ) + $Toolset + $Extra
    & $MSBuild @msbArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "Build failed: $Project ($LASTEXITCODE)"; exit 1 }
}

# Copy a lib only if it changed, so unchanged libs don't force relinks.
function Sync-Libs {
    param([string]$Dest)
    Get-ChildItem (Join-Path $LibDir '*.lib') | ForEach-Object {
        $t = Join-Path $Dest $_.Name
        if (-not (Test-Path $t) -or (Get-Item $t).LastWriteTimeUtc -ne $_.LastWriteTimeUtc -or (Get-Item $t).Length -ne $_.Length) {
            Copy-Item $_.FullName $t -Force
        }
    }
}

# 1. shared static libs
Invoke-Proj 'libogg.vcxproj'    $LibDir 'libogg'
Invoke-Proj 'libvorbis.vcxproj' $LibDir 'libvorbis'
Invoke-Proj 'libjpeg.vcxproj'   $LibDir 'libjpeg'
Invoke-Proj 'botlib.vcxproj'    $LibDir 'botlib'
foreach ($d in @($GlDir, $VkDir, $DedDir)) { Sync-Libs $d }

# 2. renderers (both emit <Cfg>-x64-renderer.lib, hence separate dirs)
Invoke-Proj 'renderer.vcxproj'   $GlDir 'renderer'
Invoke-Proj 'renderervk.vcxproj' $VkDir 'renderervk'

# 3. SDL2 static library for the client exes' gamepads (code/client/in_gamepad.c,
#    Q3E_SDL_STATIC via scripts\splitscreen.props). Built once from the
#    official SDL2 source release with its own VisualC\SDL\SDL.vcxproj, made a
#    static library per configuration by scripts\sdl2-static.props (Release:
#    SDL2-static.lib /MT, Debug: SDL2-static-debug.lib /MTd, like the clients;
#    work\sdl2-src-PROVENANCE.md).
#    Rebuilt when that props file is newer than the lib.
$SdlVersion = '2.32.10'
$SdlZipSha  = '12b2dc2eb8f2836100a7916b5d394a0c82f1f7e32693f95f98305403af242f08'
$SdlRoot    = Join-Path $Repo 'work\sdl2-src'
$SdlSrc     = Join-Path $SdlRoot "SDL2-$SdlVersion"
$SdlLibDir  = Join-Path $SdlRoot 'lib\'
$SdlName    = if ($Configuration -eq 'Debug') { 'SDL2-static-debug' } else { 'SDL2-static' }   # /MTd vs /MT, like the clients
$SdlLib     = Join-Path $SdlLibDir "$SdlName.lib"
$SdlProps   = Join-Path $Repo 'scripts\sdl2-static.props'
if (-not (Test-Path $SdlLib) -or (Get-Item $SdlProps).LastWriteTimeUtc -gt (Get-Item $SdlLib).LastWriteTimeUtc) {
    $SdlProj = Join-Path $SdlSrc 'VisualC\SDL\SDL.vcxproj'
    if (-not (Test-Path $SdlProj)) {
        $dl  = Join-Path $SdlRoot 'dl'
        $zip = Join-Path $dl "SDL2-$SdlVersion.zip"
        New-Item -ItemType Directory -Force $dl | Out-Null
        if (-not (Test-Path $zip)) {
            $url = "https://github.com/libsdl-org/SDL/releases/download/release-$SdlVersion/SDL2-$SdlVersion.zip"
            Write-Host "-- downloading $url"
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        }
        $h = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
        if ($h -ne $SdlZipSha) { Remove-Item $zip; Write-Error "SDL2 source zip sha256 $h != $SdlZipSha (deleted; see work\sdl2-src-PROVENANCE.md)"; exit 1 }
        Expand-Archive $zip -DestinationPath $SdlRoot -Force
    }
    Write-Host "-- SDL2 $SdlVersion static -> $SdlLib"
    $sdlArgs = @(
        $SdlProj, '/nologo', '/m', '/v:minimal',
        "/p:Configuration=$Configuration", '/p:Platform=x64',
        '/p:ConfigurationType=StaticLibrary', "/p:TargetName=$SdlName",
        "/p:OutDir=$SdlLibDir", "/p:IntDir=$(Join-Path $SdlRoot "obj\$Configuration\")",
        "/p:ForceImportBeforeCppTargets=$SdlProps"
    ) + $Toolset
    & $MSBuild @sdlArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "Build failed: SDL2 static ($LASTEXITCODE)"; exit 1 }
    (Get-Item $SdlLib).LastWriteTimeUtc = (Get-Date).ToUniversalTime()
}

# 4. executables (only the clients get SDL2)
$SdlArg = @("/p:SplitscreenSdlLib=$SdlLib")
Invoke-Proj 'quake3e.vcxproj'     $VkDir  'quake3e-vk' $SdlArg
Invoke-Proj 'quake3e.vcxproj'     $GlDir  'quake3e-gl' $SdlArg
Invoke-Proj 'quake3e-ded.vcxproj' $DedDir 'quake3e-ded'

# 5. collect (names = upstream's release names + "-ss", like the Linux build
#    and .github/workflows/release.yml)
$suffix = if ($Configuration -eq 'Debug') { '-debug' } else { '' }
$map = @(
    @{ Src = Join-Path $VkDir  "quake3e$suffix.x64.exe";     Dst = 'quake3e-vulkan-ss.x64.exe' },
    @{ Src = Join-Path $GlDir  "quake3e$suffix.x64.exe";     Dst = 'quake3e-ss.x64.exe' },
    @{ Src = Join-Path $DedDir "quake3e.ded$suffix.x64.exe"; Dst = 'quake3e-ss.ded.x64.exe' }
)
# don't leave stale copies of older names (quake3e.exe / quake3e-gl.exe before 0.0.0.6,
# quake3e-splitscreen*.exe / quake3e.ded.exe before 0.0.0.20) or the SDL2.dll that
# builds before 0.0.0.20 shipped
foreach ($old in @('quake3e.exe', 'quake3e-gl.exe', 'quake3e-splitscreen.exe', 'quake3e-splitscreen-gl.exe', 'quake3e.ded.exe', 'SDL2.dll')) {
    Remove-Item (Join-Path $Out $old) -ErrorAction SilentlyContinue
}
foreach ($m in $map) {
    if (-not (Test-Path $m.Src)) { Write-Error "Expected output missing: $($m.Src)"; exit 1 }
    Copy-Item $m.Src (Join-Path $Out $m.Dst) -Force
}

$tool = if ($PlatformToolset) { $PlatformToolset } else { 'v142' }
$info = @(
    "Quake3e-splitscreen",
    "version: $Version",
    "commit: $Commit",
    "configuration: $Configuration|x64",
    "built: $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))",
    "toolchain: MSBuild $tool",
    "quake3e-vulkan-ss.x64.exe: Vulkan renderer; quake3e-ss.x64.exe: OpenGL renderer; quake3e-ss.ded.x64.exe: dedicated server",
    "SDL2 $SdlVersion static (gamepads, linked into both clients; no SDL2.dll)"
)
Set-Content -Path (Join-Path $Out 'BUILD-INFO.txt') -Value $info -Encoding ASCII

Write-Host "== OK: $Out"
Get-ChildItem $Out -File | ForEach-Object { Write-Host ("   {0,-28} {1,10:N0} bytes" -f $_.Name, $_.Length) }
exit 0
