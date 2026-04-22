import pytest
import time
import logging
import re

from conftest import send_command
from header_handler import Header


def _read_until_idle(ser, timeout=3):
    """Read from serial until no new data arrives for 0.5s."""
    data = b''
    deadline = time.time() + timeout
    last_rx = time.time()
    while time.time() < deadline:
        if ser.in_waiting > 0:
            chunk = ser.read(ser.in_waiting)
            data += chunk
            last_rx = time.time()
        elif data and (time.time() - last_rx) > 0.5:
            break
        time.sleep(0.05)
    result = data.decode(errors='ignore')
    # Filter out asynchronous radio RX notifications
    result = re.sub(r'\[RX\][^\n]*\n?', '', result)
    result = re.sub(r'[\x00-\x08\x0e-\x1f]', '', result)
    return result


def wait_for_data(ser, timeout=10):
    """Wait for data to be available and fully received"""
    start_time = time.time()
    while time.time() - start_time < timeout:
        if ser.in_waiting > 0:
            time.sleep(0.5)
            prev_count = ser.in_waiting
            time.sleep(0.2)
            if ser.in_waiting == prev_count:
                return
        time.sleep(0.05)
    else:
        raise Exception("wait_for_data Timeout")


def parse_response(response):
    """Parse received message into header and message parts"""
    parts = response.split("|")
    if len(parts) >= 2:
        response_header = parts[0]
        response_hex = parts[1]
        response_hex = response_hex.replace('\r', '').replace('\n', '').replace('\x00', '').strip()
        if response_hex.endswith('Done'):
            response_hex = response_hex[:-4]
        hex_match = re.match(r'^([0-9A-Fa-f]*)', response_hex)
        if hex_match:
            response_hex = hex_match.group(1)
        if len(response_hex) % 2 != 0:
            response_hex = response_hex[:-1]
        try:
            if response_hex:
                response_message = bytes.fromhex(response_hex).decode('ascii', errors='ignore')
                response_message = response_message.replace('\x00', '').strip()
            else:
                response_message = ""
        except ValueError:
            response_message = response_hex
        values = tuple(int(value) for value in response_header.split(','))
        header = Header(*values)
        return header, response_message
    return None, response


# ===========================================================================
#  Single-device tests (only need 1 STM32 board)
# ===========================================================================

class TestDeviceSunshineScenarios:
    """Basic command tests — sunshine scenarios"""

    def test_help(self, serial_port):
        serial_port.reset_input_buffer()
        serial_port.write(b'help\r\n')
        response = _read_until_idle(serial_port, timeout=3)
        assert "Commands:" in response
        assert "help" in response
        assert "get" in response
        assert "set" in response
        assert "send" in response
        assert "version" in response

    def test_version(self, serial_port):
        response = send_command(serial_port, 'version')
        assert "STM32_LORA_V1" in response

    def test_get_frequency(self, serial_port):
        response = send_command(serial_port, 'get frequency')
        assert "Frequency:" in response
        assert "Hz" in response

    def test_set_frequency_868(self, serial_port):
        send_command(serial_port, 'set frequency 868000000')
        response = send_command(serial_port, 'get frequency')
        assert "868000000" in response

    def test_get_data_rate(self, serial_port):
        response = send_command(serial_port, 'get data_rate')
        assert "Data Rate:" in response

    def test_set_data_rate(self, serial_port):
        send_command(serial_port, 'set data_rate 7')
        response = send_command(serial_port, 'get data_rate')
        assert "7" in response

    def test_get_tx_power(self, serial_port):
        response = send_command(serial_port, 'get tx_power')
        assert "TX Power:" in response

    def test_set_tx_power(self, serial_port):
        send_command(serial_port, 'set tx_power 14')
        response = send_command(serial_port, 'get tx_power')
        assert "14" in response

    def test_get_mac_address(self, serial_port):
        response = send_command(serial_port, 'get mac_address')
        assert "MAC Address:" in response

    def test_set_mac_address(self, serial_port):
        send_command(serial_port, 'set mac_address 42')
        response = send_command(serial_port, 'get mac_address')
        assert "42" in response

    def test_send_message(self, serial_port):
        response = send_command(serial_port, 'send Hello, world!')
        # send queues the packet; should not error
        assert "Error" not in response

    def test_ping_executes(self, serial_port):
        serial_port.reset_input_buffer()
        serial_port.write(b'ping\r\n')
        time.sleep(0.5)
        response = serial_port.read(serial_port.in_waiting).decode(errors='ignore')
        assert "Error" not in response


