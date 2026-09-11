#!/usr/bin/python3
"""Hardware test tool: read the log of a RuuviTag through BlueZ.

The tool does the same GATT log read as the Ruuvi Station apps, with the
Nordic UART Service (NUS) and the Ruuvi standard message format (11 bytes:
destination, source, operation, 8-byte payload). It is only for tests of the
firmware on a Linux host with BlueZ. It uses the D-Bus API of BlueZ through
dbus-python and GLib, thus no package installation is necessary on a usual
desktop Linux.

Usage:
    nus_logread.py scan [seconds]
    nus_logread.py read <MAC> [--days N | --start EPOCH] [--csv FILE] [--timeout S]

Examples:
    nus_logread.py scan 10
    nus_logread.py read DD:BF:94:CB:41:59 --days 10 --csv out.csv
    nus_logread.py read DD:BF:94:CB:41:59 --days 400 --timeout 3600
"""
import csv
import struct
import sys
import time

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

BLUEZ = "org.bluez"
ADAPTER_IFACE = "org.bluez.Adapter1"
DEVICE_IFACE = "org.bluez.Device1"
CHAR_IFACE = "org.bluez.GattCharacteristic1"
PROPS_IFACE = "org.freedesktop.DBus.Properties"
OM_IFACE = "org.freedesktop.DBus.ObjectManager"

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # Write to the tag.
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # Notifications from the tag.
RUUVI_COMPANY_ID = 0x0499

# Ruuvi standard message.
RE_ENV_ALL = 0x3A
RE_TEMPERATURE = 0x30
RE_HUMIDITY = 0x31
RE_PRESSURE = 0x32
OP_LOG_READ = 0x11
OP_LOG_WRITE = 0x10
OP_TIMEOUT = 0xE0
OP_ERROR = 0xEE

FIELD_NAMES = {RE_TEMPERATURE: "temperature", RE_HUMIDITY: "humidity", RE_PRESSURE: "pressure"}
FIELD_SCALE = {RE_TEMPERATURE: 0.01, RE_HUMIDITY: 0.01, RE_PRESSURE: 1.0}


def decode_df5(data):
    """Decode Ruuvi data format 5 manufacturer data."""
    if len(data) < 24 or data[0] != 5:
        return None
    temp, humi, pres, ax, ay, az, power, moves, seq = struct.unpack(">hHHhhhHBH", bytes(data[1:18]))
    return {
        "temperature_c": temp * 0.005,
        "humidity_rh": humi * 0.0025,
        "pressure_pa": pres + 50000,
        "battery_mv": (power >> 5) + 1600,
        "tx_power_dbm": (power & 0x1F) * 2 - 40,
        "movement": moves,
        "sequence": seq,
        "mac": ":".join("%02X" % b for b in data[18:24]),
    }


class Bluez:
    def __init__(self):
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SystemBus()
        self.om = dbus.Interface(self.bus.get_object(BLUEZ, "/"), OM_IFACE)
        self.adapter_path = None
        for path, ifaces in self.om.GetManagedObjects().items():
            if ADAPTER_IFACE in ifaces:
                self.adapter_path = path
                break
        if self.adapter_path is None:
            sys.exit("no Bluetooth adapter")
        self.adapter = dbus.Interface(self.bus.get_object(BLUEZ, self.adapter_path), ADAPTER_IFACE)
        self.loop = GLib.MainLoop()

    def props(self, path, iface):
        return dbus.Interface(self.bus.get_object(BLUEZ, path), PROPS_IFACE)

    def device_path(self, mac):
        return self.adapter_path + "/dev_" + mac.upper().replace(":", "_")

    def start_discovery(self):
        try:
            self.adapter.SetDiscoveryFilter({"Transport": dbus.String("le")})
        except dbus.DBusException:
            pass
        try:
            self.adapter.StartDiscovery()
        except dbus.DBusException as e:
            if "InProgress" not in str(e):
                raise

    def stop_discovery(self):
        try:
            self.adapter.StopDiscovery()
        except dbus.DBusException:
            pass

    def run_for(self, seconds):
        GLib.timeout_add(int(seconds * 1000), self.loop.quit)
        self.loop.run()


