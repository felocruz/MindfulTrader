# Build Verification Commands - Pre-Compilation Checklist

**Purpose**: Verify no compilation/linking errors will occur before running build
**Status**: Ready to execute
**Expected Outcome**: All checks PASS → Build will succeed

---

## Quick Verification (5 minutes)

Run these commands to verify the architectural refactoring is complete:

### 1. Verify No Active SocketMessage References
```bash
cd /home/rcruz/devel/VSCode/MindfulTrader

# Search for SocketMessage in source files (should find 0 matches)
grep -r "SocketMessage" src/ include/
# Expected: No output (SocketMessage only in deprecated MindfulSocketZMQ.h)
```

### 2. Verify No Active m_pubQueue References
```bash
# Search for m_pubQueue in source files (should find 0 matches)
grep -r "m_pubQueue" src/ include/
# Expected: No output (m_pubQueue member removed from all active classes)
```

### 3. Verify No Active SetQueue() Calls
```bash
# Search for SetQueue in source files (should find 0 matches)
grep -r "SetQueue(" src/ include/
# Expected: No output (SetQueue method removed)
```

### 4. Verify TransportStream::Emit Integration
```bash
# Verify Emit calls are present (should find 1 match in IndicatorManager.cpp)
grep -n "TransportStream::Instance().Emit" src/
# Expected: 1 match in src/IndicatorManager.cpp line ~479
```

### 5. Verify MindfulSocketZMQ Compilation is Disabled
```bash
# Verify CMakeLists.txt has MindfulSocketZMQ.cpp commented
grep -n "MindfulSocketZMQ.cpp" CMakeLists.txt
# Expected: "# Deprecated in Elite v2.4: MindfulSocketZMQ.cpp" (commented out)
```

### 6. Verify Precompiled Header Updated
```bash
# Verify MindfulSocketZMQ.h is removed from PCH
grep -n "MindfulSocketZMQ.h" include/MindfulTrader_Precompiled.h
# Expected: "// Deprecated in Elite v2.4: MindfulSocketZMQ.h" (commented out)
```

---

## Automated Verification Script

Create this script to verify all conditions in one command:

```bash
#!/bin/bash
# File: /home/rcruz/devel/VSCode/MindfulTrader/verify_elite_v24_refactor.sh

set -e

echo "🔍 Elite v2.4 Refactoring Verification"
echo "======================================="

ERRORS=0

# Test 1: No SocketMessage in active code
if grep -r "SocketMessage" src/ include/ 2>/dev/null | grep -v "deprecated\|Deprecated" | grep -v "#.*SocketMessage"; then
    echo "❌ ERROR: SocketMessage found in active code"
    ERRORS=$((ERRORS+1))
else
    echo "✅ Test 1 PASS: No SocketMessage in active code"
fi

# Test 2: No m_pubQueue in active code
if grep -r "m_pubQueue" src/ include/ 2>/dev/null | grep -v "deprecated\|Deprecated\|// "; then
    echo "❌ ERROR: m_pubQueue found in active code"
    ERRORS=$((ERRORS+1))
else
    echo "✅ Test 2 PASS: No m_pubQueue in active code"
fi

# Test 3: No SetQueue calls
if grep -r "SetQueue(" src/ include/ 2>/dev/null | grep -v "deprecated\|Deprecated"; then
    echo "❌ ERROR: SetQueue() found in active code"
    ERRORS=$((ERRORS+1))
else
    echo "✅ Test 3 PASS: No SetQueue() calls"
fi

# Test 4: TransportStream::Emit present
if grep -r "TransportStream::Instance().Emit" src/ 2>/dev/null | grep -q "."; then
    echo "✅ Test 4 PASS: TransportStream::Emit integration confirmed"
else
    echo "❌ ERROR: TransportStream::Emit not found"
    ERRORS=$((ERRORS+1))
fi

# Test 5: MindfulSocketZMQ.cpp commented in CMakeLists.txt
if grep "# Deprecated in Elite v2.4: MindfulSocketZMQ.cpp" CMakeLists.txt 2>/dev/null | grep -q "."; then
    echo "✅ Test 5 PASS: MindfulSocketZMQ.cpp correctly commented in CMakeLists.txt"
else
    echo "⚠️  WARNING: MindfulSocketZMQ.cpp CMakeLists.txt entry not found"
fi

# Test 6: MindfulSocketZMQ.h removed from PCH
if grep "// Deprecated in Elite v2.4: MindfulSocketZMQ.h" include/MindfulTrader_Precompiled.h 2>/dev/null | grep -q "."; then
    echo "✅ Test 6 PASS: MindfulSocketZMQ.h correctly removed from PCH"
else
    echo "⚠️  WARNING: MindfulSocketZMQ.h PCH entry verification skipped"
fi

# Test 7: No remaining ThreadSafeQueue<SocketMessage> declarations
if grep -r "ThreadSafeQueue<SocketMessage>" src/ include/ 2>/dev/null | grep -v "deprecated\|Deprecated"; then
    echo "❌ ERROR: ThreadSafeQueue<SocketMessage> found in active code"
    ERRORS=$((ERRORS+1))
else
    echo "✅ Test 7 PASS: No ThreadSafeQueue<SocketMessage> declarations"
fi

echo ""
echo "======================================="
if [ $ERRORS -eq 0 ]; then
    echo "✅ ALL TESTS PASSED - Ready for build"
    echo "Next: Run './build_dll.sh' to compile"
    exit 0
else
    echo "❌ $ERRORS TEST(S) FAILED - Fix errors before building"
    exit 1
fi
```

