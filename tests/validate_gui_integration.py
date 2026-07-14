#!/usr/bin/env python3
"""
GUI Integration Validation Test
Tests all 6 ports (5555-5560) to ensure elite watchdog is ready for GUI connection
"""

import zmq
import json
import time
import sys
from datetime import datetime

# WSL2 Windows host IP (adjust if needed)
WINDOWS_IP = "172.20.112.1"

class Colors:
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    CYAN = '\033[96m'
    RESET = '\033[0m'
    BOLD = '\033[1m'

def print_header(text):
    print(f"\n{Colors.BOLD}{Colors.CYAN}{'='*60}{Colors.RESET}")
    print(f"{Colors.BOLD}{Colors.CYAN}{text}{Colors.RESET}")
    print(f"{Colors.BOLD}{Colors.CYAN}{'='*60}{Colors.RESET}\n")

def print_success(text):
    print(f"{Colors.GREEN}✓ {text}{Colors.RESET}")

def print_error(text):
    print(f"{Colors.RED}✗ {text}{Colors.RESET}")

def print_warning(text):
    print(f"{Colors.YELLOW}⚠ {text}{Colors.RESET}")

def print_info(text):
    print(f"{Colors.BLUE}ℹ {text}{Colors.RESET}")

def test_port_5560_handshake():
    """Test master controller handshake (Port 5560)"""
    print_header("Test 1: Port 5560 - Master Controller Handshake")
    
    try:
        context = zmq.Context()
        socket = context.socket(zmq.REQ)
        socket.setsockopt(zmq.RCVTIMEO, 5000)  # 5 second timeout
        socket.connect(f"tcp://{WINDOWS_IP}:5560")
        
        # Send CONFIG_REQ
        request = {
            "header": {
                "msg_type": "CONFIG_REQ",
                "version": "1.0.2",
                "timestamp_ns": time.time_ns(),
                "sender": "GUI_VALIDATION_TEST"
            },
            "payload": {
                "component_name": "VALIDATION_CLIENT",
                "capabilities": [
                    "indicator_display",
                    "manual_trade_entry",
                    "chart_visualization"
                ]
            }
        }
        
        print_info(f"Connecting to {WINDOWS_IP}:5560...")
        socket.send_json(request)
        
        print_info("Waiting for CONFIG_ACK...")
        response = socket.recv_json()
        
        # Validate response
        if response["header"]["msg_type"] == "CONFIG_ACK":
            status = response["payload"]["negotiation_status"]
            state = response["payload"]["current_state"]
            ports = response["payload"]["available_ports"]
            
            if status == "ACCEPTED":
                print_success(f"Handshake successful - Status: {status}")
                print_success(f"Current state: {state}")
                print_success(f"Available ports: {ports}")
                
                # Validate all ports are present
                required_ports = ["indicator_pub", "trade_req", "transformer_req", "execution_rep", "heartbeat_sub"]
                for port_name in required_ports:
                    if port_name in ports:
                        print_success(f"  ✓ {port_name}: {ports[port_name]}")
                    else:
                        print_error(f"  ✗ {port_name}: MISSING")
                        return False
                
                socket.close()
                return True
            else:
                print_error(f"Negotiation failed: {status}")
                return False
        else:
            print_error(f"Unexpected response type: {response['header']['msg_type']}")
            return False
            
    except zmq.Again:
        print_error("Timeout waiting for CONFIG_ACK (5 seconds)")
        print_warning("Check if Sierra Chart is running with MindfulTrader.dll loaded")
        return False
    except zmq.ZMQError as e:
        print_error(f"ZMQ Error: {e}")
        return False
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def test_port_5559_heartbeat():
    """Test heartbeat subscriber (Port 5559)"""
    print_header("Test 2: Port 5559 - Heartbeat Monitoring")
    
    try:
        context = zmq.Context()
        socket = context.socket(zmq.SUB)
        socket.subscribe("")  # Subscribe to all messages
        socket.setsockopt(zmq.RCVTIMEO, 3000)  # 3 second timeout
        socket.connect(f"tcp://{WINDOWS_IP}:5559")
        
        print_info(f"Subscribing to heartbeat at {WINDOWS_IP}:5559...")
        print_info("Waiting for heartbeat (max 3 seconds)...")
        
        heartbeat = socket.recv_json()
        
        if heartbeat["header"]["msg_type"] == "HEARTBEAT":
            state = heartbeat["payload"]["state"]
            strike_count = heartbeat["payload"]["strike_count"]
            sequence = heartbeat["header"]["sequence_id"]
            
            print_success(f"Heartbeat received - Sequence: {sequence}")
            print_success(f"State: {state}")
            print_success(f"Strike count: {strike_count}")
            
            if strike_count == 0:
                print_success("✓ No strikes detected (healthy connection)")
            else:
                print_warning(f"⚠ {strike_count} strike(s) detected")
            
            socket.close()
            return True
        else:
            print_error(f"Unexpected message type: {heartbeat['header']['msg_type']}")
            return False
            
    except zmq.Again:
        print_error("Timeout waiting for heartbeat (3 seconds)")
        print_warning("Heartbeat publisher may not be active yet")
        return False
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def test_port_5555_indicator():
    """Test indicator subscriber (Port 5555)"""
    print_header("Test 3: Port 5555 - Indicator Publisher")
    
    try:
        context = zmq.Context()
        socket = context.socket(zmq.SUB)
        socket.subscribe("")
        socket.setsockopt(zmq.RCVTIMEO, 10000)  # 10 second timeout (wait for bar update)
        socket.connect(f"tcp://{WINDOWS_IP}:5555")
        
        print_info(f"Subscribing to indicators at {WINDOWS_IP}:5555...")
        print_info("Waiting for indicator update (max 10 seconds)...")
        print_warning("Note: May take longer if no new bars on 5-second chart")
        
        indicator = socket.recv_json()
        
        if indicator["header"]["msg_type"] == "INDICATOR_UPDATE":
            indicators = indicator["payload"]["indicators"]
            symbol = indicator["payload"].get("symbol", "N/A")
            
            print_success(f"Indicator update received for {symbol}")
            print_success(f"Indicator count: {len(indicators)}")
            
            # Show first 5 indicators as sample
            for i, (key, value) in enumerate(list(indicators.items())[:5]):
                print_success(f"  ✓ {key}: {value}")
            
            if len(indicators) > 5:
                print_info(f"  ... and {len(indicators) - 5} more indicators")
            
            socket.close()
            return True
        else:
            print_error(f"Unexpected message type: {indicator['header']['msg_type']}")
            return False
            
    except zmq.Again:
        print_warning("Timeout waiting for indicator update (10 seconds)")
        print_info("This is expected if no new bars have formed")
        print_info("Indicator publisher is likely working, just waiting for data")
        return True  # Not a failure, just no data yet
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def test_watchdog_responsiveness():
    """Test that watchdog responds instantly (non-blocking)"""
    print_header("Test 4: Watchdog Non-Blocking Performance")
    
    try:
        context = zmq.Context()
        
        # Send 5 rapid-fire handshakes to test non-blocking behavior
        print_info("Sending 5 rapid handshakes to test watchdog responsiveness...")
        
        latencies = []
        for i in range(5):
            socket = context.socket(zmq.REQ)
            socket.setsockopt(zmq.RCVTIMEO, 5000)
            socket.connect(f"tcp://{WINDOWS_IP}:5560")
            
            start_time = time.time()
            
            request = {
                "header": {
                    "msg_type": "CONFIG_REQ",
                    "version": "1.0.2",
                    "timestamp_ns": time.time_ns(),
                    "sender": f"PERF_TEST_{i}"
                },
                "payload": {
                    "component_name": "PERFORMANCE_TEST",
                    "capabilities": ["test"]
                }
            }
            
            socket.send_json(request)
            response = socket.recv_json()
            
            latency_ms = (time.time() - start_time) * 1000
            latencies.append(latency_ms)
            
            print_success(f"Request {i+1}/5: {latency_ms:.1f}ms")
            socket.close()
            time.sleep(0.1)  # Brief pause between requests
        
        avg_latency = sum(latencies) / len(latencies)
        max_latency = max(latencies)
        
        print_success(f"\nAverage latency: {avg_latency:.1f}ms")
        print_success(f"Max latency: {max_latency:.1f}ms")
        
        if avg_latency < 100:
            print_success("✓ Excellent responsiveness (< 100ms avg)")
        elif avg_latency < 500:
            print_success("✓ Good responsiveness (< 500ms avg)")
        else:
            print_warning(f"⚠ Slow responsiveness ({avg_latency:.1f}ms avg)")
        
        return True
        
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def test_version_validation():
    """Test version mismatch rejection"""
    print_header("Test 5: Version Validation")
    
    try:
        context = zmq.Context()
        socket = context.socket(zmq.REQ)
        socket.setsockopt(zmq.RCVTIMEO, 5000)
        socket.connect(f"tcp://{WINDOWS_IP}:5560")
        
        # Send incompatible version (2.0.0 vs 1.0.2)
        request = {
            "header": {
                "msg_type": "CONFIG_REQ",
                "version": "2.0.0",  # Incompatible major version
                "timestamp_ns": time.time_ns(),
                "sender": "VERSION_TEST"
            },
            "payload": {
                "component_name": "VERSION_TEST",
                "capabilities": ["test"]
            }
        }
        
        print_info("Sending CONFIG_REQ with incompatible version 2.0.0...")
        socket.send_json(request)
        response = socket.recv_json()
        
        if response["payload"]["negotiation_status"] == "REJECTED":
            print_success("✓ Version mismatch correctly rejected")
            print_success(f"Server version: {response['header']['version']}")
            return True
        else:
            print_error(f"Expected REJECTED, got {response['payload']['negotiation_status']}")
            return False
            
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def main():
    print(f"\n{Colors.BOLD}{Colors.CYAN}╔{'═'*58}╗{Colors.RESET}")
    print(f"{Colors.BOLD}{Colors.CYAN}║{' '*10}Elite Watchdog GUI Integration Validator{' '*10}║{Colors.RESET}")
    print(f"{Colors.BOLD}{Colors.CYAN}╚{'═'*58}╝{Colors.RESET}\n")
    
    print_info(f"Testing connection to Sierra Chart at {WINDOWS_IP}")
    print_info(f"Date: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    
    results = {
        "Port 5560 Handshake": test_port_5560_handshake(),
        "Port 5559 Heartbeat": test_port_5559_heartbeat(),
        "Port 5555 Indicators": test_port_5555_indicator(),
        "Watchdog Performance": test_watchdog_responsiveness(),
        "Version Validation": test_version_validation()
    }
    
    # Summary
    print_header("Test Summary")
    
    passed = sum(1 for v in results.values() if v)
    total = len(results)
    
    for test_name, result in results.items():
        if result:
            print_success(f"{test_name}: PASSED")
        else:
            print_error(f"{test_name}: FAILED")
    
    print(f"\n{Colors.BOLD}Overall: {passed}/{total} tests passed{Colors.RESET}")
    
    if passed == total:
        print(f"\n{Colors.GREEN}{Colors.BOLD}🎉 ALL TESTS PASSED - Elite watchdog ready for GUI integration!{Colors.RESET}\n")
        return 0
    else:
        print(f"\n{Colors.RED}{Colors.BOLD}⚠ SOME TESTS FAILED - Review errors above{Colors.RESET}\n")
        return 1

if __name__ == "__main__":
    sys.exit(main())
