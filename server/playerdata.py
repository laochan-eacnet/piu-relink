"""SQLite player records and the original Phoenix 2.12.0 customization layout.

The 112-byte record R is copied by 0x080654f6. Display fields are consumed
at 0x0809d7ce/0x08064870: nickname +0x24, discriminator +0x34, identity
0x3c, title +0x40, avatar +0x42. Title IDs/names come from 213 entries at
0x081c57a0; avatar loading at 0x08074b95 supports numbered resources, of
which data/USER_INTERFACE/AVATAR/{SMALL,BIG} contains 000..163.

0x080b495e applies R+0x4c (64-bit modifiers), +0x54 (speed), +0x56
(noteskin), +0x58 (auto velocity), +0x5c (float timing adjustment).
0x080bb70b writes the same fields back. Command masks are the 28-byte
entries at 0x081d5040 and 0x081d5720; skins are 45 entries at 0x081d5480,
matching data/USER_INTERFACE/PLAY/SKIN/00..44/*.CFG. Timing values are
11 entries at 0x081d53c0, not symmetric milliseconds: -15..0 by 3 and
+2..+10 by 2. Auto velocity is clamped to 300..999 by 0x080c5879.

Only proven fields are editable. Login clears the very-hard judgement bit
0x800000, so that command is deliberately not offered. Legacy commands
whose modifier bits are still consumed are labelled as such. Rush, song
sorting, stage-pass mode and premium/full-mode selection are session state,
not fields of this profile; they are not invented as persistent settings.
All other bytes and unknown modifier bits survive edits unchanged.
"""
from contextlib import closing
from dataclasses import dataclass
import datetime
import json
import sqlite3
import struct

PROFILE_SIZE = 0x70
PROFILE_SCHEMA = ""

TITLE_LABELS = (
    ("BEGINNER", "EXC FOLLOWER", "A huge fan of EXC", "FEFEMZ FOLLOWER",
     "A huge fan of FEFEMZ", "SUNNY FOLLOWER", "A huge fan of SUNNY",
     "DULKI FOLLOWER", "A huge fan of DULKI", "CONRAD FOLLOWER",
     "A huge fan of CONRAD", "SPHAM FOLLOWER", "A huge fan of SPHAM")
    + tuple(f"{group} Lv.{level}" for group in ("INTERMEDIATE", "ADVANCED", "EXPERT")
            for level in range(1, 11))
    + ("THE MASTER",)
    + tuple(f"[{group}] {level}" for group in
            ("TWIST", "RUN", "DRILL", "GIMMICK", "HALF", "BRACKET")
            for level in (*(f"Lv.{number}" for number in range(1, 11)), "EXPERT"))
    + ("SPECIALIST",)
    + tuple(f"[CO-OP] Lv.{level}" for level in range(1, 11))
    + ("[CO-OP] ADVANCED", "[CO-OP] EXPERT", "[CO-OP] MASTER")
    + tuple(f"{name} GAMER" for name in
            ("ROUGH", "FAIR", "TALENTED", "MARVELOUS", "SUPERB", "EXTREME", "ULTIMATE", "PERFECT")
            for _ in range(4))
    + tuple(f"[{name}] Boss breaker" for name in
            ("The 1st", "The 2nd", "The O.B.G", "The O.B.G SE", "Perfect Collection",
             "EXTRA", "THE REBIRTH", "THE PREX3"))
    + tuple(f"[{name}] {mode} Boss breaker" for name in
            ("EXCEED", "EXCEED2", "ZERO", "NX", "NX2", "NXA", "FIESTA", "FIESTA EX",
             "FIESTA2", "PRIME", "PRIME2", "XX") for mode in ("Single", "Double"))
    + ("GOLD MEMBER", "PLATINUM MEMBER", "DIAMOND MEMBER", "VIP MEMBER", "VVIP MEMBER",
       "DOGE MAJOR STOCKHOLDER", "B.P.M FOLLOWER", "PUMP IS A SENSE", "NO SKILLS NO PUMP",
       "Human metronome", "Perfect breaker", "CHEATER", "SCROOGE", "LOVERS", "LOVERS",
       "PIU STEPMAKER", "PIU ARTIST", "PIU GUARDIAN", "PIU SUPPORTER", "PIU STAFF",
       "B1G ONE 2024 PLAYER", "B1G ONE 2024 SUPPORTER", "PHOENIX CHALLENGER",
       "RISE CHALLENGER", "DOMINION CHALLENGER")
)
SKIN_LABELS = (
    "Default", "Hwatoo", "Old", "EZ", "Slime", "Note", "Canon", "Card", "NX", "Sheep",
    "Horse", "Dog", "Girl", "Fire", "Ice", "Wind", "Left (legacy)", "Right (legacy)",
    "Both (legacy)", "NXA", "NX2", "Lightning", "Drum", "Missile", "Drum B (legacy)",
    "Drum R (legacy)", "Drum Y (legacy)", "Football", "Rebirth", "Basic", "Fiesta",
    "Fiesta 2", "Prime", "Prime 2", "XX", "Co-op Red", "Co-op Blue", "Co-op Yellow",
    "Co-op Green", "Co-op Purple", "2023 Premium", "Contest Chiduck", "Contest Toy",
    "Contest Nekoneko", "Contest Canondgirl",
)


