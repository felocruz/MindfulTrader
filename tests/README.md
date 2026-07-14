# ZMQ Bridge Testing - Quick Start Guide

## Test Suite Created ✅

The ZMQ Bridge unit test suite has been successfully created in `/tests/` directory:

```
tests/
├── python/
│   └── test_transformer_publisher.py  # 4 comprehensive tests
├── cpp/                                # (To be implemented)
├── integration/                        # (To be implemented)
└── run_python_tests.sh                # Test runner script
```

---

## Running Python Tests

### Option 1: Run All Tests (Recommended)
```bash
cd /home/rcruz/devel/VSCode/MindfulTrader/tests
./run_python_tests.sh
```

### Option 2: Run Tests Directly
```bash
cd /home/rcruz/devel/VSCode/MindfulTrader/tests
python3 python/test_transformer_publisher.py
```

---

## Test Coverage

### ✅ Test 1: Basic Publishing
- **Objective**: Verify ZMQ publisher can send valid JSON packets
- **Validates**: Topic routing, JSON serialization, message receipt
- **Port**: 5556

### ✅ Test 2: Heartbeat Frequency
- **Objective**: Verify heartbeat messages sent every ~1 second
- **Validates**: Heartbeat timing, message format, continuous operation
- **Port**: 5557

### ✅ Test 3: Message Structure
- **Objective**: Validate complete message schema
- **Validates**: All required fields, nested objects, value ranges
- **Port**: 5558

### ✅ Test 4: Timestamp Format
- **Objective**: Verify ISO8601 UTC timestamp format
- **Validates**: Timestamp parsing, age validation, UTC timezone
- **Port**: 5559

---

## Prerequisites

### Required Python Packages
```bash
# Install ZMQ library
pip3 install pyzmq

# Verify installation
python3 -c "import zmq; print(f'PyZMQ {zmq.pyzmq_version()}')"
```

### System Requirements
- Python 3.9+
- ZMQ library (libzmq)
- Ports 5556-5559 available (localhost only)

---

## Expected Test Output

```
================================================================================
ZMQ BRIDGE UNIT TESTS - PYTHON PUBLISHER
================================================================================
Start Time: 2025-12-19 14:30:00

======================================================================
TEST 1: Basic Publishing
======================================================================
✅ PASS: Basic publishing works
   Topic: ENTRY_SIGNAL
   Pattern: HOLY_GRAIL_BUY
   Confidence: 0.85
   Timestamp: 2025-12-19T14:30:00.123Z

======================================================================
TEST 2: Heartbeat Frequency
======================================================================
✅ PASS: Heartbeat frequency validated
   Heartbeats received: 5
   Total elapsed: 4.52s
   Average per heartbeat: 0.90s
   Measured interval: 1.01s (target: ~1.0s)

======================================================================
TEST 3: Message Structure Validation
======================================================================
✅ PASS: Message structure validated
   All required fields present: 6
   Pattern: MOMENTUM_PINBALL_BUY
   Confidence: 0.92
   Attention Span: 65
   ATR Multiplier: 2.8

======================================================================
TEST 4: Timestamp Format Validation
======================================================================
✅ PASS: Timestamp format validated
   Format: ISO8601 UTC
   Timestamp: 2025-12-19T14:30:05.456Z
   Age: 0.023s

======================================================================
TEST SUMMARY
======================================================================
✅ PASS: Basic Publishing
✅ PASS: Heartbeat Frequency
✅ PASS: Message Structure
✅ PASS: Timestamp Format
----------------------------------------------------------------------
Total: 4/4 tests passed (100%)
End Time: 2025-12-19 14:30:10
======================================================================
```

---

## Troubleshooting

### Port Conflicts
If tests fail with "Address already in use":
```bash
# Check what's using ZMQ ports
netstat -an | grep 555[6-9]

# Kill competing processes if needed
killall python3
```

### Missing pyzmq
If you see "ModuleNotFoundError: No module named 'zmq'":
```bash
# Install with pip
pip3 install pyzmq

# Or with conda
conda install pyzmq
```

### Permission Denied
If the test runner won't execute:
```bash
chmod +x /home/rcruz/devel/VSCode/MindfulTrader/tests/run_python_tests.sh
```

---

## Next Steps After Python Tests Pass

1. **Implement C++ Subscriber Tests**
   - Create `tests/cpp/test_transformer_bridge.cpp`
   - Test JSON parsing with nlohmann/json
   - Test alpha decay logic (10-second staleness threshold)
   - Test heartbeat timeout detection (5-second threshold)

2. **Integration Tests**
   - Round-trip latency measurement (target: <10ms avg)
   - Heartbeat failover simulation
   - Concurrent signal handling
   - Stress test: 100 msg/s burst

3. **Production Health Monitoring**
   - Daily health check script
   - Grafana dashboard integration
   - Alert thresholds (packet loss, latency spikes)

4. **Paper Trading Validation**
   - 2-week minimum with all tests passing
   - Track veto accuracy, AI signal quality
   - Validate spread capture metrics

---

## Test Architecture Notes

### Mock vs. Real Publisher
The current tests use `MockTransformerPublisher` for isolated testing. In production:
- Replace mock with actual `TransformerPublisher` from your Python ML code
- Ensure 28-feature vector generation is included
- Wire real Transformer model predictions

### Port Configuration
Tests use ports 5556-5559 to avoid conflicts with production (5555). Production setup:
```python
# Production port
PRODUCTION_PORT = 5555

# Test ports
TEST_PORT_BASIC = 5556
TEST_PORT_HEARTBEAT = 5557
TEST_PORT_STRUCTURE = 5558
TEST_PORT_TIMESTAMP = 5559
```

### Isolation Guarantee
Each test uses a separate port to ensure complete isolation. Tests can run in parallel without interference.

---

## Test Metrics

| Test | Port | Duration | Critical Path |
|------|------|----------|---------------|
| Basic Publishing | 5556 | ~1s | ZMQ socket creation, message serialization |
| Heartbeat Frequency | 5557 | ~5s | Thread timing, heartbeat interval accuracy |
| Message Structure | 5558 | ~1s | JSON schema validation, field presence |
| Timestamp Format | 5559 | ~1s | ISO8601 parsing, age validation |

**Total Suite Runtime**: ~8-10 seconds

---

## Documentation References

- **ZMQ_BRIDGE_UNIT_TEST_PROTOCOL.md**: Complete testing specification (~4,500 lines)
- **TRANSFORMER_CPP_INTEGRATION_SPEC.md**: Elite refinements with alpha decay, heartbeat (~15,000 lines)
- **ELITE_EXECUTION_ENHANCEMENTS.md**: Institutional execution intelligence (~6,000 lines)

---

## Success Criteria

✅ **Ready for C++ Integration** when:
1. All 4 Python tests pass consistently
2. No port conflicts or resource leaks
3. Heartbeat timing within ±100ms of 1.0s target
4. Message latency <2ms localhost
5. Zero test flakiness over 10 consecutive runs

---

**Status**: Python publisher tests complete and ready to run
**Next**: Execute test suite and validate ZMQ communication layer
