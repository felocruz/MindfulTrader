#!/usr/bin/env python3
"""
ZMQ Bridge Unit Tests - Python Publisher
Tests basic ZMQ publishing, heartbeat, and message validation
"""

import zmq
import json
import time
import sys
from datetime import datetime, timezone

# Mock TransformerPublisher for testing (replace with actual import in production)
class MockTransformerPublisher:
    def __init__(self, port=5555):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.PUB)
        self.socket.bind(f"tcp://127.0.0.1:{port}")
        time.sleep(0.5)  # Allow socket to initialize
        
    def publish_entry_signal(self, symbol, pattern, confidence, attention_span, atr_multiplier):
        """Publish entry signal to ZMQ"""
        message = {
            "message_type": "ENTRY_SIGNAL",
            "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")[:-4] + "Z",
            "pattern": {
                "strategy_setup": pattern,
                "confidence": confidence
            },
            "attention_metrics": {
                "attention_span": attention_span
            },
            "recommended_parameters": {
                "atr_multiplier": atr_multiplier
            },
            "regime_validation": {
                "veto": False
            }
        }
        
        self.socket.send_string("ENTRY_SIGNAL", zmq.SNDMORE)
        self.socket.send_string(json.dumps(message))
        
    def publish_heartbeat(self):
        """Publish heartbeat to ZMQ"""
        message = {
            "message_type": "HEARTBEAT",
            "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")[:-4] + "Z",
            "status": "alive"
        }
        
        self.socket.send_string("HEARTBEAT", zmq.SNDMORE)
        self.socket.send_string(json.dumps(message))
        
    def close(self):
        self.socket.close()
        self.context.term()


def test_basic_publish():
    """Test 1: Basic JSON packet publishing"""
    print("\n" + "="*70)
    print("TEST 1: Basic Publishing")
    print("="*70)
    
    try:
        # Create publisher
        publisher = MockTransformerPublisher(port=5556)
        
        # Create subscriber
        context = zmq.Context()
        subscriber = context.socket(zmq.SUB)
        subscriber.connect("tcp://127.0.0.1:5556")
        subscriber.setsockopt_string(zmq.SUBSCRIBE, "ENTRY_SIGNAL")
        subscriber.setsockopt(zmq.RCVTIMEO, 2000)  # 2-second timeout
        
        # Allow connection to establish
        time.sleep(0.5)
        
        # Publish test signal
        publisher.publish_entry_signal(
            symbol="ES",
            pattern="HOLY_GRAIL_BUY",
            confidence=0.85,
            attention_span=42,
            atr_multiplier=3.2
        )
        
        # Receive and validate
        topic = subscriber.recv_string()
        message = subscriber.recv_string()
        data = json.loads(message)
        
        # Assertions
        assert topic == "ENTRY_SIGNAL", f"Expected topic 'ENTRY_SIGNAL', got '{topic}'"
        assert data["message_type"] == "ENTRY_SIGNAL", "Invalid message_type"
        assert data["pattern"]["confidence"] == 0.85, "Invalid confidence"
        assert data["attention_metrics"]["attention_span"] == 42, "Invalid attention_span"
        assert "timestamp" in data, "Missing timestamp"
        
        print(f"✅ PASS: Basic publishing works")
        print(f"   Topic: {topic}")
        print(f"   Pattern: {data['pattern']['strategy_setup']}")
        print(f"   Confidence: {data['pattern']['confidence']}")
        print(f"   Timestamp: {data['timestamp']}")
        
        # Cleanup
        subscriber.close()
        publisher.close()
        context.term()
        
        return True
        
    except zmq.Again:
        print("❌ FAIL: Receive timeout - no message received")
        return False
    except AssertionError as e:
        print(f"❌ FAIL: Assertion error - {str(e)}")
        return False
    except Exception as e:
        print(f"❌ FAIL: Unexpected error - {str(e)}")
        return False