@dataclass(frozen=True)
class Setting:
    key: str
    label: str
    offset: int
    fmt: str
    choices: tuple
    mask: int = 0

    def read(self, profile):
        value = struct.unpack_from("<" + self.fmt, profile, self.offset)[0]
        return value & self.mask if self.mask else value

    def write(self, profile, value):
        if self.mask:
            old = struct.unpack_from("<" + self.fmt, profile, self.offset)[0]
            value = (old & ~self.mask) | value
        struct.pack_into("<" + self.fmt, profile, self.offset, value)


def flag(key, label, bit):
    return Setting(key, label, 0x4c, "Q", ((0, "Off"), (bit, "On")), bit)


SETTINGS = (
    Setting("avatar", "Avatar", 0x42, "B", tuple((i, f"Avatar {i:03d}") for i in range(164))),
    Setting("title", "Title", 0x40, "B", tuple((i, f"#{i}: {name}") for i, name in enumerate(TITLE_LABELS))),
    Setting("speed", "Scroll speed", 0x54, "H",
            ((0x400, "Auto velocity"),)
            + tuple((i * 2, f"x{i / 2:g}") for i in range(2, 14))
            + ((0x100, "Random velocity"), (0x200, "Earthworm"))),
    Setting("skin", "Noteskin", 0x56, "H", tuple(enumerate(SKIN_LABELS))),
    Setting("auto_velocity", "Auto velocity (300-999)", 0x58, "H", ()),
    Setting("timing", "Timing adjustment (native command)", 0x5c, "f",
            tuple(zip((-15, -12, -9, -6, -3, 0, 2, 4, 6, 8, 10),
                      ("-5", "-4", "-3", "-2", "-1", "0", "+1", "+2", "+3", "+4", "+5")))),
    Setting("acceleration", "Acceleration", 0x4c, "Q",
            ((0, "Normal"), (0x400, "Accelerate"), (0x200, "Decelerate")), 0x600),
    Setting("visibility", "Arrow visibility", 0x4c, "Q",
            ((0, "Normal"), (1, "Vanish"), (2, "Appear"), (3, "Nonstep")), 3),
    flag("flash", "Flash", 4),
    flag("freedom", "Freedom", 8),
    flag("random_skin", "Random noteskin", 0x40),
    flag("bga_dark", "Dark background", 0x800),
    flag("path_exceed", "Path: Exceed", 0x1000),
    flag("path_nx", "Path: NX", 0x2000),
    flag("path_under_attack", "Path: Under Attack", 0x80),
    flag("path_drop", "Path: Drop", 0x4000),
    Setting("path_vertical", "Path: vertical", 0x4c, "Q",
            ((0, "Normal"), (0x80000, "Sink"), (0x100000, "Rise")), 0x180000),
    flag("path_snake", "Path: Snake", 0x200000),
    flag("path_zigzag", "Path: Zigzag (legacy)", 0x4000000),
    Setting("alternate", "Arrow arrangement", 0x4c, "Q",
            ((0, "Normal"), (0x10, "Mirror"), (0x20, "Random"),
             (0x1000000, "Decalcomanie (legacy)")), 0x1000030),
    flag("runner", "Runner (legacy, shared by both players)", 0x8000000),
    Setting("judgement", "Judgement", 0x4c, "Q",
            ((0, "Normal"), (0x400000, "Hard"), (0x20000000, "Easy (legacy)"),
             (0x40000000, "Extra Hard (legacy)"), (0x80000000, "Ultra Hard (legacy)")), 0xe0c00000),
    flag("reverse_grade", "Reverse grade", 0x100),
    flag("hide_judgement", "Hide judgement (legacy)", 0x100000000),
    flag("judge_by_note", "Judge by note (legacy)", 0x200000000),
    flag("free_performance", "Free performance (legacy)", 0x10000000),
)
SETTING_BY_KEY = {field.key: field for field in SETTINGS}


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")