class TestDeviceRainyDayScenarios:
    """Error handling tests — rainy day scenarios"""

    def test_invalid_command(self, serial_port):
        with pytest.raises(Exception, match="Send command returned Error"):
            send_command(serial_port, 'invalid_command')

    def test_set_invalid_frequency(self, serial_port):
        with pytest.raises(Exception, match="Send command returned Error"):
            send_command(serial_port, 'set frequency 999999999')

    def test_set_out_of_range_frequency_445mhz(self, serial_port):
        with pytest.raises(Exception, match="Send command returned Error"):
            send_command(serial_port, 'set frequency 445000000')

    def test_set_out_of_range_frequency_920mhz(self, serial_port):
        with pytest.raises(Exception, match="Send command returned Error"):
            send_command(serial_port, 'set frequency 920000000')

    def test_set_invalid_data_rate(self, serial_port):
        with pytest.raises(Exception, match="Send command returned Error"):
            send_command(serial_port, 'set data_rate invalid')

    def test_set_out_of_range_data_rate_13(self, serial_port):
        with pytest.raises(Exception, match="Send command returned Error"):
            send_command(serial_port, 'set data_rate 13')

    def test_set_out_of_range_data_rate_0(self, serial_port):
        with pytest.raises(Exception, match="Send command returned Error"):
            send_command(serial_port, 'set data_rate 0')

    def test_set_invalid_tx_power(self, serial_port):
        with pytest.raises(Exception, match="Send command returned Error"):
            send_command(serial_port, 'set tx_power invalid')

    def test_set_out_of_range_tx_power_0(self, serial_port):
        with pytest.raises(Exception, match="Send command returned Error"):
            send_command(serial_port, 'set tx_power 0')

    def test_set_out_of_range_tx_power_23(self, serial_port):
        with pytest.raises(Exception, match="Send command returned Error"):
            send_command(serial_port, 'set tx_power 23')


class TestMACAddressEdgeCases:
    """MAC address boundary tests"""

    def test_set_mac_address_min_valid(self, serial_port):
        send_command(serial_port, 'set mac_address 1')
        response = send_command(serial_port, 'get mac_address')
        assert "MAC Address: 1" in response

    def test_set_mac_address_max_valid(self, serial_port):
        send_command(serial_port, 'set mac_address 254')
        response = send_command(serial_port, 'get mac_address')
        assert "MAC Address: 254" in response


class TestFlashStorage:
    """Flash configuration storage tests"""

    def test_get_flash_data(self, serial_port):
        time.sleep(0.5)  # Let any pending USB data settle
        serial_port.reset_input_buffer()
        serial_port.write(b'get flash\r\n')
        response = _read_until_idle(serial_port, timeout=5)
        logging.info(f"\nFlash data: {response}")
        assert "Flash Data" in response
        assert "Frequency" in response
        assert "Data Rate" in response
        assert "TX Power" in response
        assert "MAC Address" in response

    def test_write_flash(self, serial_port):
        send_command(serial_port, 'set mac_address 33')
        response = send_command(serial_port, 'set flash 1')
        assert "Done" in response


