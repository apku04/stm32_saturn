import pytest
import time
import logging
import re
import zlib

from conftest import send_command, STM32_VID, STM32_PID
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
    r"(?:snr=(-?\d+)\s+sf=(\d+)\s+freq=(\d+)\s+)?"
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
        src = int(m.group(1))
        dst = int(m.group(2))
        rssi = int(m.group(3))
        prssi = int(m.group(4))
        msg_type = int(m.group(8))
        seq = int(m.group(9))
        length = int(m.group(10))
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


def send_and_receive(ser_tx, ser_rx, message, attempts=3):
    """Send a payload and return the parsed RX header/message, retrying RF misses."""
    last_response = ""
    for attempt in range(1, attempts + 1):
        ser_rx.reset_input_buffer()
        send_command(ser_tx, f"send {message}")
        try:
            wait_for_data(ser_rx, timeout=12)
        except Exception:
            logging.warning("No RF data on attempt %d/%d", attempt, attempts)
            time.sleep(1)
            continue

        last_response = ser_rx.read(ser_rx.in_waiting).decode(errors='ignore')
        header, response_message = parse_response(last_response)
        if header and response_message == message:
            return header, response_message
        logging.warning("Unexpected RF response on attempt %d/%d: %r", attempt, attempts, last_response)
        time.sleep(1)

    header, response_message = parse_response(last_response)
    return header, response_message


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
        assert "0x08010000" in response
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
        assert "0x08010000" in response


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
        header, response_message = send_and_receive(ser1, ser2, expected_message)
        logging.info(f"\n Expected: {expected_message}\n Received: {response_message}")
        assert expected_message == response_message

    def test_change_mac_send_receive(self, serial_ports):
        ser1 = serial_ports.sers[0]
        ser2 = serial_ports.sers[1]

        expected_message = "Hello, world!"
        expected_mac_addr = 54

        send_command(ser1, f'set mac_address {expected_mac_addr}')

        header, response_message = send_and_receive(ser1, ser2, expected_message)

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
        header, message = send_and_receive(ser1, ser2, msg1)
        assert message == msg1
        assert header.source_adr == 101
        logging.info(f"\nDevice 1->2: {message}, source={header.source_adr}")

        time.sleep(1)

        # Device 2 -> Device 1
        msg2 = "From2To1"
        header, message = send_and_receive(ser2, ser1, msg2)
        assert message == msg2
        assert header.source_adr == 102
        logging.info(f"\nDevice 2->1: {message}, source={header.source_adr}")


# ===========================================================================
#  CFG-OTA (OTA-over-LoRa remote configuration) protocol tests
#  Need 2 STM32 boards. See cfg_ota_rx_dispatch() in app/lora/main.c for the
#  receive side and firmware/app/lora/main.c's CFG-OTA block for the wire
#  format doc: PREPARE (handshake) -> DATA (values, ack'd received) -> END
#  (commit trigger, independent CRC check) -> target restarts into the new
#  config as PROVISIONAL -> pings its last-heard-from peer to verify the link
#  survived -> a final RESULT ack reports VERIFIED (committed) or ROLLBACK
#  (reverted live + persisted + rebooted again into the previous config).
# ===========================================================================

CFGOTA_OP_PREPARE = 0
CFGOTA_OP_DATA = 1
CFGOTA_OP_END = 2
CFGOTA_OP_RESULT = 3

CFGOTA_STATUS_OK = 0
CFGOTA_STATUS_BUSY = 1
CFGOTA_STATUS_BAD_SIZE = 2
CFGOTA_STATUS_BAD_VALUE = 3
CFGOTA_STATUS_CRC_FAIL = 4
CFGOTA_STATUS_ROLLBACK = 5
CFGOTA_STATUS_VERIFIED = 6

CFGOTA_ACK_RE = re.compile(r"\[CFGOTA_ACK\]\s+from=(\d+)\s+op=(\d+)\s+status=(\d+)")