def validate_profile(profile, user_id):
    profile = bytes(profile)
    if len(profile) != PROFILE_SIZE:
        raise ValueError("stored local profile is not 112 bytes")
    if not 1 <= user_id <= 0xffffffff or struct.unpack_from("<I", profile, 0x3c)[0] != user_id:
        raise ValueError("stored profile ID does not match its local user")
    if b"\0" not in profile[0x24:0x34]:
        raise ValueError("stored profile nickname is not terminated")
    return profile


def nickname_bytes(nickname):
    try:
        encoded = nickname.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError("nickname must contain 1-12 printable ASCII characters") from error
    if not 1 <= len(encoded) <= 12 or any(c < 32 or c > 126 for c in encoded):
        raise ValueError("nickname must contain 1-12 printable ASCII characters")
    return encoded


def new_profile(user_id, nickname):
    if not 1 <= user_id <= 0xffffffff:
        raise ValueError("local profile ID outside nonzero u32")
    encoded = nickname_bytes(nickname)
    profile = bytearray(PROFILE_SIZE)
    profile[0x24:0x24 + len(encoded)] = encoded
    struct.pack_into("<I", profile, 0x34, user_id % 10000)
    struct.pack_into("<I", profile, 0x3c, user_id)
    struct.pack_into("<H", profile, 0x54, 0x400)
    struct.pack_into("<H", profile, 0x58, 350)
    return bytes(profile)


def profile_fields(profile):
    fields = {field.key: field.read(profile) for field in SETTINGS}
    fields.update(nickname=profile[0x24:0x34].split(b"\0", 1)[0].decode("ascii", errors="replace"),
                  discriminator=struct.unpack_from("<I", profile, 0x34)[0],
                  profile_id=struct.unpack_from("<I", profile, 0x3c)[0])
    return fields


def _latest(connection, user_id):
    saved = connection.execute(
        "SELECT version,save_data FROM profiles WHERE user_id=? ORDER BY version DESC,id DESC LIMIT 1",
        (user_id,)).fetchone()
    if saved:
        return saved[0], validate_profile(saved[1], user_id)
    row = connection.execute("SELECT nickname FROM users WHERE id=?", (user_id,)).fetchone()
    if not row:
        raise KeyError(user_id)
    return 0, new_profile(user_id, row[0])


def _append(connection, user_id, version, profile):
    profile = validate_profile(profile, user_id)
    fields = profile_fields(profile)
    connection.execute(
        "INSERT INTO profiles(user_id,version,save_data,parsed_fields,timestamp) VALUES(?,?,?,?,?)",
        (user_id, version + 1, profile, json.dumps(fields, separators=(",", ":")), now()))
    connection.execute("UPDATE users SET nickname=? WHERE id=?", (fields["nickname"], user_id))
    return version + 1


def load_profile(db, card_id):
    timestamp = now()
    with closing(sqlite3.connect(db)) as connection, connection:
        connection.execute("PRAGMA foreign_keys=ON")
        connection.execute("BEGIN IMMEDIATE")
        connection.execute(
            "INSERT OR IGNORE INTO users(card_id,nickname,created,last_login) VALUES(?,?,?,?)",
            (card_id, "LOCAL" + card_id[-7:].upper(), timestamp, timestamp))
        user_id = connection.execute("SELECT id FROM users WHERE card_id=?", (card_id,)).fetchone()[0]
        version, profile = _latest(connection, user_id)
        if not version:
            _append(connection, user_id, version, profile)
        connection.execute("UPDATE users SET last_login=? WHERE id=?", (timestamp, user_id))
    return user_id, profile


def list_players(db):
    with closing(sqlite3.connect(db)) as connection:
        return connection.execute(
            "SELECT id,card_id,nickname,last_login FROM users ORDER BY nickname COLLATE NOCASE,id").fetchall()