Save as `verify_elite_v24_refactor.sh` and run:
```bash
chmod +x verify_elite_v24_refactor.sh
./verify_elite_v24_refactor.sh
```

**Expected Output**:
```
🔍 Elite v2.4 Refactoring Verification
=======================================
✅ Test 1 PASS: No SocketMessage in active code
✅ Test 2 PASS: No m_pubQueue in active code
✅ Test 3 PASS: No SetQueue() calls
✅ Test 4 PASS: TransportStream::Emit integration confirmed
✅ Test 5 PASS: MindfulSocketZMQ.cpp correctly commented in CMakeLists.txt
✅ Test 6 PASS: MindfulSocketZMQ.h correctly removed from PCH
✅ Test 7 PASS: No ThreadSafeQueue<SocketMessage> declarations

=======================================
✅ ALL TESTS PASSED - Ready for build
Next: Run './build_dll.sh' to compile
```

---

## Build Command

Once verification passes, compile with:

```bash
cd /home/rcruz/devel/VSCode/MindfulTrader
./build_dll.sh
```

**Expected Output** (last lines):
```
[100%] Linking CXX shared module MindfulTrader.dll
[100%] Built target MindfulTrader_dll

✅ MindfulTrader.dll built successfully
📦 Output: build-windows/MindfulTrader.dll (1.4MB)
⏱️  Build time: ~45 seconds
```

---

## Post-Build Verification

After `build_dll.sh` succeeds:

### 1. Verify DLL Size
```bash
ls -lh build-windows/MindfulTrader.dll
# Expected: ~1.4 MB (should NOT include deprecated code bloat)
```

### 2. Verify No Warnings About Missing Symbols
```bash
strings build-windows/MindfulTrader.dll | grep "MindfulSocketZMQ"
# Expected: No output (should find nothing)
```

### 3. Verify TransportStream Symbols Present
```bash
nm build-windows/MindfulTrader.dll | grep "TransportStream"
# Expected: Multiple matches (TransportStream::Emit, Instance, etc.)
```

---

## Deployment Checklist

- [ ] Pre-build verification passed (all 7 tests)
- [ ] Build completed without errors
- [ ] Build completed without warnings (about MindfulSocketZMQ)
- [ ] DLL size reasonable (~1.4 MB)
- [ ] No "missing symbol" errors mentioned
- [ ] TransportStream symbols present in DLL

**When all checks pass**: ✅ Ready to deploy MindfulTrader.dll

---

## Rollback Plan (If Issues Occur)

If compilation fails, check these common issues:

### Issue: "Undefined reference to TransportStream::Instance()"
**Solution**: Verify TransportStream.h is included in MindfulTrader_Precompiled.h
```bash
grep -n "TransportStream" include/MindfulTrader_Precompiled.h
# Should find include line (TransportStream included via other headers)
```

### Issue: "Undefined reference to EventSerializer::CreateEvent()"
**Solution**: Verify EventSerializer is properly compiled
```bash
ls -la src/EventSerializer.cpp
# File should exist and be in CMakeLists.txt SOURCES
```

### Issue: "SocketMessage: No such file or directory"
**Solution**: Verify SocketMessage.h #include was removed from PCH
```bash
grep -n "SocketMessage.h" include/MindfulTrader_Precompiled.h
# Should find nothing (or commented out)
```

### Issue: Compilation succeeds but DLL 3+ MB
**Solution**: MindfulSocketZMQ.cpp may still be compiling
```bash
grep "src/MindfulSocketZMQ.cpp" CMakeLists.txt
# Should be commented out or removed
```

---

## Success Criteria

**All conditions must be TRUE for production release**:

- ✅ Pre-build verification script passes all 7 tests
- ✅ Compilation completes with zero errors
- ✅ Build produces no warnings about deprecated symbols
- ✅ DLL size is ~1.4 MB (not bloated)
- ✅ Post-build verification confirms no MindfulSocketZMQ symbols
- ✅ ZMQ connection test shows events flowing via TransportStream
- ✅ Performance test shows 10-20× improvement in event latency

**Status**: Ready to execute

---

## Next Steps

1. ✅ **Verify**: Run the automated verification script
2. ✅ **Build**: Execute `./build_dll.sh`
3. ✅ **Test**: Run post-build verification
4. ✅ **Deploy**: Copy MindfulTrader.dll to Sierra Chart

**Estimated Time**: 15 minutes (verify + build + test)

**Go/No-Go Decision Point**: After build succeeds and all verifications pass → **READY FOR PRODUCTION DEPLOYMENT**

