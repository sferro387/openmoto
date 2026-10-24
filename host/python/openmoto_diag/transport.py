"""Line transports to the diagnostic shell: a real serial port or the offline
simulator subprocess (tests/diag_host/diag_sim). Both expose the same tiny
interface: write_line(str) and read_lines(timeout) -> list[str].

The serial transport deliberately does NOT open the port on import and is only
constructed by the CLI once the vehicle/profile guards have passed. Opening a
serial port toggles DTR and resets the Mega, so construction is an explicit act.
"""
import subprocess
import time


class SimTransport:
    """Drive tests/diag_host/diag_sim in --stdin mode as a line pipe.

    The sim prints an ``EOC`` marker after the startup banner and after each
    input line settles, so read_until_eoc() returns exactly the lines produced
    in response to one command."""

    def __init__(self, sim_binary, tx_enabled=True):
        self._tx = tx_enabled
        self._proc = subprocess.Popen(
            [str(sim_binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1)
        # Consume the startup banner up to its EOC.
        self.banner = self.read_lines()

    def write_line(self, line):
        self._proc.stdin.write(line + '\n')
        self._proc.stdin.flush()

    def read_lines(self, timeout=10.0):
        """Return the lines up to (not including) the next EOC marker."""
        out = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._proc.stdout.readline()
            if line == '':
                break
            line = line.rstrip('\n')
            if line == 'EOC':
                return out
            out.append(line)
        return out

    def close(self):
        try:
            self._proc.stdin.close()
            self._proc.wait(timeout=5)
        except Exception:
            self._proc.kill()


class SerialTransport:
    """Real serial line to the Mega. Importing pyserial is deferred so the
    package works for offline tests without the dependency installed."""

    def __init__(self, port, baud=115200):
        import serial  # pyserial
        # Opening resets the Mega (DTR). Caller must have cleared vehicle guards.
        self._ser = serial.Serial(port, baud, timeout=0.2)
        self.port = port

    def write_line(self, line):
        self._ser.write((line + '\n').encode('ascii'))
        self._ser.flush()

    def read_lines(self, timeout=1.0):
        out = []
        deadline = time.time() + timeout
        buf = b''
        while time.time() < deadline:
            chunk = self._ser.read(256)
            if chunk:
                buf += chunk
                while b'\n' in buf:
                    raw, buf = buf.split(b'\n', 1)
                    out.append(raw.decode('ascii', 'replace').rstrip('\r'))
        return out

    def close(self):
        self._ser.close()