def get_player(db, user_id):
    with closing(sqlite3.connect(db)) as connection:
        connection.execute("BEGIN")
        row = connection.execute(
            "SELECT card_id,last_login FROM users WHERE id=?", (user_id,)).fetchone()
        if not row:
            raise KeyError(user_id)
        version, profile = _latest(connection, user_id)
        played = connection.execute("SELECT count(*) FROM scores WHERE user_id=?", (user_id,)).fetchone()[0]
        best_count = connection.execute(
            "SELECT count(*) FROM best_records WHERE user_id=?", (user_id,)).fetchone()[0]
        earned_titles = connection.execute(
            "SELECT title_id,awarded FROM title_rewards WHERE user_id=? ORDER BY awarded,title_id",
            (user_id,)).fetchall()
        return {"id": user_id, "card_id": row[0], "last_login": row[1], "version": version,
                "profile": profile, "fields": profile_fields(profile), "played_results": played,
                "best_count": best_count, "earned_titles": earned_titles}


class StaleProfile(ValueError):
    pass


def update_player(db, user_id, values, expected_version=None):
    """Apply a validated partial edit to the latest bytes; never change identity."""
    unknown = set(values) - {"nickname"} - SETTING_BY_KEY.keys()
    if unknown:
        raise ValueError("unknown customization field: " + sorted(unknown)[0])
    parsed = {}
    for key, value in values.items():
        if key == "nickname":
            parsed[key] = nickname_bytes(value)
            continue
        field = SETTING_BY_KEY[key]
        if value == "keep":
            continue
        try:
            number = int(value)
        except (ValueError, TypeError) as error:
            raise ValueError(f"invalid {field.label}") from error
        if key == "auto_velocity":
            valid = 300 <= number <= 999
        else:
            valid = number in dict(field.choices)
        if not valid:
            raise ValueError(f"invalid {field.label}")
        parsed[key] = number
    with closing(sqlite3.connect(db)) as connection, connection:
        connection.execute("PRAGMA foreign_keys=ON")
        connection.execute("BEGIN IMMEDIATE")
        version, old = _latest(connection, user_id)
        if expected_version is not None and version != expected_version:
            raise StaleProfile("player changed since this form was opened; reload before saving")
        profile = bytearray(old)
        for key, value in parsed.items():
            if key == "nickname":
                # Preserve padding beyond the visible twelve-character nickname.
                profile[0x24:0x31] = value.ljust(13, b"\0")
            else:
                SETTING_BY_KEY[key].write(profile, value)
        if profile != old:
            version = _append(connection, user_id, version, profile)
        return version


def save_prefix(db, user_id, prefix36):
    """The native 0x13200e upload carries only R[0:36], not cosmetics."""
    if len(prefix36) != 36:
        raise ValueError("profile prefix must be 36 bytes")
    with closing(sqlite3.connect(db)) as connection, connection:
        connection.execute("PRAGMA foreign_keys=ON")
        connection.execute("BEGIN IMMEDIATE")
        version, old = _latest(connection, user_id)
        profile = bytes(prefix36) + old[36:]
        if profile != old:
            _append(connection, user_id, version, profile)


def save_result_options(db, user_id, result108):
    """Persist options from the mapped played-result structure, keeping all else.

    Builder 0x080854e6 copies player+0x203cc.. options. Profile writes at
    0x080bb70b and 0x080e0e70 establish the matching R offsets. The signed
    result word at +0x4c is stored as its original low 16 bits at R+0x58.
    """
    if len(result108) != 108:
        raise ValueError("played result must be 108 bytes")
    if struct.unpack_from("<I", result108, 0x1c)[0] != user_id:
        raise ValueError("played result does not belong to this player")
    # Buffered uploads describe a previous play, not the player's current
    # preferences. Keep their scores/history without undoing newer web edits.
    if result108[0x22] != 0:
        return
    with closing(sqlite3.connect(db)) as connection, connection:
        connection.execute("PRAGMA foreign_keys=ON")
        connection.execute("BEGIN IMMEDIATE")
        version, old = _latest(connection, user_id)
        profile = bytearray(old)
        profile[0x4c:0x58] = result108[0x40:0x4c]
        profile[0x58:0x5a] = result108[0x4c:0x4e]
        profile[0x5c:0x60] = result108[0x50:0x54]
        if profile != old:
            _append(connection, user_id, version, profile)
