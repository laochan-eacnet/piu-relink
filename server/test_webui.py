"""Server-rendered form round trips, rejection, and output escaping."""
from contextlib import closing
from io import BytesIO
from pathlib import Path
from types import SimpleNamespace
import sqlite3
import tempfile
import unittest
import urllib.parse

import gameplay
import playerdata
import piuserver
import webui


class Request:
    def __init__(self, db, path, method="GET", data=b"", content_type="application/x-www-form-urlencoded"):
        self.server = SimpleNamespace(db=db)
        self.path = path
        self.command = method
        self.headers = {"Content-Type": content_type, "Content-Length": str(len(data))}
        self.rfile = BytesIO(data)
        self.wfile = BytesIO()
        self.status = None
        self.response_headers = {}

    def send_response(self, status):
        self.status = status

    def send_header(self, name, value):
        self.response_headers[name] = value

    def end_headers(self):
        pass

    def body(self):
        return self.wfile.getvalue().decode("utf-8")


class WebUITests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.db = str(Path(self.directory.name) / "players.db")
        with closing(sqlite3.connect(self.db)) as connection:
            connection.executescript(piuserver.SCHEMA + playerdata.PROFILE_SCHEMA + gameplay.SCHEMA)
        self.user_id, _ = playerdata.load_profile(self.db, "01020304")
        self.path = f"/players/{self.user_id}"

    def values(self):
        player = playerdata.get_player(self.db, self.user_id)
        values = {"nickname": player["fields"]["nickname"], "version": str(player["version"])}
        for field in playerdata.SETTINGS:
            value = player["fields"][field.key]
            values[field.key] = str(int(value))
        return values

    def post(self, values):
        body = urllib.parse.urlencode(values).encode("utf-8")
        request = Request(self.db, self.path, "POST", body)
        self.assertTrue(webui.handle(request))
        return request

    def test_profile_overview_shows_shared_progression_and_reward_labels(self):
        with closing(sqlite3.connect(self.db)) as connection, connection:
            connection.execute(
                "INSERT INTO title_rewards(user_id,title_id,event_id,awarded) VALUES(?,?,?,?)",
                (self.user_id, 212, 1, "2026-10-17T12:00:00Z"))
            connection.execute(
                "INSERT INTO best_records(user_id,song_id,selector,mode,record,timestamp) VALUES(?,?,?,?,?,?)",
                (self.user_id, 1870, 0, 0, bytes(24), "2026-10-17T12:00:00Z"))
            connection.execute(
                "INSERT INTO scores(user_id,song_id,chart,score,timestamp) VALUES(?,?,?,?,?)",
                (self.user_id, "1870", "0", 900000, "2026-10-17T12:00:00Z"))
        request = Request(self.db, self.path)
        self.assertTrue(webui.handle(request))
        self.assertEqual(request.status, 200)
        self.assertIn("Uploaded played results: <strong>1</strong>", request.body())
        self.assertIn("Charts with native best records: <strong>1</strong>", request.body())
        self.assertIn("#212: DOMINION CHALLENGER", request.body())
        self.assertIn("2026-10-17T12:00:00Z", request.body())

    def test_registered_player_select_save_reload_and_next_login(self):
        listing = Request(self.db, "/players")
        self.assertTrue(webui.handle(listing))
        self.assertEqual(listing.status, 200)
        self.assertIn(f"href='{self.path}'", listing.body())
        values = self.values()
        values.update(nickname="<N&\"'", avatar="163", title="212", skin="44", auto_velocity="700",
                      speed="26", timing="10", flash="4", hide_judgement=str(0x100000000))
        response = self.post(values)
        self.assertEqual(response.status, 303)
        self.assertEqual(response.response_headers["Location"], self.path + "?saved=1")
        view = Request(self.db, self.path + "?saved=1")
        self.assertTrue(webui.handle(view))
        self.assertEqual(view.status, 200)
        self.assertIn("Saved.", view.body())
        self.assertIn("&lt;N&amp;&quot;&#x27;", view.body())
        self.assertNotIn("<N&", view.body())
        self.assertIn("<option value='163' selected>", view.body())
        self.assertIn("<option value='212' selected>", view.body())
        user_id, profile = playerdata.load_profile(self.db, "01020304")
        self.assertEqual(user_id, self.user_id)
        self.assertEqual(profile[0x24:0x2a], b"<N&\"'\0")
        self.assertEqual(profile[0x40], 212)
        self.assertEqual(profile[0x42], 163)
        fields = playerdata.profile_fields(profile)
        for key in ("skin", "auto_velocity", "speed", "timing", "flash", "hide_judgement"):
            self.assertEqual(fields[key], int(values[key]))

    def test_invalid_duplicate_unknown_and_stale_forms_are_rejected(self):
        values = self.values()
        invalid = dict(values, nickname="CHANGED", avatar="999")
        self.assertEqual(self.post(invalid).status, 400)
        self.assertNotEqual(playerdata.get_player(self.db, self.user_id)["fields"]["nickname"], "CHANGED")
        duplicate = list(values.items()) + [("nickname", "DUPLICATE")]
        self.assertEqual(self.post(duplicate).status, 400)
        self.assertEqual(self.post(dict(values, profile_id="2")).status, 400)
        self.assertEqual(self.post(dict(values, nickname="CHANGED")).status, 303)
        self.assertEqual(self.post(values).status, 409)
        self.assertEqual(playerdata.get_player(self.db, self.user_id)["fields"]["nickname"], "CHANGED")

    def test_unknown_native_values_can_be_kept_without_normalizing_bytes(self):
        with closing(sqlite3.connect(self.db)) as connection, connection:
            profile = bytearray(connection.execute("SELECT save_data FROM profiles").fetchone()[0])
            profile[0x42] = 164  # Missing numbered resource; preserve imported data, do not offer it.
            profile[0x54:0x56] = b"\0\0"
            connection.execute("UPDATE profiles SET save_data=?", (profile,))
        request = Request(self.db, self.path)
        self.assertTrue(webui.handle(request))
        self.assertIn("Keep existing native value (164)", request.body())
        values = self.values()
        values.update(avatar="keep", speed="keep", nickname="KEPT")
        self.assertEqual(self.post(values).status, 303)
        _, saved = playerdata.load_profile(self.db, "01020304")
        self.assertEqual(saved[0x42], 164)
        self.assertEqual(saved[0x54:0x56], b"\0\0")

    def test_non_web_routes_fall_through_and_unknown_players_do_not_register(self):
        request = Request(self.db, "/piu.module/piu.get_ip_to_country.php")
        self.assertFalse(webui.handle(request))
        self.assertIsNone(request.status)
        for path in ("/players/999999", "/players/<script>", "/players/1/extra"):
            with self.subTest(path=path):
                request = Request(self.db, path)
                self.assertTrue(webui.handle(request))
                self.assertEqual(request.status, 404)
        self.assertEqual(len(playerdata.list_players(self.db)), 1)


if __name__ == "__main__":
    unittest.main()