class TestBeaconAndRouting:
    """Beacon and routing table tests"""

    def test_beacon_enable_disable(self, serial_port):
        response = send_command(serial_port, 'set beacon 1')
        assert "Done" in response
        response = send_command(serial_port, 'set beacon 0')
        assert "Done" in response

    def test_get_routing_table(self, serial_port):
        serial_port.reset_input_buffer()
        serial_port.write(b'get routing\r\n')
        time.sleep(2)
        response = serial_port.read(serial_port.in_waiting).decode(errors='ignore')
        logging.info(f"\nRouting table: {response}")
        assert "Routing table" in response


class TestResetCommand:
    """Device reset test"""

    def test_reset_and_reconnect(self, serial_port):
        """Test that reset command resets the device and it comes back"""
        serial_port.write(b'reset\r\n')
        serial_port.close()
        time.sleep(3)  # Wait for USB re-enumeration

        # Reopen
        import serial as ser_mod
        port_name = serial_port.name
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                new_ser = ser_mod.Serial(port_name, 115200, timeout=0.2)
                time.sleep(0.5)
                new_ser.reset_input_buffer()
                new_ser.write(b'version\r\n')
                time.sleep(0.5)
                resp = new_ser.read(1024).decode(errors='ignore')
                if 'STM32_LORA' in resp:
                    new_ser.close()
                    # Reopen original fixture port
                    serial_port.open()
                    return
                new_ser.close()
            except Exception:
                pass
            time.sleep(0.5)
        pytest.fail("Device did not come back after reset")


# ===========================================================================
#  Two-device communication tests (need 2 STM32 boards)
# ===========================================================================

class TestDeviceCommunicationSunshineScenarios:
    """Over-the-air communication tests between two devices"""

    def test_send_receive_message(self, serial_ports):
        ser1 = serial_ports.sers[0]
        ser2 = serial_ports.sers[1]

        expected_message = "Hello, world!"
        ser2.reset_input_buffer()
        send_command(ser1, f"send {expected_message}")

        wait_for_data(ser2)
        response = ser2.read(ser2.in_waiting).decode(errors='ignore')
        header, response_message = parse_response(response)
        logging.info(f"\n Expected: {expected_message}\n Received: {response_message}")
        assert expected_message == response_message

    def test_change_mac_send_receive(self, serial_ports):
        ser1 = serial_ports.sers[0]
        ser2 = serial_ports.sers[1]

        expected_message = "Hello, world!"
        expected_mac_addr = 54

        send_command(ser1, f'set mac_address {expected_mac_addr}')

        ser2.reset_input_buffer()
        send_command(ser1, f"send {expected_message}")

        wait_for_data(ser2)
        response = ser2.read(ser2.in_waiting).decode(errors='ignore')
        header, response_message = parse_response(response)

        logging.info(f"\n Expected: {expected_message}\n Received: {response_message}")
        assert expected_message == response_message
        assert expected_mac_addr == header.source_adr

    def test_bidirectional_message_exchange(self, serial_ports):
        ser1 = serial_ports.sers[0]
        ser2 = serial_ports.sers[1]

        send_command(ser1, 'set mac_address 101')
        send_command(ser2, 'set mac_address 102')

        # Device 1 -> Device 2
        msg1 = "From1To2"
        ser2.reset_input_buffer()
        send_command(ser1, f"send {msg1}")
        wait_for_data(ser2)
        response = ser2.read(ser2.in_waiting).decode(errors='ignore')
        header, message = parse_response(response)
        assert message == msg1
        assert header.source_adr == 101
        logging.info(f"\nDevice 1->2: {message}, source={header.source_adr}")

        time.sleep(1)

        # Device 2 -> Device 1
        msg2 = "From2To1"
        ser1.reset_input_buffer()
        send_command(ser2, f"send {msg2}")
        wait_for_data(ser1)
        response = ser1.read(ser1.in_waiting).decode(errors='ignore')
        header, message = parse_response(response)
        assert message == msg2
        assert header.source_adr == 102
        logging.info(f"\nDevice 2->1: {message}, source={header.source_adr}")
