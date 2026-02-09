# Git Submodule Approach Status

## Summary

Successfully switched from pure Nix flake approach to git submodule approach for `logos-messaging-nim`.

## What Works

✅ **Git Submodule Setup**
- Added `vendor/logos-messaging-nim` tracking `feat-lmapi-lib` branch
- Configured `.gitmodules` with `fetchRecurseSubmodules = true`
- All 40+ nested nim dependencies initialized successfully
- Total: 43,845 objects cloned (32.56 MiB for main + dependencies)

✅ **Local Build**
- `make liblogosdelivery` in `vendor/logos-messaging-nim` works perfectly
- Produces `build/liblogosdelivery.dylib` (35MB) successfully
- All dependencies resolve correctly
- Build time: ~2-3 minutes on M1 Mac

✅ **Header Files**
- `liblogosdelivery/liblogosdelivery.h` available in submodule
- Contains all required API functions:
  - `logosdelivery_create_node`, `logosdelivery_destroy`
  - `logosdelivery_start_node`, `logosdelivery_stop_node`
  - `logosdelivery_send` (JSON)
  - `logosdelivery_subscribe`, `logosdelivery_unsubscribe`
  - `logosdelivery_set_event_callback`

## What's Blocked

❌ **Nix Flake Integration**
- **Root Cause**: Nix flakes in Git mode don't copy submodules into the store
- When the flake evaluates, it copies the parent repository but not submodule contents
- `vendor/logos-messaging-nim` becomes an empty directory in `/nix/store/...`
- Attempted solutions:
  1. `path:./vendor/...` - Requires files to be Git-tracked in parent repo
  2. `git+file:./vendor/...` - Triggers recursive fetch of problematic dependencies (lsquic)
  3. `builtins.path` - Doesn't work from flake evaluation context
  4. `--impure` flag - Store path still doesn't contain submodule

## Possible Solutions

### Option 1: Use `?dir=submodule` (NOT VIABLE)
Nix flakes don't support building from subdirectories of a flake repo.

### Option 2: Copy Submodule into Nix Store Manually (COMPLEX)
Create custom derivation that:
1. Manually copies submodule contents into store
2. Triggers full nim build within derivation
3. Issues: Need to handle all 40+ nested submodules, large closure size

### Option 3: Hybrid Build (RECOMMENDED FOR NOW)
Keep submodule for development, use local build artifacts:
```bash
# Developer workflow:
git clone --recursive https://github.com/logos-co/logos-delivery-module
cd logos-delivery-module
make -C vendor/logos-messaging-nim liblogosdelivery  # Build liblogosdelivery locally
nix build  # Build module using pre-built library
```

This requires:
- Updating CMakeLists.txt to find library in vendor/logos-messaging-nim/build/
- Treating liblogosdelivery as a "pre-built dependency" during Nix builds
- Similar to how we'd handle vendored binaries

### Option 4: Vendor Pre-built Binaries (AGAINST ORIGINAL GOAL)
- Build liblogosdelivery on CI for each platform
- Commit binaries to repo
- Contradicts original "pure nix dependencies" requirement

### Option 5: Wait for Nix Flake Submodule Support
- Track: https://github.com/NixOS/nix/issues/4423
- Not currently on roadmap

## Files Modified

1. `.gitmodules` - Configure recursive submodule fetch
2. `flake.nix` - Removed GitHub input, attempted path-based input
3. `flake.lock` - Updated to remove logos-messaging-nim flake dependencies

## Commits

- `e2982bc` - Switch to git submodule approach
- `181fff8` - Create custom Nix derivation
- `637e468` - Use builtins.path (attempted fix)

## Next Steps

Need user decision on which solution to pursue:
1. Hybrid build with local artifacts?
2. Complex manual copying approach?
3. Vendor pre-built binaries (defeats original purpose)?
4. Keep investigating Nix solutions?

## Technical Details

**Submodule Info:**
- URL: https://github.com/logos-messaging/logos-messaging-nim
- Branch: feat-lmapi-lib
- Commit: ec77e5dd
- Nested submodules: 42 (including nimbus-build-system, nim-* libraries)

**Build System:**
- Makefile target: `liblogosdelivery`
- Compiler: Nim 2.2+ (from nimbus-build-system)
- Dependencies: openssl, gmp, libbacktrace, BearSSL, zerokit/RLN
- Output: Dynamic library (.dylib on macOS, .so on Linux)
- Location: `build/liblogosdelivery.{dylib,so}`

**Library Size:**
- macOS ARM64: 35MB (liblogosdelivery.dylib)
- Includes all transitive nim dependencies compiled in

