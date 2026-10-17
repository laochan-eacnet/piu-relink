"""Byte-preserving profile edits and native result/profile storage boundaries."""
from contextlib import closing
from pathlib import Path
import sqlite3
import struct
import tempfile
import unittest

import gameplay
import playerdata
import piuserver


class PlayerDataTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.db = str(Path(self.directory.name) / "players.db")
        with closing(sqlite3.connect(self.db)) as connection:
            connection.executescript(piuserver.SCHEMA + playerdata.PROFILE_SCHEMA + gameplay.SCHEMA)
        self.user_id, self.initial = playerdata.load_profile(self.db, "11223344")

    def profile_rows(self):
        with closing(sqlite3.connect(self.db)) as connection:
            return connection.execute(
                "SELECT version,save_data FROM profiles WHERE user_id=? ORDER BY version,id",
                (self.user_id,)).fetchall()

    def test_edit_preserves_identity_reserved_bytes_unknown_bits_and_old_rows(self):
        original = bytearray(self.initial)
        original[:36] = bytes(range(36))
        original[0x41] = 0x85
        original[0x43:0x4c] = b"unchanged"
        original[0x60:] = bytes(range(16))
        unknown_bits = (1 << 62) | (1 << 39)
        struct.pack_into("<Q", original, 0x4c, unknown_bits | 4)
        with closing(sqlite3.connect(self.db)) as connection, connection:
            connection.execute("UPDATE profiles SET save_data=? WHERE user_id=?", (original, self.user_id))
        version = playerdata.update_player(
            self.db, self.user_id,
            {"nickname": "NEW NAME", "avatar": "163", "title": "212", "skin": "44",
             "auto_velocity": "999", "speed": "26", "timing": "-15", "visibility": "2"},
            expected_version=1)
        self.assertEqual(version, 2)
        user_id, profile = playerdata.load_profile(self.db, "11223344")
        self.assertEqual(user_id, self.user_id)
        expected = bytearray(original)
        expected[0x24:0x31] = b"NEW NAME".ljust(13, b"\0")
        expected[0x40] = 212
        expected[0x42] = 163
        struct.pack_into("<Q", expected, 0x4c, unknown_bits | 4 | 2)
        struct.pack_into("<H", expected, 0x54, 26)
        struct.pack_into("<H", expected, 0x56, 44)
        struct.pack_into("<H", expected, 0x58, 999)
        struct.pack_into("<f", expected, 0x5c, -15)
        self.assertEqual(profile, bytes(expected))
        self.assertEqual(self.profile_rows(), [(1, bytes(original)), (2, bytes(expected))])
        with closing(sqlite3.connect(self.db)) as connection:
            self.assertEqual(connection.execute(
                "SELECT card_id,nickname FROM users WHERE id=?", (self.user_id,)).fetchone(),
                ("11223344", "NEW NAME"))

    def test_native_save_helpers_merge_with_latest_web_profile(self):
        playerdata.update_player(self.db, self.user_id, {"nickname": "WEB", "title": "43", "avatar": "7"})
        prefix = bytes(range(36))
        playerdata.save_prefix(self.db, self.user_id, prefix)
        result = bytearray(108)
        struct.pack_into("<I", result, 0x1c, self.user_id)
        struct.pack_into("<Q", result, 0x40, 0x123456789abcdef0)
        struct.pack_into("<2H", result, 0x48, 0x400, 44)
        struct.pack_into("<i", result, 0x4c, 487)
        struct.pack_into("<f", result, 0x50, 8)
        playerdata.save_result_options(self.db, self.user_id, result)
        profile = playerdata.load_profile(self.db, "11223344")[1]
        self.assertEqual(profile[:36], prefix)
        self.assertEqual(profile[0x24:0x28], b"WEB\0")
        self.assertEqual(profile[0x40:0x43], b"+\0\7")
        self.assertEqual(profile[0x4c:0x58], result[0x40:0x4c])
        self.assertEqual(struct.unpack_from("<H", profile, 0x58)[0], 487)
        self.assertEqual(struct.unpack_from("<f", profile, 0x5c)[0], 8)
        self.assertEqual([row[0] for row in self.profile_rows()], [1, 2, 3, 4])

    def test_rejected_edits_do_not_partially_write(self):
        for key, value in (("avatar", "164"), ("title", "213"), ("skin", "45"),
                           ("auto_velocity", "299"), ("auto_velocity", "1000"),
                           ("timing", "1"), ("speed", "3"), ("judgement", str(0x800000)),
                           ("nickname", "TOO LONG NAME"), ("nickname", "bad\nname"),
                           ("nickname", "nonasciié"), ("profile_id", "2")):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                playerdata.update_player(self.db, self.user_id, {"nickname": "CHANGED", key: value})
        self.assertEqual(self.profile_rows(), [(1, self.initial)])
        playerdata.update_player(self.db, self.user_id, {"nickname": "CHANGED"}, expected_version=1)
        with self.assertRaises(playerdata.StaleProfile):
            playerdata.update_player(self.db, self.user_id, {"nickname": "STALE"}, expected_version=1)
        self.assertEqual(playerdata.get_player(self.db, self.user_id)["fields"]["nickname"], "CHANGED")

    def test_missing_and_mismatched_players_are_not_registered(self):
        with self.assertRaises(KeyError):
            playerdata.get_player(self.db, self.user_id + 1)
        with self.assertRaises(KeyError):
            playerdata.update_player(self.db, self.user_id + 1, {"nickname": "MISSING"})
        result = bytearray(108)
        struct.pack_into("<I", result, 0x1c, self.user_id + 1)
        with self.assertRaises(ValueError):
            playerdata.save_result_options(self.db, self.user_id, result)
        self.assertEqual(len(playerdata.list_players(self.db)), 1)
        self.assertEqual(self.profile_rows(), [(1, self.initial)])


if __name__ == "__main__":
    unittest.main()
