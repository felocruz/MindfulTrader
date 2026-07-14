# build_dll.sh - Quick Reference

## What Was Fixed

### 5 Critical Issues Resolved ✅

1. **Platform detection** - Now works on both Linux and macOS
2. **No bc dependency** - Uses awk for math (faster, more compatible)
3. **Auto-initialization** - Builds build-windows directory if missing
4. **CMake diagnostics** - Shows full output on errors (was silent)
5. **Build logging** - Captures full build log for debugging

---

## Usage

```bash
# Standard build (clean + build)
cd /home/rcruz/devel/VSCode/MindfulTrader
./build_dll.sh

# Skip cleaning, just rebuild
./build_dll.sh --no-clean

# Clean only (no build)
./build_dll.sh --clean-only

# Use specific number of parallel jobs
./build_dll.sh --jobs 16
```

---

## Output Interpretation

### Success (Example)
```
[0/3] Initializing build directory...    # Auto-setup if needed
✓ Build directory initialized

[1/3] Cleaning old build files...        # PCH cleanup
✓ Removed CMakeCache.txt
✓ CMake reconfigured

[2/3] Building MindfulTrader.dll (8 jobs)
[100%] Built target MindfulTrader_DLL

✓ Build completed in 45s

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Build Successful! 🎉
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  📦 DLL Path:     bin/MindfulTrader.dll
  📊 Size:         1.42 MB
  📅 Timestamp:    2026-02-01 14:32:15
```

### Failure Handling
```
✗ Build failed. Last 30 lines of build output:

[Detailed compiler errors displayed here]

# Build log saved to: build-windows/build_output.log
```

---

## Build Artifacts

After successful build:
- **DLL**: `build-windows/bin/MindfulTrader.dll` (1.4MB typical)
- **Log**: `build-windows/build_output.log` (complete build diagnostics)

---

## Troubleshooting

### CMake configuration fails
- Check: `/mnt/c/Users/rcruz/vcpkg/installed/x64-windows/` exists
- Check: CMakePresets.json syntax is valid
- Solution: `cmake --preset wsl-clang-cl-release` (manual debug)

### Build fails with compiler errors
- Solution 1: Check `build-windows/build_output.log` for details
- Solution 2: Run `./build_dll.sh --clean-only` then rebuild
- Solution 3: Verify header file changes didn't break compilation

### PCH (Precompiled Header) issues
- Symptom: "file has been modified since PCH was built"
- Solution: `./build_dll.sh` (automatic PCH regeneration included)

---

## Next Steps After Building

```bash
# Option 1: Deploy to Windows
cd /home/rcruz/devel/VSCode/MindfulTrader
./deploy_mindfultrader.sh

# Option 2: Test Python pipeline
cd /home/rcruz/devel/VSCode/lbrnet
mamba run -n mts python scripts/collect_data.py \
  --input=data/raw/event_data.parquet \
  --output=data/training/test.parquet \
  --max-events=1000

# Option 3: Verify schema generation
cd /home/rcruz/devel/VSCode/schema
./regenerate_schema.sh
```

---

## Platform Compatibility

| OS | Status | Notes |
|----|--------|-------|
| **Linux (WSL)** | ✅ FIXED | Primary target, all fixes applied |
| **macOS** | ✅ Supported | OS detection handles both stat formats |
| **Windows (Git Bash)** | ✅ Likely works | stat should be available via Git Bash |

---

## Performance

| Metric | Before | After |
|--------|--------|-------|
| **Build time** | ~45-60s | ~45-60s (same, no overhead) |
| **Setup time** | Manual | Auto (Phase 0) |
| **Error clarity** | Silent failures | Full diagnostics |
| **bc dependency** | Required | Not needed |

---

## Related Documentation

- [BUILD_DLL_FIXES.md](BUILD_DLL_FIXES.md) - Detailed fix analysis
- [ARCHITECTURE.md](ARCHITECTURE.md) - Build system architecture
- [deploy_mindfultrader.sh](deploy_mindfultrader.sh) - Deployment script

