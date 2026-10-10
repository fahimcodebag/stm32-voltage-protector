#!/usr/bin/env python3
"""Host-side console for the STM32 Voltage Protector.

Two transports:
  --port /dev/ttyUSB0   real hardware (or a USB-serial adapter) via pyserial
  --wokwi <proj_dir>    spawn `wokwi-cli --interactive` and speak to the simulation

Commands (type at the prompt):
  get                 show device settings
  uv <volts>          set under-voltage trip
  ov <volts>          set over-voltage trip
  oc <milliamps>      set over-current trip
  reset               clear a latched fault, re-close the relay
  inject <csv>        stream MATLAB test vectors into the device
  log <file.csv>      start recording telemetry
  quit
"""
import argparse, csv, os, queue, shutil, subprocess, sys, threading, time

FAULTS = {0: "none", 1: "under-voltage", 2: "over-voltage", 3: "over-current"}


class Transport:
    def readline(self): raise NotImplementedError
    def write(self, s): raise NotImplementedError
    def close(self): pass


class SerialTransport(Transport):
    def __init__(self, port, baud):
        import serial  # pyserial, imported lazily so --wokwi needs no dependency
        self.s = serial.Serial(port, baud, timeout=0.2)

    def readline(self):
        line = self.s.readline()
        return line.decode("utf-8", "replace").strip() if line else None

    def write(self, s):
        self.s.write((s + "\n").encode())

    def close(self):
        self.s.close()


class WokwiTransport(Transport):
    """Drives wokwi-cli as a subprocess; its stdin/stdout is the virtual UART."""

    def __init__(self, project_dir, timeout_ms=300000):
        cli = shutil.which("wokwi-cli") or os.path.expanduser("~/.wokwi/bin/wokwi-cli")
        if not os.path.exists(cli):
            sys.exit("wokwi-cli not found; install it or use --port")
        if not os.environ.get("WOKWI_CLI_TOKEN"):
            sys.exit("WOKWI_CLI_TOKEN is not set (see ~/.config/wokwi/token.env)")
        self.p = subprocess.Popen(
            [cli, "--interactive", "--timeout", str(timeout_ms), project_dir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.p.stdout:
            self.q.put(line.rstrip("\r\n"))
        self.q.put(None)

    def readline(self):
        try:
            return self.q.get(timeout=0.2)
        except queue.Empty:
            return None

    def write(self, s):
        if self.p.stdin:
            self.p.stdin.write(s + "\n")
            self.p.stdin.flush()

    def close(self):
        self.p.terminate()


class Console:
    def __init__(self, t):
        self.t = t
        self.logw = None
        self.logf = None
        self.running = True

    def handle(self, line):
        if not line:
            return
        f = line.split(",")
        if f[0] == "T" and len(f) >= 6:
            ms, mv, ma, state, fault = f[1], int(f[2]), int(f[3]), f[4], int(f[5])
            sys.stdout.write(
                f"\r{int(ms)/1000:8.2f}s  {mv/1000:6.1f} V  {ma/1000:6.2f} A  "
                f"{'CONNECTED' if state == '1' else 'TRIPPED  '}  {FAULTS.get(fault, '?'):<14}")
            sys.stdout.flush()
            if self.logw:
                self.logw.writerow([ms, mv, ma, state, fault])
        elif f[0] == "EVT":
            print(f"\n*** FAULT {FAULTS.get(int(f[2]), '?')} at {int(f[1])/1000:.3f}s "
                  f"(RMS {f[3]} V) -> relay OPEN")
        elif f[0] == "CFG":
            print(f"\nsettings: UV={f[1]} V  OV={f[2]} V  OC={int(f[3])/1000:.2f} A")
        elif line.startswith(("ACK", "ERR")):
            print("\n" + line)
        else:
            print("\n" + line)

    def rx_loop(self):
        while self.running:
            try:
                line = self.t.readline()
            except Exception:
                break
            if line is None:
                continue
            self.handle(line)

    def inject(self, path):
        with open(path) as fh:
            counts = [int(float(r[0])) for r in csv.reader(fh) if r and r[0].strip()]
        print(f"injecting {len(counts)} samples from {path}")
        self.t.write(f"INJECT {len(counts)}")
        time.sleep(0.3)
        for i in range(0, len(counts), 20):          # one mains cycle per frame
            self.t.write("S," + ",".join(str(c) for c in counts[i:i + 20]))
            time.sleep(0.02)
        print("injection complete")

    def start_log(self, path):
        self.logf = open(path, "w", newline="")
        self.logw = csv.writer(self.logf)
        self.logw.writerow(["ms", "v_mV", "i_mA", "state", "fault"])
        print(f"logging telemetry -> {path}")


def main():
    ap = argparse.ArgumentParser(description="STM32 Voltage Protector host console")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--port", help="serial port, e.g. /dev/ttyUSB0 or COM3")
    g.add_argument("--wokwi", metavar="DIR", help="run against a Wokwi project directory")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    t = SerialTransport(args.port, args.baud) if args.port else WokwiTransport(args.wokwi)
    c = Console(t)
    threading.Thread(target=c.rx_loop, daemon=True).start()
    print(__doc__.split("Commands")[0].strip())
    print("type 'help' for commands, 'quit' to exit\n")

    try:
        while True:
            cmd = input().strip()
            if not cmd:
                continue
            w = cmd.split()
            k = w[0].lower()
            if k in ("quit", "exit"):
                break
            elif k == "help":
                print(__doc__[__doc__.index("Commands"):])
            elif k == "get":
                t.write("GET")
            elif k in ("uv", "ov", "oc") and len(w) == 2:
                t.write(f"SET {k.upper()} {w[1]}")
            elif k == "reset":
                t.write("RESET")
            elif k == "inject" and len(w) == 2:
                c.inject(w[1])
            elif k == "log" and len(w) == 2:
                c.start_log(w[1])
            else:
                print("unrecognised; type 'help'")
    except (EOFError, KeyboardInterrupt):
        pass
    finally:
        c.running = False
        if c.logf:
            c.logf.close()
        t.close()
        print("\nclosed")


if __name__ == "__main__":
    main()