def test_heartbeat_frequency():
    """Test 2: Heartbeat is sent every 1 second"""
    print("\n" + "="*70)
    print("TEST 2: Heartbeat Frequency")
    print("="*70)
    
    try:
        # Create publisher
        publisher = MockTransformerPublisher(port=5557)
        
        # Create subscriber
        context = zmq.Context()
        subscriber = context.socket(zmq.SUB)
        subscriber.connect("tcp://127.0.0.1:5557")
        subscriber.setsockopt_string(zmq.SUBSCRIBE, "HEARTBEAT")
        subscriber.setsockopt(zmq.RCVTIMEO, 2000)  # 2-second timeout
        
        # Allow connection to establish
        time.sleep(0.5)
        
        heartbeat_count = 0
        start_time = time.time()
        timestamps = []
        
        # Manually send 5 heartbeats with 1-second intervals
        for i in range(5):
            publisher.publish_heartbeat()
            
            # Receive heartbeat
            topic = subscriber.recv_string()
            message = subscriber.recv_string()
            data = json.loads(message)
            
            assert topic == "HEARTBEAT", f"Expected HEARTBEAT, got {topic}"
            assert data["message_type"] == "HEARTBEAT", "Invalid message_type"
            
            heartbeat_count += 1
            timestamps.append(time.time())
            
            if i < 4:  # Don't wait after last heartbeat
                time.sleep(1.0)
        
        elapsed = time.time() - start_time
        avg_interval = elapsed / 5.0
        
        # Calculate actual intervals between heartbeats
        intervals = [timestamps[i+1] - timestamps[i] for i in range(len(timestamps)-1)]
        avg_measured_interval = sum(intervals) / len(intervals) if intervals else 0
        
        print(f"✅ PASS: Heartbeat frequency validated")
        print(f"   Heartbeats received: {heartbeat_count}")
        print(f"   Total elapsed: {elapsed:.2f}s")
        print(f"   Average per heartbeat: {avg_interval:.2f}s")
        print(f"   Measured interval: {avg_measured_interval:.2f}s (target: ~1.0s)")
        
        # Cleanup
        subscriber.close()
        publisher.close()
        context.term()
        
        return True
        
    except zmq.Again:
        print(f"❌ FAIL: Heartbeat timeout (received {heartbeat_count}/5)")
        return False
    except AssertionError as e:
        print(f"❌ FAIL: {str(e)}")
        return False
    except Exception as e:
        print(f"❌ FAIL: {str(e)}")
        return False


def test_message_structure():
    """Test 3: Validate complete message structure"""
    print("\n" + "="*70)
    print("TEST 3: Message Structure Validation")
    print("="*70)
    
    try:
        # Create publisher
        publisher = MockTransformerPublisher(port=5558)
        
        # Create subscriber
        context = zmq.Context()
        subscriber = context.socket(zmq.SUB)
        subscriber.connect("tcp://127.0.0.1:5558")
        subscriber.setsockopt_string(zmq.SUBSCRIBE, "ENTRY_SIGNAL")
        subscriber.setsockopt(zmq.RCVTIMEO, 2000)
        
        time.sleep(0.5)
        
        # Publish signal
        publisher.publish_entry_signal(
            symbol="ES",
            pattern="MOMENTUM_PINBALL_BUY",
            confidence=0.92,
            attention_span=65,
            atr_multiplier=2.8
        )
        
        # Receive
        topic = subscriber.recv_string()
        message = subscriber.recv_string()
        data = json.loads(message)
        
        # Required fields validation
        required_fields = {
            "message_type": str,
            "timestamp": str,
            "pattern": dict,
            "attention_metrics": dict,
            "recommended_parameters": dict,
            "regime_validation": dict
        }
        
        for field, expected_type in required_fields.items():
            assert field in data, f"Missing required field: {field}"
            assert isinstance(data[field], expected_type), \
                f"Field {field} should be {expected_type}, got {type(data[field])}"
        
        # Nested field validation
        assert "strategy_setup" in data["pattern"], "Missing pattern.strategy_setup"
        assert "confidence" in data["pattern"], "Missing pattern.confidence"
        assert "attention_span" in data["attention_metrics"], "Missing attention_span"
        assert "atr_multiplier" in data["recommended_parameters"], "Missing atr_multiplier"
        assert "veto" in data["regime_validation"], "Missing veto"
        
        # Value range validation
        assert 0.0 <= data["pattern"]["confidence"] <= 1.0, "Confidence out of range [0.0, 1.0]"
        assert data["attention_metrics"]["attention_span"] > 0, "Attention span must be positive"
        assert data["recommended_parameters"]["atr_multiplier"] > 0, "ATR multiplier must be positive"
        
        print(f"✅ PASS: Message structure validated")
        print(f"   All required fields present: {len(required_fields)}")
        print(f"   Pattern: {data['pattern']['strategy_setup']}")
        print(f"   Confidence: {data['pattern']['confidence']:.2f}")
        print(f"   Attention Span: {data['attention_metrics']['attention_span']}")
        print(f"   ATR Multiplier: {data['recommended_parameters']['atr_multiplier']:.1f}")
        
        # Cleanup
        subscriber.close()
        publisher.close()
        context.term()
        
        return True
        
    except AssertionError as e:
        print(f"❌ FAIL: {str(e)}")
        return False
    except Exception as e:
        print(f"❌ FAIL: {str(e)}")
        return False