def cmd_scan(seconds):
    bz = Bluez()
    seen = {}

    def on_props(iface, changed, invalidated, path=None):
        if iface != DEVICE_IFACE:
            return
        if "ManufacturerData" in changed or "Name" in changed:
            try:
                p = bz.props(path, DEVICE_IFACE)
                name = str(p.Get(DEVICE_IFACE, "Name")) if "Name" in changed or path in seen else ""
            except dbus.DBusException:
                name = ""
            md = changed.get("ManufacturerData", {})
            if RUUVI_COMPANY_ID in md:
                d = decode_df5(bytes(md[RUUVI_COMPANY_ID]))
                seen[path] = (name or seen.get(path, ("", None))[0], d)

    bz.bus.add_signal_receiver(on_props, dbus_interface=PROPS_IFACE, signal_name="PropertiesChanged",
                               path_keyword="path")
    bz.start_discovery()
    bz.run_for(seconds)
    bz.stop_discovery()
    # Also the devices BlueZ already knows.
    for path, ifaces in bz.om.GetManagedObjects().items():
        dev = ifaces.get(DEVICE_IFACE)
        if not dev:
            continue
        md = dev.get("ManufacturerData", {})
        if RUUVI_COMPANY_ID in md and path not in seen:
            seen[path] = (str(dev.get("Name", "")), decode_df5(bytes(md[RUUVI_COMPANY_ID])))
    for path, (name, d) in sorted(seen.items()):
        mac = path.split("dev_")[-1].replace("_", ":")
        print("%s  %-14s %s" % (mac, name, d))
    return 0


