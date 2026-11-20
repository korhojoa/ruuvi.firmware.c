"""Check the settings wire format and error handling without Bluetooth."""

import struct
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

import nus_logread as nus
import nus_settings as settings


class SettingsTests(unittest.TestCase):
    def make_client(self, op=settings.CONFIG_WRITE, announce_ms=1285, storage_s=600):
        with patch.object(nus, "Bluez"):
            client = settings.SettingsClient("00:00:00:00:00:00")
        client.reader.rx = Mock()
        client.reader.tx_path = "/tag/tx"
        reply = bytes([settings.SYS_CONFIG, settings.SYS_CONFIG, op]) + struct.pack(
            ">II", announce_ms, storage_s
        )
        client.reader.rx.WriteValue.side_effect = lambda *args: client.on_notify(
            nus.CHAR_IFACE, {"Value": reply}, [], path="/tag/tx"
        )
        return client

    def test_limits_and_interval_relationship(self):
        for pair in [(100, 1), (10000, 20), (1285, 600), (10000, 65535)]:
            settings.validate(*pair)
        for pair in [(99, 600), (10001, 600), (1285, 0), (1285, 65536), (10000, 19)]:
            with self.assertRaises(ValueError):
                settings.validate(*pair)

    def test_read_and_write_use_big_endian_and_exact_lengths(self):
        client = self.make_client(announce_ms=5000, storage_s=1800)
        self.assertEqual(client.exchange(settings.CONFIG_READ), (5000, 1800))
        read = bytes(client.reader.rx.WriteValue.call_args.args[0])
        self.assertEqual(read, bytes.fromhex("2222070000000000000000"))
        self.assertEqual(
            client.exchange(settings.CONFIG_WRITE, 5000, 1800), (5000, 1800)
        )
        write = bytes(client.reader.rx.WriteValue.call_args.args[0])
        self.assertEqual(write, bytes.fromhex("2222060000138800000708"))

    def test_unauthorized_write_reports_configuration_mode(self):
        client = self.make_client(op=settings.UNAUTHORIZED)
        with self.assertRaisesRegex(RuntimeError, "write denied"):
            client.exchange(settings.CONFIG_WRITE, 1285, 600)

    def test_error_reply_does_not_look_like_success(self):
        client = self.make_client(op=0xEE)
        with self.assertRaisesRegex(RuntimeError, "rejected"):
            client.exchange(settings.CONFIG_WRITE, 1285, 600)

    def test_unrelated_notifications_do_not_complete_request(self):
        client = self.make_client()
        client.on_notify(nus.CHAR_IFACE, {"Value": bytes(11)}, [], path="/tag/tx")
        self.assertIsNone(client.response)
        client.on_notify(nus.CHAR_IFACE, {"Value": bytes(11)}, [], path="/other/tx")
        self.assertIsNone(client.response)

    def test_timeout_is_bounded(self):
        client = self.make_client()
        client.reader.rx.WriteValue.side_effect = None
        with (
            patch.object(settings.time, "monotonic", side_effect=[0, 16]),
            self.assertRaisesRegex(RuntimeError, "timed out"),
        ):
            client.exchange(settings.CONFIG_READ)

    def test_close_releases_connection_even_if_notification_stop_fails(self):
        client = self.make_client()
        client.notifying = True
        client.connected_here = True
        client.dev = Mock()
        client.receiver = Mock()
        client.reader.tx = SimpleNamespace(
            StopNotify=Mock(side_effect=RuntimeError("stop"))
        )
        with self.assertRaisesRegex(RuntimeError, "stop"):
            client.close()
        client.dev.Disconnect.assert_called_once()
        client.bz.stop_discovery.assert_called_once()
        client.receiver.remove.assert_called_once()


if __name__ == "__main__":
    unittest.main()
