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


RX_PATTERN = re.compile(
    r"\[RX\]\s+src=(\d+)\s+dst=(\d+)\s+rssi=(-?\d+)\s+prssi=(-?\d+)\s+"
    r"type=(\d+)\s+seq=(\d+)\s+len=(\d+)"
)


def parse_response(response):
    """Parse STM32 [RX] output into header and message parts.

    Firmware format (two lines):
        [RX] src=1 dst=2 rssi=-80 prssi=-80 type=0 seq=42 len=18
        48 65 6C 6C 6F 2C 20 77 6F 72 6C 64 21
    """
    m = RX_PATTERN.search(response)
    if m:
        src, dst, rssi, prssi, msg_type, seq, length = (int(v) for v in m.groups())
        header = Header(
            rssi=rssi, prssi=prssi, rxCnt=0,
            destination_adr=dst, source_adr=src,
            sequence_num=seq, control_mac=0, protocol_Ver=0,
            TTL=0, mesh_dest=0, mesh_tbl_entries=0,
            mesh_src=0, control_app=msg_type, length=length,
        )
        # Payload hex is on the line(s) after the [RX] line
        after_rx = response[m.end():].strip()
        hex_line = after_rx.split('\n')[0] if after_rx else ""
        hex_bytes = re.findall(r'[0-9A-Fa-f]{2}', hex_line)
        if hex_bytes:
            response_message = bytes(int(b, 16) for b in hex_bytes).decode('ascii', errors='ignore')
            response_message = response_message.replace('\x00', '').strip()
        else:
            response_message = ""
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


class TestOTAFlash:
    """OTA firmware bank flash tests"""

    def test_ota_bank_info(self, serial_port):
        """Verify OTA bank reports correct address and size"""
        serial_port.reset_input_buffer()
        serial_port.write(b'get ota bank\r\n')
        response = _read_until_idle(serial_port, timeout=3)
        logging.info(f"\nOTA bank info: {response}")
        assert "OTA Bank:" in response
        assert "0x08010000" in response.upper()
        assert "size=63488" in response

    def test_ota_erase(self, serial_port):
        """Erase the OTA bank and verify it succeeds"""
        time.sleep(0.5)
        serial_port.reset_input_buffer()
        serial_port.write(b'set ota erase\r\n')
        response = _read_until_idle(serial_port, timeout=10)
        logging.info(f"\nOTA erase: {response}")
        assert "OTA Erase: OK" in response

    def test_ota_write_and_read(self, serial_port):
        """Write 8 bytes to OTA bank and read them back"""
        # Erase first to guarantee clean state
        time.sleep(0.5)
        serial_port.reset_input_buffer()
        serial_port.write(b'set ota erase\r\n')
        _read_until_idle(serial_port, timeout=10)

        response = send_command(serial_port, 'set ota write 0000 DEADBEEF01020304')
        assert "OTA Write: OK" in response

        serial_port.reset_input_buffer()
        serial_port.write(b'get ota read 0000 8\r\n')
        response = _read_until_idle(serial_port, timeout=3)
        logging.info(f"\nOTA read: {response}")
        assert "OTA Read:" in response
        assert "DE AD BE EF 01 02 03 04" in response

    def test_ota_write_at_offset(self, serial_port):
        """Write 8 bytes at a non-zero offset (0x10)"""
        response = send_command(serial_port, 'set ota write 0010 A5A5A5A5B6B6B6B6')
        assert "OTA Write: OK" in response

        serial_port.reset_input_buffer()
        serial_port.write(b'get ota read 0010 8\r\n')
        response = _read_until_idle(serial_port, timeout=3)
        logging.info(f"\nOTA read at offset 0x10: {response}")
        assert "OTA Read:" in response
        assert "A5 A5 A5 A5 B6 B6 B6 B6" in response

    def test_ota_write_alignment_reject(self, serial_port):
        """Misaligned write should return error"""
        serial_port.reset_input_buffer()
        serial_port.write(b'set ota write 0001 DEADBEEF01020304\r\n')
        response = _read_until_idle(serial_port, timeout=3)
        logging.info(f"\nOTA misaligned write: {response}")
        assert "OTA Write: FAIL" in response
        assert "err=-2" in response

    def test_ota_pending_lifecycle(self, serial_port):
        """Set pending flag, verify it reads back, then clear it"""
        # Set pending
        response = send_command(serial_port, 'set ota pending 25000')
        assert "OTA Pending: SET" in response

        # Verify reads back
        serial_port.reset_input_buffer()
        serial_port.write(b'get ota pending\r\n')
        response = _read_until_idle(serial_port, timeout=3)
        logging.info(f"\nOTA pending after set: {response}")
        assert "OTA Pending: YES" in response
        assert "size=25000" in response

        # Clear
        response = send_command(serial_port, 'set ota clear')
        assert "OTA Pending: CLEARED" in response

        # Verify cleared
        serial_port.reset_input_buffer()
        serial_port.write(b'get ota pending\r\n')
        response = _read_until_idle(serial_port, timeout=3)
        logging.info(f"\nOTA pending after clear: {response}")
        assert "OTA Pending: NO" in response

    def test_ota_full_roundtrip(self, serial_port):
        """Full roundtrip: erase, write 16 bytes, read back, verify bank addr"""
        # Erase
        time.sleep(0.5)
        serial_port.reset_input_buffer()
        serial_port.write(b'set ota erase\r\n')
        response = _read_until_idle(serial_port, timeout=10)
        assert "OTA Erase: OK" in response

        # Write first 8 bytes at offset 0
        response = send_command(serial_port, 'set ota write 0000 0102030405060708')
        assert "OTA Write: OK" in response

        # Write next 8 bytes at offset 8
        response = send_command(serial_port, 'set ota write 0008 090A0B0C0D0E0F10')
        assert "OTA Write: OK" in response

        # Read back 16 bytes
        serial_port.reset_input_buffer()
        serial_port.write(b'get ota read 0000 16\r\n')
        response = _read_until_idle(serial_port, timeout=3)
        logging.info(f"\nOTA roundtrip read: {response}")
        assert "OTA Read:" in response
        assert "01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10" in response

        # Verify bank address still correct
        serial_port.reset_input_buffer()
        serial_port.write(b'get ota bank\r\n')
        response = _read_until_idle(serial_port, timeout=3)
        assert "0x08010000" in response.upper()


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


