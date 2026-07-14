#!/bin/bash
# ZMQ Bridge Test Runner
# Executes all Python unit tests for the Transformer Publisher

set -e  # Exit on error

echo "================================================================================"
echo "ZMQ BRIDGE TEST SUITE - PYTHON PUBLISHER"
echo "================================================================================"
echo "Date: $(date)"
echo "Platform: $(uname -s)"
echo "Python: $(python3 --version 2>&1)"
echo "================================================================================"
echo ""

# Check if pyzmq is installed
echo "Checking dependencies..."
if ! python3 -c "import zmq" 2>/dev/null; then
    echo "❌ ERROR: pyzmq not installed"
    echo "Install with: pip3 install pyzmq"
    exit 1
else
    ZMQ_VERSION=$(python3 -c "import zmq; print(zmq.zmq_version())")
    PYZMQ_VERSION=$(python3 -c "import zmq; print(zmq.pyzmq_version())")
    echo "✅ ZMQ version: $ZMQ_VERSION"
    echo "✅ PyZMQ version: $PYZMQ_VERSION"
fi

# Check for json (built-in, but verify)
if ! python3 -c "import json" 2>/dev/null; then
    echo "❌ ERROR: json module not available"
    exit 1
else
    echo "✅ json module available"
fi

echo ""
echo "================================================================================"
echo "RUNNING TESTS"
echo "================================================================================"
echo ""

# Run Python publisher tests
cd "$(dirname "$0")/.."  # Go to tests/ directory
python3 python/test_transformer_publisher.py
TEST_EXIT_CODE=$?

echo ""
echo "================================================================================"
echo "TEST RESULTS"
echo "================================================================================"

if [ $TEST_EXIT_CODE -eq 0 ]; then
    echo "✅ ALL TESTS PASSED"
    echo ""
    echo "Next steps:"
    echo "1. Implement C++ subscriber tests (tests/cpp/)"
    echo "2. Run integration tests (tests/integration/)"
    echo "3. Deploy to paper trading environment"
    exit 0
else
    echo "❌ TESTS FAILED (Exit code: $TEST_EXIT_CODE)"
    echo ""
    echo "Troubleshooting:"
    echo "1. Check ZMQ port conflicts (5556-5559)"
    echo "2. Verify firewall allows localhost ZMQ"
    echo "3. Review test output above for specific failures"
    exit $TEST_EXIT_CODE
fi