def wait_for_cfgota_ack(ser, op, timeout=8, from_mac=None):
    """Read raw serial until a matching [CFGOTA_ACK] line appears, or return
    None on timeout. Doesn't rely on newline-clean framing — [CFGOTA_ACK] can
    land concatenated onto a preceding print, so this scans with re.finditer
    over the whole accumulated buffer rather than splitting into lines."""
    deadline = time.time() + timeout
    buf = ""
    while time.time() < deadline:
        n = ser.in_waiting
        if n:
            buf += ser.read(n).decode(errors='ignore')
            for m in CFGOTA_ACK_RE.finditer(buf):
                frm, o, status = int(m.group(1)), int(m.group(2)), int(m.group(3))
                if o == op and (from_mac is None or frm == from_mac):
                    return status
        time.sleep(0.05)
    return None


def cfgota_prepare(ser, target, timeout=8):
    ser.reset_input_buffer()
    ser.write(f'send cfgota prepare {target}\r\n'.encode())
    return wait_for_cfgota_ack(ser, CFGOTA_OP_PREPARE, timeout=timeout, from_mac=target)


def cfgota_data(ser, target, pwr, sf, beacon_s, timeout=8):
    ser.reset_input_buffer()
    ser.write(f'send cfgota data {target} {pwr} {sf} {beacon_s}\r\n'.encode())
    return wait_for_cfgota_ack(ser, CFGOTA_OP_DATA, timeout=timeout, from_mac=target)


def cfgota_end(ser, target, crc_hex, timeout=8):
    ser.reset_input_buffer()
    ser.write(f'send cfgota end {target} {crc_hex}\r\n'.encode())
    return wait_for_cfgota_ack(ser, CFGOTA_OP_END, timeout=timeout, from_mac=target)


def abort_cfgota_session(ser, target):
    """Force the target back to CFGOTA_IDLE from READY or RECEIVED, via an
    intentionally out-of-range DATA (pwr=0). The firmware resets to idle on
    ANY validation failure regardless of which state it was in — this is the
    cheap, immediate alternative to waiting out the 15s idle timeout between
    tests. Returns the ack status (should be BAD_VALUE)."""
    return cfgota_data(ser, target, pwr=0, sf=7, beacon_s=10)


def cfgota_push_full(ser, target, pwr, sf, beacon_s,
                      prepare_timeout=8, data_timeout=8, end_timeout=8):
    """Drive PREPARE -> DATA -> END to completion. Returns (ok, stage, status):
    stage is the step that failed ('prepare'/'data'/'end') or None if every
    step ack'd OK — a True result means the target has committed the change
    as PROVISIONAL and is rebooting, not that it's verified yet."""
    st = cfgota_prepare(ser, target, timeout=prepare_timeout)
    if st != CFGOTA_STATUS_OK:
        return False, 'prepare', st
    st = cfgota_data(ser, target, pwr, sf, beacon_s, timeout=data_timeout)
    if st != CFGOTA_STATUS_OK:
        return False, 'data', st
    payload = bytes([pwr & 0xFF, sf & 0xFF, beacon_s & 0xFF, (beacon_s >> 8) & 0xFF])
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    st = cfgota_end(ser, target, f"{crc:08x}", timeout=end_timeout)
    if st != CFGOTA_STATUS_OK:
        return False, 'end', st
    return True, None, CFGOTA_STATUS_OK


def reconnect_by_mac(old_ser, expected_mac, timeout=20):
    """Close a serial handle and find whichever STM32 CDC device now reports
    `expected_mac`, after an internal reset (SCB_AIRCR SYSRESETREQ — the same
    mechanism the plain 'reset' command uses, see TestResetCommand). CFG-OTA's
    commit and rollback paths both reboot this way.

    Deliberately does NOT assume the device comes back on the SAME
    /dev/ttyACM* path — a soft reset on this hardware can re-enumerate it
    under a different number even though it's the same physical board (see
    project memory 'hw-setup-stm32-nodes': "ACM numbers shift after USB
    reconnects"). Identifying it by its own reported mac_address, scanned
    across every present STM32 CDC port, is robust to that; matching by port
    name is not."""
    import serial as ser_mod
    import serial.tools.list_ports as list_ports
    try:
        old_ser.close()
    except Exception:
        pass
    deadline = time.time() + timeout
    while time.time() < deadline:
        for p in list_ports.comports():
            if not (p.vid and f"{p.vid:04x}" == STM32_VID
                    and p.pid and f"{p.pid:04x}" == STM32_PID):
                continue
            try:
                cand = ser_mod.Serial(p.device, 115200, timeout=0.3)
                cand.dtr = True
                cand.rts = True
                time.sleep(0.25)
                cand.reset_input_buffer()
                cand.write(b'get mac_address\r\n')
                time.sleep(0.3)
                resp = cand.read(cand.in_waiting).decode(errors='ignore')
                m = re.search(r'MAC Address:\s*(\d+)', resp)
                if m and int(m.group(1)) == expected_mac:
                    return cand
                cand.close()
            except ser_mod.SerialException:
                pass
        time.sleep(0.3)
    raise Exception(f"no STM32 CDC device reporting mac={expected_mac} "
                    f"came back within {timeout}s")


