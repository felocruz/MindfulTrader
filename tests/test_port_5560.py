#!/usr/bin/env python3
"""
Test client for Port 5560 - Master Controller handshake
Tests the CONFIG_REQ/CONFIG_ACK negotiation flow
"""

import zmq
import json
import time

def test_handshake():
    """Test CONFIG_REQ/CONFIG_ACK handshake"""
    
    # For WSL → Windows: Get Windows host IP
    import subprocess
    try:
        windows_ip = subprocess.check_output(
            "ip route show | grep -i default | awk '{ print $3}'",
            shell=True
        ).decode().strip()
    except:
        windows_ip = "172.24.144.1"  # Fallback WSL2 default
    
    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.connect(f"tcp://{windows_ip}:5560")
    socket.setsockopt(zmq.RCVTIMEO, 500)  # 500ms timeout per attempt
    
    print(f"🔌 Connecting to Windows host at {windows_ip}:5560")
    
    print("=" * 60)
    print("Port 5560 Master Controller Handshake Test")
    print("=" * 60)
    
    # Build CONFIG_REQ message
    request = {
        "header": {
            "msg_type": "CONFIG_REQ",
            "version": "1.0.2",
            "timestamp_ns": time.time_ns(),
            "sender": "PYTHON_TEST_CLIENT"
        },
        "payload": {
            "component_name": "TEST_GUI",
            "capabilities": [
                "indicator_display",
                "manual_trade_entry",
                "chart_visualization"
            ]
        }
    }
    
    print("\n📤 Sending CONFIG_REQ (will retry until response)...")
    print(json.dumps(request, indent=2))
    
    # Retry loop: Sierra Chart only polls during bar updates
    max_retries = 20  # 20 attempts * 500ms = 10s total
    for attempt in range(1, max_retries + 1):
        try:
            # Send request
            print(f"\n⏳ Attempt {attempt}/{max_retries}: Waiting for CONFIG_ACK...")
            socket.send_string(json.dumps(request))
            
            # Try to receive response (500ms timeout)
            response_str = socket.recv_string()
            response = json.loads(response_str)
            break  # Success - exit retry loop
            
        except zmq.Again:
            # Timeout - retry
            if attempt == max_retries:
                print(f"\n🔴 TIMEOUT: C++ did not respond after {max_retries} attempts")
                print("   Is Sierra Chart running with bars updating?")
                return False
            # Close and recreate socket for next attempt (REQ/REP state machine)
            socket.close()
            socket = context.socket(zmq.REQ)
            socket.connect(f"tcp://{windows_ip}:5560")
            socket.setsockopt(zmq.RCVTIMEO, 500)
            continue
    
    try:
        
        print("\n📥 Received response:")
        print(json.dumps(response, indent=2))
        
        # Validate response
        if response["header"]["msg_type"] == "ERROR":
            error = response["payload"]
            print(f"\n🔴 ERROR: {error['error_code']}")
            print(f"   Message: {error['error_message']}")
            return False
        
        if response["header"]["msg_type"] == "CONFIG_ACK":
            payload = response["payload"]
            print(f"\n✅ Handshake successful!")
            print(f"   Status: {payload['negotiation_status']}")
            print(f"   State: {payload['current_state']}")
            print(f"   Ports: {payload['available_ports']}")
            print(f"   Config: {payload['config']}")
            return True
        
        print(f"\n⚠️ Unexpected message type: {response['header']['msg_type']}")
        return False
        
    except Exception as e:
        print(f"\n🔴 ERROR: {e}")
        return False
    finally:
        socket.close()
        context.term()

def test_version_mismatch():
    """Test version mismatch rejection"""
    
    # For WSL → Windows: Get Windows host IP
    import subprocess
    try:
        windows_ip = subprocess.check_output(
            "ip route show | grep -i default | awk '{ print $3}'",
            shell=True
        ).decode().strip()
    except:
        windows_ip = "172.24.144.1"  # Fallback WSL2 default
    
    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.connect(f"tcp://{windows_ip}:5560")
    socket.setsockopt(zmq.RCVTIMEO, 5000)
    
    print("\n" + "=" * 60)
    print("Testing Version Mismatch (Major Version Incompatible)")
    print("=" * 60)
    
    # Send with incompatible major version
    request = {
        "header": {
            "msg_type": "CONFIG_REQ",
            "version": "2.0.0",  # Major version 2 (incompatible)
            "timestamp_ns": time.time_ns(),
            "sender": "PYTHON_TEST_CLIENT"
        },
        "payload": {
            "component_name": "TEST_GUI",
            "capabilities": ["indicator_display"]
        }
    }
    
    print("\n📤 Sending CONFIG_REQ with version 2.0.0...")
    
    try:
        socket.send_string(json.dumps(request))
        response_str = socket.recv_string()
        response = json.loads(response_str)
        
        if response["header"]["msg_type"] == "ERROR":
            error = response["payload"]
            print(f"\n✅ Version rejected as expected:")
            print(f"   Code: {error['error_code']}")
            print(f"   Message: {error['error_message']}")
            print(f"   Required: {error['required_version']}")
            print(f"   Provided: {error['provided_version']}")
            return True
        else:
            print(f"\n⚠️ Expected ERROR, got {response['header']['msg_type']}")
            return False
            
    except Exception as e:
        print(f"\n🔴 ERROR: {e}")
        return False
    finally:
        socket.close()
        context.term()

if __name__ == "__main__":
    print("\n🚀 Starting Port 5560 Tests\n")
    
    # Test 1: Normal handshake
    test1_passed = test_handshake()
    
    time.sleep(1)
    
    # Test 2: Version mismatch (only if test 1 passed)
    # test2_passed = test_version_mismatch()
    
    print("\n" + "=" * 60)
    print("Test Results:")
    print("=" * 60)
    print(f"✅ Test 1 (Normal Handshake): {'PASSED' if test1_passed else 'FAILED'}")
    # print(f"✅ Test 2 (Version Mismatch): {'PASSED' if test2_passed else 'FAILED'}")
    print()
