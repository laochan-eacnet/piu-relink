"""Publication preservation tests.

Run: python -m unittest discover -s tools -p 'test_package.py'
"""
from pathlib import Path
import configparser
import os
import signal
import subprocess
import tempfile
import unittest
from unittest import mock

import package
import run


class MergeIniTests(unittest.TestCase):
    def test_existing_values_blank_values_and_custom_text_are_unchanged(self):
        existing = (
            "# Cabinet-specific settings; do not replace.\n"
            "[mounts]\n/piu/data = X:/Phoenix/data\n/custom = Z:/shared\n"
            "[display]\nmode = borderless\nwidth = 1920\nheight = 1080\n"
            "[server]\napi_base = http://192.168.1.50:8088/private\ncdn_base =\n"
            "[input]\nvirtual_lxio = false\nupper_left = KP_Home\nreader_port = COM7\n"
            "[paths]\noverlay_root =\n"
            "\n; Operator extension\n[custom]\nopaque = keep = this ; literally\n"
        )
        defaults = (
            "[mounts]\n/piu/data = data\n"
            "[display]\nmode = windowed\nwidth = 0\nheight = 0\n"
            "[server]\napi_base =\ncdn_base = http://127.0.0.1:8080\n"
            "[input]\nvirtual_lxio = true\nupper_left = Q\n"
            "[paths]\noverlay_root = runtime/overlay\n"
        )
        self.assertEqual(package.merge_ini(existing, defaults), existing)


    def test_crlf_and_case_insensitive_section_and_option_matching(self):
        existing = (
            "# Windows cabinet\r\n[DiSpLaY]\r\nMoDe = fullscreen\r\nWiDtH =\r\n"
            "[SERVER]\r\nAPI_BASE = http://10.0.0.8:8080\r\n"
        )
        defaults = (
            "[display]\nmode = windowed\nwidth = 0\n# Automatic height.\nheight = 0\n"
            "[server]\napi_base =\n"
        )
        merged = package.merge_ini(existing, defaults)
        self.assertIn("[DiSpLaY]\r\nMoDe = fullscreen\r\nWiDtH =\r\n", merged)
        self.assertIn("# Automatic height.\r\nheight = 0\r\n", merged)
        self.assertIn("[SERVER]\r\nAPI_BASE = http://10.0.0.8:8080\r\n", merged)
        for unwanted in ("[display]", "[server]", "mode = windowed", "width = 0", "api_base ="):
            self.assertNotIn(unwanted, merged)
        self.assertNotIn("\n", merged.replace("\r\n", ""))
        self.assertEqual(package.merge_ini(merged, defaults), merged)

    def test_new_sections_keep_their_header_comments(self):
        existing = "[server]\napi_base =\n"
        defaults = (
            "[server]\napi_base =\n"
            "# Read-only data.\n[immutable]\n/piu/data = yes\n"
            "# Window mode.\n[display]\nmode = windowed\n"
        )
        merged = package.merge_ini(existing, defaults)
        self.assertIn("\n# Read-only data.\n[immutable]\n/piu/data = yes\n", merged)
        self.assertIn("\n# Window mode.\n[display]\nmode = windowed\n", merged)
        self.assertNotIn("/piu/data = yes\n# Window mode.", merged)
        self.assertEqual(package.merge_ini(merged, defaults), merged)

    def test_existing_file_without_terminating_newline(self):
        existing = "# Intentionally disconnected\n[server]\napi_base ="
        unchanged_defaults = "[SERVER]\nAPI_BASE = http://127.0.0.1:8080\n"
        self.assertEqual(package.merge_ini(existing, unchanged_defaults), existing)
        defaults = unchanged_defaults + "# Separate CDN route.\ncdn_base =\n"
        merged = package.merge_ini(existing, defaults)
        self.assertTrue(merged.startswith(existing + "\n"))
        self.assertIn("# Separate CDN route.\ncdn_base =\n", merged)
        self.assertNotIn("http://127.0.0.1:8080", merged)
        self.assertEqual(package.merge_ini(merged, defaults), merged)



    def test_missing_defaults_become_effective_without_replacing_operator_choices(self):
        existing = "[INPUT]\nvirtual_lxio = false\ncard1 = F9\n[server]\napi_base =\n"
        defaults = ("[input]\nvirtual_lxio = true\nvirtual_card_reader = false\n"
                    "card1 = F3\ncard2 = F4\n[server]\napi_base = http://127.0.0.1:8080\n"
                    "[paths]\nruntime_dir = runtime\n")
        merged = package.merge_ini(existing, defaults)
        parser = configparser.ConfigParser()
        parser.read_string(merged)
        actual = {section.casefold(): dict(parser.items(section)) for section in parser.sections()}
        self.assertEqual(actual["input"], {"virtual_lxio": "false", "virtual_card_reader": "false",
                                          "card1": "F9", "card2": "F4"})
        self.assertEqual(actual["server"]["api_base"], "")
        self.assertEqual(actual["paths"]["runtime_dir"], "runtime")
        self.assertEqual(package.merge_ini(merged, defaults), merged)