class TestCfgOtaConfigPush:
    """OTA-over-LoRa remote CONFIGURATION: handshake -> ack'd transfer ->
    commit -> restart -> self-verify -> VERIFIED or auto-ROLLBACK.

    Ordered so every test leaves the target's cfgota state IDLE for the next
    one: validation-failure tests self-reset (BAD_VALUE/CRC_FAIL do), and
    prepare-only/end-too-early tests call abort_cfgota_session() explicitly.
    The two full-cycle tests at the end actually commit + reboot the target.
    """

    TARGET_MAC = 11   # serial_ports fixture assigns sers[0]=10, sers[1]=11

    @pytest.fixture(scope='class', autouse=True)
    def _persist_macs_to_flash(self, serial_ports):
        """The shared serial_ports fixture sets MAC addresses RAM-only (see
        conftest.py — no 'set flash 1' there). Every other test class gets
        away with that because nothing else in the suite reboots the device
        for real; this class does (CFG-OTA commit/rollback), and a real
        reboot reloads mac_address from flash — so 10/11 need to be
        persisted, not just live in RAM, or the target silently reverts to
        its old identity mid-suite. Same 'set mac_address N; set flash 1'
        pattern as TestFlashStorage.test_write_flash."""
        for idx, ser in enumerate(serial_ports.sers):
            mac = 10 + idx
            send_command(ser, f'set mac_address {mac}')
            send_command(ser, 'set flash 1')

    def test_help_lists_cfgota(self, serial_port):
        response = send_command(serial_port, 'help')
        assert 'cfgota' in response.lower()

    def test_pending_flag_clear_before_suite(self, serial_ports):
        """Precondition: neither board should already be mid-rollback-dance
        from an earlier run before these tests start."""
        for ser in serial_ports.sers:
            ser.reset_input_buffer()
            ser.write(b'get cfgota\r\n')
            time.sleep(0.3)
            resp = ser.read(ser.in_waiting).decode(errors='ignore')
            assert 'CFGOTA Pending: NO' in resp, \
                f"{ser.name} has a stuck provisional config: {resp!r}"

    def test_prepare_ack_ok(self, serial_ports):
        pusher, target = serial_ports.sers[0], serial_ports.sers[1]
        status = cfgota_prepare(pusher, self.TARGET_MAC)
        assert status == CFGOTA_STATUS_OK, f"expected PREPARE ok, got status={status}"
        # Session is now READY on the target; clean up for the next test.
        assert abort_cfgota_session(pusher, self.TARGET_MAC) == CFGOTA_STATUS_BAD_VALUE

    def test_prepare_wrong_target_no_reply(self, serial_ports):
        """A PREPARE addressed to a mac nobody owns must never be acked by
        either board — they only act when the payload's target byte equals
        their own mac (main.c: `p[0] != get_mac_address()` early-return)."""
        pusher = serial_ports.sers[0]
        status = cfgota_prepare(pusher, 253, timeout=5)
        assert status is None, f"unexpected ack for an unowned target: status={status}"

    def test_data_without_prepare_is_busy(self, serial_ports):
        """DATA with no active session is rejected (state==IDLE -> BUSY),
        not silently accepted."""
        pusher, target = serial_ports.sers[0], serial_ports.sers[1]
        status = cfgota_data(pusher, self.TARGET_MAC, pwr=14, sf=7, beacon_s=10)
        assert status == CFGOTA_STATUS_BUSY, f"expected BUSY with no session, got status={status}"

    def test_data_bad_pwr_rejected(self, serial_ports):
        # Each sub-case needs its OWN prepare: a BAD_VALUE ack resets the
        # target to CFGOTA_IDLE (that's the point of it), so a second DATA
        # sent right after would itself get BUSY ("no session"), not
        # BAD_VALUE — that would be testing the wrong thing.
        pusher, target = serial_ports.sers[0], serial_ports.sers[1]
        assert cfgota_prepare(pusher, self.TARGET_MAC) == CFGOTA_STATUS_OK
        status = cfgota_data(pusher, self.TARGET_MAC, pwr=0, sf=7, beacon_s=10)
        assert status == CFGOTA_STATUS_BAD_VALUE, f"pwr=0 should be rejected, got status={status}"

        assert cfgota_prepare(pusher, self.TARGET_MAC) == CFGOTA_STATUS_OK
        status = cfgota_data(pusher, self.TARGET_MAC, pwr=23, sf=7, beacon_s=10)
        assert status == CFGOTA_STATUS_BAD_VALUE, f"pwr=23 should be rejected, got status={status}"

    def test_data_bad_sf_rejected(self, serial_ports):
        pusher, target = serial_ports.sers[0], serial_ports.sers[1]
        assert cfgota_prepare(pusher, self.TARGET_MAC) == CFGOTA_STATUS_OK
        status = cfgota_data(pusher, self.TARGET_MAC, pwr=14, sf=4, beacon_s=10)
        assert status == CFGOTA_STATUS_BAD_VALUE, f"sf=4 should be rejected, got status={status}"

        assert cfgota_prepare(pusher, self.TARGET_MAC) == CFGOTA_STATUS_OK
        status = cfgota_data(pusher, self.TARGET_MAC, pwr=14, sf=13, beacon_s=10)
        assert status == CFGOTA_STATUS_BAD_VALUE, f"sf=13 should be rejected, got status={status}"

    def test_data_bad_beacon_rejected(self, serial_ports):
        pusher, target = serial_ports.sers[0], serial_ports.sers[1]
        assert cfgota_prepare(pusher, self.TARGET_MAC) == CFGOTA_STATUS_OK
        status = cfgota_data(pusher, self.TARGET_MAC, pwr=14, sf=7, beacon_s=0)
        assert status == CFGOTA_STATUS_BAD_VALUE, f"beacon_s=0 should be rejected, got status={status}"

    def test_data_ack_ok_then_bad_crc_end_rejected(self, serial_ports):
        """Valid DATA is ack'd as fully received; an END with the WRONG crc
        is rejected and — critically — nothing is applied (live radio config
        on the target is unchanged, proven directly on its own console)."""
        pusher, target = serial_ports.sers[0], serial_ports.sers[1]
        before_pwr = send_command(target, 'get tx_power')
        before_sf = send_command(target, 'get data_rate')

        assert cfgota_prepare(pusher, self.TARGET_MAC) == CFGOTA_STATUS_OK
        status = cfgota_data(pusher, self.TARGET_MAC, pwr=9, sf=10, beacon_s=42)
        assert status == CFGOTA_STATUS_OK, f"expected DATA ok, got status={status}"

        status = cfgota_end(pusher, self.TARGET_MAC, "deadbeef")
        assert status == CFGOTA_STATUS_CRC_FAIL, f"expected CRC_FAIL, got status={status}"

        after_pwr = send_command(target, 'get tx_power')
        after_sf = send_command(target, 'get data_rate')
        assert after_pwr == before_pwr, "tx_power changed despite a rejected commit"
        assert after_sf == before_sf, "data_rate changed despite a rejected commit"

    def test_end_before_data_is_busy(self, serial_ports):
        """END while still in READY (no DATA yet) is rejected without
        touching state — confirmed by a subsequent DATA still working."""
        pusher, target = serial_ports.sers[0], serial_ports.sers[1]
        assert cfgota_prepare(pusher, self.TARGET_MAC) == CFGOTA_STATUS_OK
        status = cfgota_end(pusher, self.TARGET_MAC, "00000000")
        assert status == CFGOTA_STATUS_BUSY, f"expected BUSY (no data yet), got status={status}"
        # Session is still READY (END-too-early doesn't touch state) — clean up.
        assert abort_cfgota_session(pusher, self.TARGET_MAC) == CFGOTA_STATUS_BAD_VALUE

    def test_double_prepare_is_busy(self, serial_ports):
        pusher, target = serial_ports.sers[0], serial_ports.sers[1]
        assert cfgota_prepare(pusher, self.TARGET_MAC) == CFGOTA_STATUS_OK
        status = cfgota_prepare(pusher, self.TARGET_MAC)
        assert status == CFGOTA_STATUS_BUSY, f"expected BUSY on a 2nd prepare, got status={status}"
        assert abort_cfgota_session(pusher, self.TARGET_MAC) == CFGOTA_STATUS_BAD_VALUE

    def test_full_success_commit_reboot_and_verify(self, serial_ports):
        """The whole happy path: PREPARE/DATA/END all ack ok, the target
        restarts, and — because the pushed SF/power still match the pusher's
        own radio settings — its post-reboot verify ping succeeds. Confirms
        the RESULT ack (VERIFIED) AND that the new setting actually persisted
        by reconnecting to the target and reading it back directly."""
        pusher, target = serial_ports.sers[0], serial_ports.sers[1]
        before = send_command(target, 'get beacon_interval')
        new_beacon_s = 9 if 'Interval: 9' not in before else 8

        ok, stage, status = cfgota_push_full(
            pusher, self.TARGET_MAC, pwr=14, sf=7, beacon_s=new_beacon_s)
        assert ok, f"push failed at {stage!r} with status={status}"

        result = wait_for_cfgota_ack(pusher, CFGOTA_OP_RESULT, timeout=30,
                                     from_mac=self.TARGET_MAC)
        assert result == CFGOTA_STATUS_VERIFIED, \
            f"expected VERIFIED (SF/power unchanged, link should hold), got status={result}"

        new_target = reconnect_by_mac(target, self.TARGET_MAC, timeout=20)
        serial_ports.sers[1] = new_target
        resp = send_command(new_target, 'get beacon_interval')
        assert f"Interval: {new_beacon_s} s" in resp, f"beacon interval did not persist: {resp!r}"
        resp = send_command(new_target, 'get cfgota')
        assert 'CFGOTA Pending: NO' in resp, f"still provisional after VERIFIED: {resp!r}"

    def test_rollback_on_unreachable_sf(self, serial_ports):
        """Push a spreading factor that the pusher itself does NOT use — the
        target reboots onto it, genuinely cannot be heard, exhausts its
        verify-ping retries, and must revert on its own. This is the core
        safety guarantee of the feature: a bad-but-in-range config can never
        strand the node, only cost it one extra reboot cycle."""
        pusher, target = serial_ports.sers[0], serial_ports.sers[1]
        pusher_sf = int(re.search(r'\d+', send_command(pusher, 'get data_rate')).group())
        broken_sf = 5 if pusher_sf != 5 else 6
        assert broken_sf != pusher_sf

        ok, stage, status = cfgota_push_full(
            pusher, self.TARGET_MAC, pwr=14, sf=broken_sf, beacon_s=10)
        assert ok, f"push failed at {stage!r} with status={status}"

        # settle(2s) + up to 5 verify-ping attempts 4s apart (main.c
        # CFG_OTA_VERIFY_SETTLE_MS/MAX_TRIES/TIMEOUT_MS) ~= 22s worst case,
        # plus the two physical reboots — give it real margin.
        result = wait_for_cfgota_ack(pusher, CFGOTA_OP_RESULT, timeout=40,
                                     from_mac=self.TARGET_MAC)
        assert result == CFGOTA_STATUS_ROLLBACK, \
            f"expected ROLLBACK for an unreachable SF, got status={result}"

        new_target = reconnect_by_mac(target, self.TARGET_MAC, timeout=20)
        serial_ports.sers[1] = new_target
        resp = send_command(new_target, 'get data_rate')
        assert str(pusher_sf) in resp, f"SF did not roll back to {pusher_sf}: {resp!r}"
        resp = send_command(new_target, 'get cfgota')
        assert 'CFGOTA Pending: NO' in resp, f"still provisional after ROLLBACK: {resp!r}"
