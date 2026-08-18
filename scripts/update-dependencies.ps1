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
    @{ Path = 'external/windbot'; Url = 'https://github.com/ProjectIgnis/windbot.git'; Commit = 'fa0ae767967afc6a820784837f11cd3fabb9c47c' },
    @{ Path = 'external/raylib'; Url = 'https://github.com/raysan5/raylib.git'; Commit = 'dbc56a87da87d973a9c5baa4e7438a9d20121d28' }
)

foreach ($dependency in $dependencies) {
    $target = Join-Path $root $dependency.Path
    $gitDir = Join-Path $target '.git'
    if ((Test-Path $target) -and -not (Test-Path $gitDir)) {
        # BabelCDB, LFLists, and the trimmed CardScripts subset are committed
        # directly into this project's own git history (see .gitignore's
        # notes on each) rather than vendored as a separate pinned clone -
        # on a fresh checkout (any CI run, or anyone who hasn't run this
        # script before) the directory already exists with the needed files
        # in it, but with no nested .git of its own. "git -C $target" in
        # that case doesn't fail outright - it walks up and silently
        # operates on *this* repo's own .git instead (fetching this
        # project's own "origin", then failing to check out the
        # dependency's pinned hash, since that commit obviously isn't in
        # this repo's object database) - functionally a harmless no-op
        # since the needed files are already correct, but it prints
        # confusing "fatal: reference is not a tree" errors that look like
        # a real failure. Skip it outright instead.
        Write-Host "Skipping $($dependency.Path) - already present from this repo's own git history, not a separate pinned clone."
        continue
    }
    if (-not (Test-Path $target)) {
        git clone $dependency.Url $target
    }
    git -C $target fetch --tags origin
    $ref = if ($AllowUnpinned) { 'origin/master' } else { $dependency.Commit }
    git -C $target checkout --detach $ref
    # ygopro-core vendors lua/src as a git submodule (see its .gitmodules) -
    # a plain "git clone" leaves that directory completely empty, since
    # submodule content is never fetched implicitly. That's invisible on any
    # machine that already has a properly-initialized checkout sitting
    # around from before (every local dev session so far), but on a
    # genuinely fresh clone (any CI run) it leaves
    # external/ygopro-core/lua/src with zero files, which CMakeLists.txt's
    # file(GLOB ... lua/src/*.c) then silently turns into "No SOURCES given
    # to target: ygopro_lua" - a confusing error that looks nothing like a
    # missing-submodule problem. Run unconditionally (a no-op on
    # dependencies without submodules) rather than special-casing
    # ygopro-core, since that's one less thing to keep in sync if another
    # dependency ever gains a submodule of its own.
    git -C $target submodule update --init --recursive
}
