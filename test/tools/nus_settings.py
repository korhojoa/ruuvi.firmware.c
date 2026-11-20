#!/usr/bin/python3
"""Read or change the persistent vlongmem intervals through BlueZ.

Usage: nus_settings.py MAC [--announce-ms MS] [--storage-s SECONDS]
A write requires the tag's configuration mode (a short button B press).
"""

import argparse
import struct
import sys
import time

import dbus
import nus_logread as nus

SYS_CONFIG = 0x22
CONFIG_WRITE = 0x06
CONFIG_READ = 0x07
UNAUTHORIZED = 0xEA


def validate(announce_ms: int, storage_s: int) -> None:
    if not 100 <= announce_ms <= 10000:
        raise ValueError("announcement interval must be 100 to 10000 ms")
    if not 1 <= storage_s <= 65535:
        raise ValueError("storage interval must be 1 to 65535 seconds")
    if storage_s * 1000 < 2 * announce_ms:
        raise ValueError(
            "storage interval must be at least twice the announcement interval"
        )


class SettingsClient:
    def __init__(self, mac: str):
        self.bz = nus.Bluez()
        self.mac = mac
        self.reader = nus.LogReader(self.bz, mac, 0, 15, None)
        self.response = None
        self.dev = None
        self.connected_here = False
        self.notifying = False
        self.receiver = None

    def on_notify(self, iface, changed, invalidated, path=None):
        if iface != nus.CHAR_IFACE or path != self.reader.tx_path:
            return
        data = bytes(changed.get("Value", []))
        if len(data) == 11 and data[:2] == bytes([SYS_CONFIG, SYS_CONFIG]):
            self.response = data
            self.bz.loop.quit()

    def connect(self) -> None:
        bz = self.bz
        path = bz.device_path(self.mac)
        bz.start_discovery()
        deadline = time.monotonic() + 30
        while path not in bz.om.GetManagedObjects():
            if time.monotonic() >= deadline:
                raise RuntimeError("tag was not found during discovery")
            bz.run_for(0.5)
        props = bz.props(path, nus.DEVICE_IFACE)
        if props.Get(nus.DEVICE_IFACE, "Connected"):
            raise RuntimeError("tag already has a connection on this host")
        self.dev = dbus.Interface(bz.bus.get_object(nus.BLUEZ, path), nus.DEVICE_IFACE)
        self.connected_here = True
        for attempt in range(6):
            try:
                self.dev.Connect(timeout=15)
                break
            except dbus.DBusException:
                if attempt == 5:
                    raise
                bz.run_for(1)
        bz.stop_discovery()
        deadline = time.monotonic() + 15
        while not props.Get(nus.DEVICE_IFACE, "ServicesResolved"):
            if time.monotonic() >= deadline:
                raise RuntimeError("GATT services did not resolve")
            bz.run_for(0.5)
        if not self.reader.find_chars(path):
            raise RuntimeError("NUS characteristics were not found")
        assert self.reader.tx is not None
        self.receiver = bz.bus.add_signal_receiver(
            self.on_notify,
            dbus_interface=nus.PROPS_IFACE,
            signal_name="PropertiesChanged",
            path_keyword="path",
        )
        self.reader.tx.StartNotify()
        self.notifying = True
        bz.run_for(0.2)

    def exchange(self, op: int, announce_ms=0, storage_s=0) -> tuple[int, int]:
        assert self.reader.rx is not None
        self.response = None
        request = bytes([SYS_CONFIG, SYS_CONFIG, op]) + struct.pack(
            ">II", announce_ms, storage_s
        )
        self.reader.rx.WriteValue(
            dbus.Array(request, signature="y"), {"type": dbus.String("request")}
        )
        deadline = time.monotonic() + 15
        while self.response is None:
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    "settings reply timed out; firmware may not support settings"
                )
            self.bz.run_for(0.5)
        if self.response[2] == UNAUTHORIZED:
            raise RuntimeError(
                "write denied: enable configuration mode with a short button B press"
            )
        if self.response[2] != CONFIG_WRITE:
            raise RuntimeError(
                "tag rejected the settings request: " + self.response.hex()
            )
        values = struct.unpack(">II", self.response[3:])
        validate(*values)
        return values

    def close(self) -> None:
        try:
            if self.notifying:
                assert self.reader.tx is not None
                self.reader.tx.StopNotify()
        finally:
            try:
                if self.connected_here and self.dev is not None:
                    self.dev.Disconnect()
            finally:
                self.bz.stop_discovery()
                if self.receiver is not None:
                    self.receiver.remove()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mac")
    parser.add_argument("--announce-ms", type=int)
    parser.add_argument("--storage-s", type=int)
    args = parser.parse_args()
    client = SettingsClient(args.mac)
    try:
        client.connect()
        current = client.exchange(CONFIG_READ)
        requested = (
            args.announce_ms if args.announce_ms is not None else current[0],
            args.storage_s if args.storage_s is not None else current[1],
        )
        validate(*requested)
        if requested != current:
            if client.exchange(CONFIG_WRITE, *requested) != requested:
                raise RuntimeError("settings acknowledgement differs from the request")
            if client.exchange(CONFIG_READ) != requested:
                raise RuntimeError("settings readback differs from the request")
        print(f"announcement interval: {requested[0]} ms")
        print(f"storage interval: {requested[1]} s")
        return 0
    except (RuntimeError, ValueError, dbus.DBusException) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
