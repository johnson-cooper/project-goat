param(
    [switch]$AllowUnpinned
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dependencies = @(
    @{ Path = 'external/ygopro-core'; Url = 'https://github.com/edo9300/ygopro-core.git'; Commit = '9a0c558c2d686542f7914a6d529fd7aa57746aed' },
    @{ Path = 'external/CardScripts'; Url = 'https://github.com/ProjectIgnis/CardScripts.git'; Commit = '6d4cfc16326ddb4c1d74f5835d8e43e2c8228007' },
    @{ Path = 'external/BabelCDB'; Url = 'https://github.com/ProjectIgnis/BabelCDB.git'; Commit = '7f3a3af2520a31122d6ed0db2077d3aba04a97d6' },
    @{ Path = 'external/LFLists'; Url = 'https://github.com/ProjectIgnis/LFLists.git'; Commit = '931655bd21435ba6b1851f3c66e2d967f06ff4d6' },
    @{ Path = 'external/windbot'; Url = 'https://github.com/ProjectIgnis/windbot.git'; Commit = 'fa0ae767967afc6a820784837f11cd3fabb9c47c' }
)

foreach ($dependency in $dependencies) {
    $target = Join-Path $root $dependency.Path
    if (-not (Test-Path $target)) {
        git clone $dependency.Url $target
    }
    git -C $target fetch --tags origin
    $ref = if ($AllowUnpinned) { 'origin/master' } else { $dependency.Commit }
    git -C $target checkout --detach $ref
}