class MergeConfigTests(unittest.TestCase):
    def test_merge_preserves_crlf_and_repeat_merge_does_not_write(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "phoenixtools.ini"
            path.write_bytes(b"[DISPLAY]\r\nMode = borderless\r\n")
            defaults = "[display]\nmode = windowed\n# Automatic width.\nwidth = 0\n"
            package.merge_config(path, defaults)
            merged = path.read_bytes()
            self.assertIn(b"[DISPLAY]\r\nMode = borderless\r\n", merged)
            self.assertIn(b"# Automatic width.\r\nwidth = 0\r\n", merged)
            self.assertNotIn(b"\n", merged.replace(b"\r\n", b""))
            old_time = 946684800000000000
            os.utime(path, ns=(old_time, old_time))
            before = path.stat().st_mtime_ns
            package.merge_config(path, defaults)
            self.assertEqual(path.read_bytes(), merged)
            self.assertEqual(path.stat().st_mtime_ns, before)

    def test_legacy_runtime_settings_migrate_before_new_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "phoenixtools.ini"
            path.write_bytes(
                b"# Existing unified settings\r\n"
                b"[server]\r\napi_base = http://192.168.1.50:8080\r\ncdn_base =\r\n"
                b"[display]\r\nheight = 1080\r\n"
                b"[input]\r\nvirtual_card_reader = false\r\n"
            )
            legacy = path.with_name("lxrt.ini")
            legacy.write_bytes(
                b"[mounts]\r\n/piu/data = X:/Phoenix/data\r\n/custom = Z:/shared\r\n"
                b"[immutable]\r\n/piu/data = no\r\n"
                b"[process]\r\ncwd = /piu\r\n"
                b"[display]\r\nmode = fullscreen\r\nwidth = 1920\r\nheight = 720\r\n"
                b"[audio]\r\nmode = exclusive\r\n"
                b"[custom]\r\nopaque = keep = this\r\n"
            )
            defaults = (
                "[server]\napi_base =\ncdn_base =\ngame_port = 15040\n"
                "[input]\nvirtual_card_reader = true\n"
                "[mounts]\n/piu/data = data\n"
                "[immutable]\n/piu/data = yes\n"
                "[display]\nmode = windowed\nwidth = 0\nheight = 0\n"
                "[audio]\nmode = shared\n"
                "[serial]\n# /dev/ttyUSB0 = COM3\n"
                "[debug]\ntrace = false\nlog =\n"
            )
            package.merge_config(path, defaults)
            merged = path.read_bytes()
            self.assertFalse(legacy.exists())
            self.assertNotIn(b"\n", merged.replace(b"\r\n", b""))
            parser = configparser.ConfigParser()
            parser.read_string(merged.decode("utf-8"))
            self.assertEqual(dict(parser["mounts"]),
                             {"/piu/data": "X:/Phoenix/data", "/custom": "Z:/shared"})
            self.assertEqual(parser["immutable"]["/piu/data"], "no")
            self.assertEqual(parser["process"]["cwd"], "/piu")
            self.assertEqual(dict(parser["display"]),
                             {"mode": "fullscreen", "width": "1920", "height": "1080"})
            self.assertEqual(parser["audio"]["mode"], "exclusive")
            self.assertEqual(parser["custom"]["opaque"], "keep = this")
            self.assertEqual(parser["server"]["api_base"], "http://192.168.1.50:8080")
            self.assertEqual(parser["server"]["cdn_base"], "")
            self.assertEqual(parser["server"]["game_port"], "15040")
            self.assertEqual(parser["input"]["virtual_card_reader"], "false")
            self.assertEqual(dict(parser["serial"]), {})
            self.assertEqual(dict(parser["debug"]), {"trace": "false", "log": ""})
            os.utime(path, ns=(946684800000000000, 946684800000000000))
            before = path.stat().st_mtime_ns
            package.merge_config(path, defaults)
            self.assertEqual(path.read_bytes(), merged)
            self.assertEqual(path.stat().st_mtime_ns, before)
            self.assertFalse(legacy.exists())

    def test_legacy_config_migrates_when_unified_config_is_absent(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "phoenixtools.ini"
            legacy = path.with_name("lxrt.ini")
            legacy.write_bytes(b"# Original runtime settings\r\n[DISPLAY]\r\nMode = borderless")
            defaults = "[display]\nmode = windowed\nwidth = 0\n[server]\ngame_port = 15040\n"
            package.merge_config(path, defaults)
            merged = path.read_bytes()
            self.assertTrue(merged.startswith(
                b"# Original runtime settings\r\n[DISPLAY]\r\nMode = borderless\r\n"))
            self.assertIn(b"width = 0\r\n", merged)
            self.assertIn(b"[server]\r\ngame_port = 15040\r\n", merged)
            self.assertFalse(legacy.exists())
            package.merge_config(path, defaults)
            self.assertEqual(path.read_bytes(), merged)


class RunConfigTests(unittest.TestCase):
    def test_overrides_preserve_unrelated_text_and_last_setting_wins(self):
        original = (
            b"\xef\xbb\xbf# Cabinet settings\r\n[DeBuG]\r\nTRACE = false\r\nlog = old.log\r\n"
            b"[server]\r\ngame_port = 15040"
        )
        settings = [run.setting(value) for value in (
            "debug.trace=false", "DEBUG.TRACE=true", "debug.log=",
            "debug.snapshot_dir=frames=all", "server.game_port=15041",
            "audio.mode=exclusive",
        )]
        changed = run.override_ini(original, settings)
        self.assertTrue(changed.startswith(b"\xef\xbb\xbf# Cabinet settings\r\n[DeBuG]\r\n"))
        self.assertIn(b"TRACE = true\r\nlog = \r\n", changed)
        self.assertNotIn(b"\n", changed.replace(b"\r\n", b""))
        parser = configparser.ConfigParser()
        parser.read_string(changed.decode("utf-8-sig"))
        actual = {section.casefold(): dict(parser.items(section)) for section in parser.sections()}
        self.assertEqual(actual["debug"], {
            "trace": "true", "log": "", "snapshot_dir": "frames=all",
        })
        self.assertEqual(actual["server"]["game_port"], "15041")
        self.assertEqual(actual["audio"]["mode"], "exclusive")

    def test_original_config_is_restored_on_all_managed_exit_paths(self):
        for scenario in ("normal", "timeout", "interrupt", "terminate",
                         "launch_error", "injector_error"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                exe = root / "piu.exe"
                ini = root / "phoenixtools.ini"
                original = b"\xef\xbb\xbf# Cabinet\r\n[debug]\r\ntrace = false\r\nlog ="
                ini.write_bytes(original)
                target = mock.Mock()
                injector = mock.Mock()
                target.poll.return_value = injector.poll.return_value = None
                injector.wait.return_value = 0

                def wait(timeout=None):
                    if timeout is not None:
                        if scenario == "timeout":
                            raise subprocess.TimeoutExpired(str(exe), timeout)
                        if scenario == "interrupt":
                            raise KeyboardInterrupt
                        if scenario == "terminate":
                            run.terminate(signal.SIGTERM, None)
                    return 0

                target.wait.side_effect = wait
                launched = []

                def popen(command, **kwargs):
                    self.assertIn(b"trace = true\r\n", ini.read_bytes())
                    launched.append((command, kwargs))
                    if scenario == "launch_error":
                        raise OSError("program launch failed")
                    if len(launched) == 1:
                        return target
                    if scenario == "injector_error":
                        raise OSError("injector launch failed")
                    return injector

                argv = ["run.py", "-t", "0.01", "-o", str(root / "run.log"),
                        "-C", str(root), "-s", "debug.trace=true", "-i", "input.txt",
                        str(exe), "game-argument"]
                expected = {"interrupt": KeyboardInterrupt, "terminate": SystemExit,
                            "launch_error": OSError, "injector_error": OSError}
                with mock.patch.object(run.sys, "argv", argv), \
                        mock.patch.object(run.subprocess, "Popen", side_effect=popen), \
                        mock.patch("builtins.print"):
                    if scenario in expected:
                        with self.assertRaises(expected[scenario]):
                            run.main()
                    else:
                        run.main()
                self.assertEqual(ini.read_bytes(), original)
                self.assertEqual(launched[0][0], [str(exe.resolve()), "game-argument"])
                self.assertEqual(launched[0][1]["cwd"], str(root))
                if scenario != "launch_error":
                    target.kill.assert_called()
                    target.wait.assert_called()
                if scenario not in ("launch_error", "injector_error"):
                    injector.kill.assert_called_once()
                    injector.wait.assert_called_once()
                for _, kwargs in launched:
                    if kwargs["stdout"] is not None:
                        self.assertTrue(kwargs["stdout"].closed)

    def test_absent_config_is_removed_after_run_or_launch_failure(self):
        for launch_error in (False, True):
            with self.subTest(launch_error=launch_error), tempfile.TemporaryDirectory() as directory:
                exe = Path(directory) / "piu.exe"
                ini = exe.with_name("phoenixtools.ini")
                target = mock.Mock()
                target.poll.return_value = 0
                target.wait.return_value = 0

                def popen(command, **kwargs):
                    self.assertEqual(ini.read_bytes(), b"[debug]\ntrace = true\n")
                    if launch_error:
                        raise OSError("program launch failed")
                    return target

                argv = ["run.py", "-s", "debug.trace=true", str(exe)]
                with mock.patch.object(run.sys, "argv", argv), \
                        mock.patch.object(run.subprocess, "Popen", side_effect=popen), \
                        mock.patch("builtins.print"):
                    if launch_error:
                        with self.assertRaises(OSError):
                            run.main()
                    else:
                        run.main()
                self.assertFalse(ini.exists())



class PublishServerTests(unittest.TestCase):
    def test_publication_updates_code_without_copying_or_touching_user_state(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            destination = Path(directory) / "published"
            modules = {
                "piuserver.py": b"# updated server\n",
                "gameplay.py": b"# current gameplay\n",
            }
            source_files = {f"server/{name}": content for name, content in modules.items()}
            source_files.update({
                "tools/package.py": b"# fake module location\n",
                "tools/start-server.cmd": b"@python server/piuserver.py\r\n",
                "server/test_piuserver.py": b"# source tests\n",
                "server/__pycache__/piuserver.cpython-314.pyc": b"source bytecode",
                "server/piu.db": b"SQLite format 3\0source database",
                "server/players.sqlite3": b"SQLite format 3\0source player database",
                "server/runtime/source-only.bin": b"source server runtime",
                "server/data/source-only.bin": b"source server data",
                "runtime/source-only.bin": b"source runtime",
                "data/source-only.bin": b"source data",
            })
            destination_files = {
                "server/piuserver.py": b"# obsolete server\n",
                "start-server.cmd": b"@echo obsolete launcher\r\n",
                "server/piu.db": b"SQLite format 3\0existing server users",
                "players.sqlite3": b"SQLite format 3\0existing users",
                "runtime/virtual-sda.img": b"existing virtual disk\0",
                "runtime/cards.db": b"SQLite format 3\0existing runtime cards",
                "data/song.bin": b"existing mounted game data",
                "card1.txt": b"11223344\r\n",
            }
            for root, files in ((source, source_files), (destination, destination_files)):
                for relative, content in files.items():
                    path = root / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(content)

            preserved = {}
            for relative, content in destination_files.items():
                if relative not in ("server/piuserver.py", "start-server.cmd"):
                    path = destination / relative
                    os.utime(path, ns=(946684800000000000, 946684800000000000))
                    preserved[relative] = (content, path.stat().st_mtime_ns)
            directory_times = {
                name: (destination / name).stat().st_mtime_ns for name in ("runtime", "data")
            }

            with mock.patch.object(package, "__file__", str(source / "tools" / "package.py")):
                for publication in range(2):
                    with self.subTest(publication=publication + 1):
                        package.publish_server(destination)
                        files = {
                            path.relative_to(destination).as_posix(): path
                            for path in destination.rglob("*") if path.is_file()
                        }
                        for excluded in ("server/test_piuserver.py", "server/players.sqlite3",
                                         "server/__pycache__/piuserver.cpython-314.pyc",
                                         "server/runtime/source-only.bin", "server/data/source-only.bin",
                                         "runtime/source-only.bin", "data/source-only.bin"):
                            self.assertNotIn(excluded, files)
                        self.assertEqual(files["card2.txt"].read_bytes(), b"")
                        state = {
                            name: (path.read_bytes(), path.stat().st_mtime_ns)
                            for name, path in files.items()
                        }
                        for name, original in preserved.items():
                            self.assertEqual(state[name], original)
                        self.assertEqual(
                            {name: (destination / name).stat().st_mtime_ns for name in directory_times},
                            directory_times,
                        )


if __name__ == "__main__":
    unittest.main()