class LogReader:
    def __init__(self, bz, mac, start_s, timeout_s, csv_path):
        self.bz = bz
        self.mac = mac
        self.start_s = start_s
        self.timeout_s = timeout_s
        self.csv_path = csv_path
        self.rx = None
        self.tx = None
        self.samples = {}  # timestamp -> {field: value}
        self.messages = 0
        self.heartbeats = 0
        self.done = False
        self.error = None
        self.t_request = None
        self.t_last = None

    def find_chars(self, dev_path):
        for path, ifaces in self.bz.om.GetManagedObjects().items():
            ch = ifaces.get(CHAR_IFACE)
            if not ch or not path.startswith(dev_path):
                continue
            uuid = str(ch["UUID"]).lower()
            if uuid == NUS_RX:
                self.rx = dbus.Interface(self.bz.bus.get_object(BLUEZ, path), CHAR_IFACE)
            elif uuid == NUS_TX:
                self.tx_path = path
                self.tx = dbus.Interface(self.bz.bus.get_object(BLUEZ, path), CHAR_IFACE)
        return self.rx is not None and self.tx is not None

    def on_notify(self, iface, changed, invalidated, path=None):
        if iface != CHAR_IFACE or path != self.tx_path or "Value" not in changed:
            return
        data = bytes(changed["Value"])
        self.t_last = time.monotonic()
        if len(data) >= 1 and data[0] == 5:
            self.heartbeats += 1
            return
        if len(data) != 11:
            print("odd message: %s" % data.hex())
            return
        self.messages += 1
        dest, src, op = data[0], data[1], data[2]
        payload = data[3:]
        if op == OP_LOG_WRITE and payload == b"\xff" * 8:
            self.done = True
            self.bz.loop.quit()
            return
        if op == OP_TIMEOUT:
            self.error = "tag sent timeout (0xE0)"
            self.bz.loop.quit()
            return
        if op == OP_ERROR:
            self.error = "tag sent error (0xEE)"
            self.bz.loop.quit()
            return
        if op != OP_LOG_WRITE or src not in FIELD_NAMES:
            print("unexpected message: %s" % data.hex())
            return
        ts, raw = struct.unpack(">Ii", payload)
        # The Android app reads 0xFFFFFFFF as "no value" for humidity and
        # pressure. For temperature -1 is a valid -0.01 deg C.
        value = None if (raw == -1 and src != RE_TEMPERATURE) else raw * FIELD_SCALE[src]
        self.samples.setdefault(ts, {})[FIELD_NAMES[src]] = value
        if self.messages % 2000 == 0:
            el = time.monotonic() - self.t_request
            print("  %d messages, %d timestamps, %.0f msg/s" % (self.messages, len(self.samples), self.messages / el))

    def run(self):
        bz = self.bz
        dev_path = bz.device_path(self.mac)
        bz.start_discovery()
        # Wait for the device object.
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if dev_path in bz.om.GetManagedObjects():
                break
            bz.run_for(1)
        else:
            bz.stop_discovery()
            sys.exit("device %s not found in 30 s" % self.mac)
        dev = dbus.Interface(bz.bus.get_object(BLUEZ, dev_path), DEVICE_IFACE)
        dprops = bz.props(dev_path, DEVICE_IFACE)
        print("connecting to %s (%s)" % (self.mac, dprops.Get(DEVICE_IFACE, "Name")))
        # BlueZ removes a device that is not connected soon after the
        # discovery stops, and can abort a connection during the discovery.
        # Thus keep the discovery on and try some times.
        t0 = time.monotonic()
        for attempt in range(1, 7):
            try:
                dev.Connect(timeout=60)
                break
            except dbus.DBusException as e:
                print("connect attempt %d: %s" % (attempt, e.get_dbus_message()))
                if attempt == 6:
                    bz.stop_discovery()
                    sys.exit("cannot connect")
                bz.run_for(2)
        bz.stop_discovery()
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and not dprops.Get(DEVICE_IFACE, "ServicesResolved"):
            bz.run_for(0.5)
        if not self.find_chars(dev_path):
            dev.Disconnect()
            sys.exit("NUS characteristics not found")
        print("connected in %.1f s, services resolved" % (time.monotonic() - t0))
        bz.bus.add_signal_receiver(self.on_notify, dbus_interface=PROPS_IFACE,
                                   signal_name="PropertiesChanged", path_keyword="path")
        self.tx.StartNotify()
        bz.run_for(0.5)
        now = int(time.time())
        req = bytes([RE_ENV_ALL, RE_ENV_ALL, OP_LOG_READ]) + struct.pack(">II", now, self.start_s)
        print("request: %s (now %d, start %d, %.1f days)" % (req.hex(), now, self.start_s, (now - self.start_s) / 86400.0))
        self.t_request = time.monotonic()
        self.t_last = self.t_request
        self.rx.WriteValue(dbus.Array(req, signature="y"), {"type": dbus.String("request")})
        # Run until the end message, an error, or a silence of 30 s.
        while not self.done and self.error is None:
            bz.run_for(1)
            if time.monotonic() - self.t_last > 30:
                self.error = "no message for 30 s"
                break
            if time.monotonic() - self.t_request > self.timeout_s:
                self.error = "timeout %d s" % self.timeout_s
                break
        elapsed = time.monotonic() - self.t_request
        try:
            self.tx.StopNotify()
        except dbus.DBusException:
            pass
        dev.Disconnect()
        self.report(now, elapsed)
        return 0 if self.error is None else 1

    def report(self, now, elapsed):
        print("result: %s" % ("complete" if self.done else self.error))
        print("messages: %d in %.1f s (%.0f msg/s), heartbeats: %d" %
              (self.messages, elapsed, self.messages / elapsed if elapsed else 0, self.heartbeats))
        ts_list = sorted(self.samples)
        print("timestamps: %d" % len(ts_list))
        if not ts_list:
            return
        gaps = {}
        for a, b in zip(ts_list, ts_list[1:]):
            gaps[b - a] = gaps.get(b - a, 0) + 1
        print("first: %s (%d), last: %s (%d), now - last = %d s" % (
            time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(ts_list[0])), ts_list[0],
            time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(ts_list[-1])), ts_list[-1], now - ts_list[-1]))
        print("gaps between timestamps (s: count): %s" % dict(sorted(gaps.items())[:8]))
        complete = sum(1 for t in ts_list if len(self.samples[t]) == 3)
        print("timestamps with all three fields: %d" % complete)
        for t in ts_list[:3] + ts_list[-3:]:
            print("  %s %s" % (time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(t)), self.samples[t]))
        if self.csv_path:
            with open(self.csv_path, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["timestamp", "iso_utc", "temperature_c", "humidity_rh", "pressure_pa"])
                for t in ts_list:
                    s = self.samples[t]
                    w.writerow([t, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t)),
                                s.get("temperature"), s.get("humidity"), s.get("pressure")])
            print("csv: %s" % self.csv_path)


def main(argv):
    if len(argv) < 2 or argv[1] not in ("scan", "read"):
        print(__doc__)
        return 2
    if argv[1] == "scan":
        return cmd_scan(float(argv[2]) if len(argv) > 2 else 10.0)
    mac = argv[2]
    days = 10.0
    start = None
    csv_path = None
    timeout_s = 900
    args = argv[3:]
    while args:
        a = args.pop(0)
        if a == "--days":
            days = float(args.pop(0))
        elif a == "--start":
            start = int(args.pop(0))
        elif a == "--csv":
            csv_path = args.pop(0)
        elif a == "--timeout":
            timeout_s = int(args.pop(0))
        else:
            sys.exit("unknown argument %s" % a)
    if start is None:
        start = int(time.time() - days * 86400)
    bz = Bluez()
    return LogReader(bz, mac, start, timeout_s, csv_path).run()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