class TestBattery:
    """Battery ADC tests"""

    def test_get_battery(self, serial_port):
        """Verify battery command returns voltage and raw ADC value"""
        response = send_command(serial_port, 'get battery')
        assert "Battery:" in response
        assert "mV" in response
        assert "raw:" in response
        # Parse values
        import re
        m = re.search(r'Battery:\s*(\d+)\s*mV\s*\(raw:\s*(\d+)\)', response)
        assert m, f"Could not parse battery response: {response}"
        mv = int(m.group(1))
        raw = int(m.group(2))
        logging.info(f"\nBattery: {mv} mV (raw: {raw})")
        # Raw should be non-negative and within 12-bit range
        assert 0 <= raw <= 4095
        # mV should match formula: raw * 6600 / 4096
        expected_mv = raw * 6600 // 4096
        assert abs(mv - expected_mv) <= 2, f"mV mismatch: got {mv}, expected {expected_mv}"


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


class TestDFUBootloader:
    """DFU bootloader entry and recovery tests"""

    def test_dfu_help_shows_command(self, serial_port):
        """Verify 'dfu' appears in help output"""
        response = send_command(serial_port, 'help')
        assert 'dfu' in response.lower(), "DFU command not listed in help"

    def test_dfu_enter_and_return(self, serial_port):
        """Test that 'dfu' command enters bootloader, then flash and return to app"""
        import serial as ser_mod
        import serial.tools.list_ports
        import subprocess
        import os

        port_name = serial_port.name

        # Locate the .bin file relative to this test file
        test_dir = os.path.dirname(os.path.abspath(__file__))
        bin_file = os.path.join(test_dir, '..', 'lora.bin')
        assert os.path.isfile(bin_file), f"Firmware binary not found: {bin_file}"

        # Verify app is running
        response = send_command(serial_port, 'version')
        assert 'STM32_LORA' in response

        # Send dfu command — MCU jumps to bootloader
        serial_port.write(b'dfu\r\n')
        serial_port.close()
        time.sleep(2)

        # Verify DFU device appeared (VID 0483, PID df11)
        dfu_found = False
        deadline = time.time() + 10
        while time.time() < deadline:
            result = subprocess.run(['lsusb'], capture_output=True, text=True)
            if '0483:df11' in result.stdout:
                dfu_found = True
                break
            time.sleep(0.5)
        assert dfu_found, "DFU device (0483:df11) did not appear after 'dfu' command"

        # Flash the same firmware back via dfu-util
        result = subprocess.run(
            ['sudo', 'dfu-util', '-a', '0', '-s', '0x08000000:leave',
             '-D', bin_file],
            capture_output=True, text=True, timeout=30
        )
        assert 'File downloaded successfully' in result.stdout, \
            f"DFU flash failed: {result.stdout} {result.stderr}"

        # Wait for app to re-enumerate as CDC
        time.sleep(4)

        # Find the STM32 CDC port (may have changed)
        new_port = None
        deadline = time.time() + 10
        while time.time() < deadline:
            for p in serial.tools.list_ports.comports():
                if p.vid and f"{p.vid:04x}" == "0483" and p.pid and f"{p.pid:04x}" == "5740":
                    new_port = p.device
                    break
            if new_port:
                break
            time.sleep(0.5)
        assert new_port, "App did not re-enumerate as USB CDC after DFU flash"

        # Verify app is running by sending version command
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                new_ser = ser_mod.Serial(new_port, 115200, timeout=0.5)
                time.sleep(0.3)
                new_ser.reset_input_buffer()
                new_ser.write(b'version\r\n')
                time.sleep(0.5)
                resp = new_ser.read(1024).decode(errors='ignore')
                new_ser.close()
                if 'STM32_LORA' in resp:
                    # Reopen the fixture port for subsequent tests
                    serial_port.port = new_port
                    serial_port.open()
                    return
            except Exception:
                pass
            time.sleep(0.5)
        pytest.fail("App did not respond after DFU round-trip")


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
