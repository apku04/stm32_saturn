import pytest
import serial
import serial.tools.list_ports
import logging
import time
import re
import os

# STM32 LoRa Tracker USB identifiers
STM32_VID = "0483"
STM32_PID = "5740"
STM32_VENDOR = "Saturn"

# Can be overridden with environment variable: SERIAL_PORTS=/dev/ttyACM1,/dev/ttyACM2
CANDIDATE_PORTS = os.environ.get('SERIAL_PORTS', '').split(',') if os.environ.get('SERIAL_PORTS') else []
BAUD_RATE = 115200
TIMEOUT = 0.2


def find_stm32_ports():
    """Find all serial ports that are STM32 LoRa Tracker devices."""
    stm32_ports = []
    for p in serial.tools.list_ports.comports():
        if p.vid and f"{p.vid:04x}" == STM32_VID and p.pid and f"{p.pid:04x}" == STM32_PID:
            stm32_ports.append(p.device)
            logging.info(f"Found STM32 LoRa Tracker on {p.device} (S/N: {p.serial_number})")
    return sorted(stm32_ports)


def get_available_ports():
    """Get list of ports to use — either explicit list or auto-detected STM32 devices."""
    if CANDIDATE_PORTS and CANDIDATE_PORTS[0]:
        return CANDIDATE_PORTS
    return find_stm32_ports()


def open_port(port_name, timeout=5):
    """Open a serial port with retries."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return serial.Serial(port_name, BAUD_RATE, timeout=TIMEOUT)
        except serial.SerialException:
            time.sleep(0.1)
    raise serial.SerialException(f"Could not open {port_name} after {timeout}s")


def probe_port(port_name):
    """Probe a port to check if it responds as STM32 LoRa app."""
    try:
        ser = serial.Serial(port_name, BAUD_RATE, timeout=0.5)
        time.sleep(0.15)
        ser.reset_input_buffer()
        ser.write(b'version\r\n')
        time.sleep(0.5)
        resp = ser.read(1024).decode('ascii', errors='replace')
        ser.close()
        if 'STM32_LORA' in resp:
            return resp.strip()
    except serial.SerialException:
        pass
    return None


def send_command(ser, command, timeout=5):
    """Send command and wait for response."""
    ser.reset_input_buffer()
    ser.write((command + '\r\n').encode())
    logging.info(f"Sending to {ser.name}: {command}")

    if command == "reset":
        time.sleep(0.2)
        return ""

    response = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(1024)
        if chunk:
            response += chunk
            # For multi-line responses (help, flash, routing), keep reading
            # until no more data arrives for a short period
            if b'Error' in response:
                logging.error(f"Error response: {response.decode(errors='ignore')}")
                raise Exception("Send command returned Error")
            if b'Done' in response:
                break
            # Wait a bit more to see if more data is coming
            time.sleep(0.1)
            more = ser.read(1024)
            if more:
                response += more
                continue
            # No more data after a pause — response is complete
            if response:
                break
        time.sleep(0.05)
    else:
        raise Exception("send_command Timeout")

    resp_str = response.decode(errors='ignore')
    # Filter out asynchronous radio RX notifications that may appear in output
    resp_str = re.sub(r'\[RX\][^\n]*\n?', '', resp_str)
    # Remove stray binary/control chars
    resp_str = re.sub(r'[\x00-\x08\x0e-\x1f]', '', resp_str)
    resp_str = resp_str.strip()
    logging.info(f"Response from {ser.name}: {resp_str}")
    return resp_str


def ensure_devices_ready(timeout=10):
    """Find and open STM32 LoRa devices. Returns list of open serial connections."""
    deadline = time.time() + timeout

    while time.time() < deadline:
        ports = get_available_ports()
        app_ports = []

        for port in ports:
            version = probe_port(port)
            if version:
                app_ports.append(port)
                logging.info(f"Found app on {port}: {version}")

        if app_ports:
            connections = []
            for port in app_ports:
                ser = open_port(port)
                connections.append(ser)
            return connections

        time.sleep(0.5)

    raise Exception(f"Could not find any STM32 LoRa devices within {timeout}s")


class SerialPorts:
    def __init__(self):
        self.sers = ensure_devices_ready()
        logging.info(f"Opened {len(self.sers)} device(s): {[s.name for s in self.sers]}")


@pytest.fixture(scope='class')
def serial_port():
    """Single device fixture — for tests that only need one device."""
    ports = SerialPorts()
    ser = ports.sers[0]
    # Set a known MAC address
    send_command(ser, "set mac_address 10")
    send_command(ser, "set beacon 0")
    yield ser
    if ser.is_open:
        ser.close()


@pytest.fixture(scope='class')
def serial_ports():
    """Multi-device fixture — for communication tests that need 2+ devices."""
    ports = SerialPorts()
    if len(ports.sers) < 2:
        pytest.skip("Need at least 2 STM32 devices for communication tests")
    # Ensure both devices share known-good radio config (earlier tests may change it)
    for idx, ser in enumerate(ports.sers):
        mac_addr = 10 + idx
        send_command(ser, "set frequency 868000000")
        send_command(ser, "set data_rate 7")
        send_command(ser, "set tx_power 14")
        send_command(ser, f"set mac_address {mac_addr}")
        send_command(ser, "set beacon 0")
        logging.info(f"Set {ser.name} MAC address to {mac_addr}")
    yield ports
    for ser in ports.sers:
        if ser.is_open:
            ser.close()