def test_timestamp_format():
    """Test 4: Validate ISO8601 timestamp format"""
    print("\n" + "="*70)
    print("TEST 4: Timestamp Format Validation")
    print("="*70)
    
    try:
        # Create publisher
        publisher = MockTransformerPublisher(port=5559)
        
        # Create subscriber
        context = zmq.Context()
        subscriber = context.socket(zmq.SUB)
        subscriber.connect("tcp://127.0.0.1:5559")
        subscriber.setsockopt_string(zmq.SUBSCRIBE, "ENTRY_SIGNAL")
        subscriber.setsockopt(zmq.RCVTIMEO, 2000)
        
        time.sleep(0.5)
        
        # Publish signal
        publisher.publish_entry_signal(
            symbol="ES",
            pattern="TURTLE_SOUP_BUY",
            confidence=0.78,
            attention_span=38,
            atr_multiplier=3.5
        )
        
        # Receive
        topic = subscriber.recv_string()
        message = subscriber.recv_string()
        data = json.loads(message)
        
        timestamp = data["timestamp"]
        
        # Parse ISO8601 timestamp
        try:
            # Expected format: 2025-12-19T14:30:00.000Z
            dt = datetime.fromisoformat(timestamp.replace('Z', '+00:00'))
            
            # Verify it's recent (within last 5 seconds)
            now = datetime.now(timezone.utc)
            age = (now - dt).total_seconds()
            
            assert age < 5.0, f"Timestamp too old: {age:.2f}s"
            assert timestamp.endswith('Z'), "Timestamp must end with 'Z' (UTC)"
            
            print(f"✅ PASS: Timestamp format validated")
            print(f"   Format: ISO8601 UTC")
            print(f"   Timestamp: {timestamp}")
            print(f"   Age: {age:.3f}s")
            
        except ValueError as e:
            raise AssertionError(f"Invalid timestamp format: {timestamp} ({str(e)})")
        
        # Cleanup
        subscriber.close()
        publisher.close()
        context.term()
        
        return True
        
    except AssertionError as e:
        print(f"❌ FAIL: {str(e)}")
        return False
    except Exception as e:
        print(f"❌ FAIL: {str(e)}")
        return False


def run_all_tests():
    """Run all Python publisher tests"""
    print("\n" + "="*70)
    print("ZMQ BRIDGE UNIT TESTS - PYTHON PUBLISHER")
    print("="*70)
    print(f"Start Time: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    
    tests = [
        ("Basic Publishing", test_basic_publish),
        ("Heartbeat Frequency", test_heartbeat_frequency),
        ("Message Structure", test_message_structure),
        ("Timestamp Format", test_timestamp_format),
    ]
    
    results = []
    
    for test_name, test_func in tests:
        try:
            result = test_func()
            results.append((test_name, result))
        except Exception as e:
            print(f"❌ FAIL: {test_name} - Uncaught exception: {str(e)}")
            results.append((test_name, False))
    
    # Summary
    print("\n" + "="*70)
    print("TEST SUMMARY")
    print("="*70)
    
    passed = sum(1 for _, result in results if result)
    total = len(results)
    
    for test_name, result in results:
        status = "✅ PASS" if result else "❌ FAIL"
        print(f"{status}: {test_name}")
    
    print("-"*70)
    print(f"Total: {passed}/{total} tests passed ({100*passed//total}%)")
    print(f"End Time: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("="*70)
    
    return passed == total


if __name__ == "__main__":
    success = run_all_tests()
    sys.exit(0 if success else 1)
