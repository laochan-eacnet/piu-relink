"""Small server-rendered LAN player editor; no external packages or JavaScript."""
import html
import urllib.parse

import playerdata


STYLE = """
body{font:16px system-ui,sans-serif;max-width:1050px;margin:2rem auto;padding:0 1rem;background:#f5f7fa;color:#172435}
a{color:#1459a0}table{border-collapse:collapse;width:100%;background:white}th,td{text-align:left;padding:.65rem;border-bottom:1px solid #d4dce5}
form{background:white;padding:1.2rem;border:1px solid #d4dce5;border-radius:.5rem}.settings{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:1rem}label{display:flex;flex-direction:column;gap:.3rem}input,select,button{font:inherit;padding:.45rem;max-width:100%;box-sizing:border-box}button{margin-top:1.2rem;background:#1459a0;color:white;border:0;border-radius:.3rem;cursor:pointer}.notice{background:#e2f2e7;padding:.7rem}.error{background:#ffe6e6;padding:.7rem}.muted{color:#536477;font-size:.9rem}code{overflow-wrap:anywhere}
"""


def escape(value):
    return html.escape(str(value), quote=True)


def document(title, body):
    return ("<!doctype html><html lang=en><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            f"<title>{escape(title)} · Phoenix LAN</title><style>{STYLE}</style>"
            "<header><a href='/players'>Phoenix LAN players</a></header>"
            f"<main><h1>{escape(title)}</h1>{body}</main>"
            "<footer><p class=muted>LAN development server. Anyone on this LAN can edit these players. "
            "Changes apply on the next card login; finish any active game before editing.</p></footer></html>")


def send(handler, status, body, location=None):
    data = body.encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "text/html; charset=utf-8")
    handler.send_header("Content-Length", str(len(data)))
    handler.send_header("Cache-Control", "no-store")
    if location:
        handler.send_header("Location", location)
    handler.end_headers()
    handler.wfile.write(data)


def player_list(db):
    rows = []
    for user_id, card_id, nickname, last_login in playerdata.list_players(db):
        rows.append(f"<tr><td><a href='/players/{user_id}'>{escape(nickname)}</a></td>"
                    f"<td><code>{escape(card_id)}</code></td><td>{escape(last_login)}</td></tr>")
    if not rows:
        return "<p>No registered players yet. Scan a card in the game once, then reload this page.</p>"
    return ("<p>Select a registered player to view or change customization.</p>"
            "<table><thead><tr><th>Nickname</th><th>Card</th><th>Last game login (UTC)</th></tr></thead>"
            f"<tbody>{''.join(rows)}</tbody></table>")


def field_control(field, current):
    key = escape(field.key)
    label = escape(field.label)
    if field.key == "auto_velocity":
        if 300 <= current <= 999:
            control = f"<input name='{key}' type=number min=300 max=999 step=1 required value='{current}'>"
        else:
            control = (f"<select name='{key}'><option value=keep>Keep existing native value "
                       f"({escape(current)})</option>"
                       + ''.join(f"<option value='{i}'>{i}</option>" for i in range(300, 1000))
                       + "</select>")
    else:
        choices = dict(field.choices)
        options = []
        if current not in choices:
            options.append(f"<option value=keep selected>Keep existing native value ({escape(current)})</option>")
        for value, text in field.choices:
            selected = " selected" if current == value else ""
            options.append(f"<option value='{value}'{selected}>{escape(text)}</option>")
        control = f"<select name='{key}'>{''.join(options)}</select>"
    return f"<label>{label}{control}</label>"


def progression_summary(player):
    rows = []
    for title_id, awarded in player["earned_titles"]:
        name = (playerdata.TITLE_LABELS[title_id] if 0 <= title_id < len(playerdata.TITLE_LABELS)
                else "Unmapped native title")
        rows.append(f"<li>#{title_id}: {escape(name)} — {escape(awarded)}</li>")
    rewards = ("<ul>" + ''.join(rows) + "</ul>" if rows
               else "<p>No local event titles earned yet.</p>")
    return (f"<p>Uploaded played results: <strong>{player['played_results']}</strong> · "
            f"Charts with native best records: <strong>{player['best_count']}</strong></p>"
            f"<details><summary>Earned local event titles ({len(rows)})</summary>{rewards}</details>"
            "<p class=muted>Event rewards are local LAN policy. The editor also permits choosing "
            "any title actually supported by this build.</p>")


def player_form(player, saved=False):
    fields = player["fields"]
    title = f"{fields['nickname']} #{fields['discriminator']:04d}"
    notice = "<p class=notice>Saved. Log in with this card again to apply these settings.</p>" if saved else ""
    body = (f"{notice}<p>Card <code>{escape(player['card_id'])}</code> · "
            f"Player ID {player['id']} · Profile revision {player['version']}</p>"
            + progression_summary(player)
            + f"<form method=post action='/players/{player['id']}'>"
            f"<input type=hidden name=version value='{player['version']}'>"
            "<div class=settings><label>Nickname (1-12 printable ASCII characters)"
            f"<input name=nickname minlength=1 maxlength=12 required value='{escape(fields['nickname'])}'></label>"
            + ''.join(field_control(field, fields[field.key]) for field in playerdata.SETTINGS)
            + "</div><button type=submit>Save customization</button></form>"
            "<p class=muted>Title IDs and noteskins are from this build's own tables. "
            "Legacy options remain readable by the game but are hidden in its normal command menu. "
            "Some path effects are shared with the other player. The game clears Very Hard judgement "
            "at login, so it is not offered. Auto velocity is used when scroll speed is Auto velocity. "
            "Timing adjustment labels are the native -5..+5 command levels, not milliseconds. "
            "Session-only rush, sort order and stage-pass controls are not stored in this profile. "
            "Unmapped bytes and player identity are preserved.</p>")
    return title, body


def read_form(handler):
    if handler.headers.get("Content-Type", "").split(";", 1)[0].strip().lower() != "application/x-www-form-urlencoded":
        raise ValueError("submit a URL-encoded customization form")
    if handler.headers.get("Transfer-Encoding"):
        raise ValueError("chunked forms are not supported")
    try:
        length = int(handler.headers.get("Content-Length", ""))
    except ValueError as error:
        raise ValueError("form Content-Length is required") from error
    if not 0 < length <= 16384:
        raise ValueError("customization form is empty or too large")
    raw = handler.rfile.read(length)
    if len(raw) != length:
        raise ValueError("incomplete customization form")
    try:
        pairs = urllib.parse.parse_qsl(raw.decode("utf-8"), keep_blank_values=True,
                                      strict_parsing=True, errors="strict", max_num_fields=64)
    except UnicodeError as error:
        raise ValueError("form is not valid UTF-8") from error
    values = {}
    for key, value in pairs:
        if key in values:
            raise ValueError("duplicate customization field")
        values[key] = value
    required = {"nickname", "version"} | playerdata.SETTING_BY_KEY.keys()
    if set(values) != required:
        raise ValueError("form has missing or unknown customization fields")
    try:
        version = int(values.pop("version"))
    except ValueError as error:
        raise ValueError("invalid profile revision") from error
    if version < 0:
        raise ValueError("invalid profile revision")
    return version, values


def handle(handler):
    """Return False only for non-web routes, allowing game HTTP dispatch."""
    url = urllib.parse.urlsplit(handler.path)
    path = url.path
    if path not in ("/", "/players", "/players/") and not path.startswith("/players/"):
        return False
    if handler.command not in ("GET", "POST"):
        send(handler, 405, document("Method not allowed", "<p>Use GET or POST.</p>"))
        return True
    try:
        if path in ("/", "/players", "/players/"):
            if handler.command != "GET":
                send(handler, 405, document("Method not allowed", "<p>Select a player before saving.</p>"))
            else:
                send(handler, 200, document("Registered players", player_list(handler.server.db)))
            return True
        text_id = path[len("/players/"):]
        if not text_id.isascii() or not text_id.isdecimal() or len(text_id) > 10:
            raise KeyError(text_id)
        user_id = int(text_id)
        if handler.command == "POST":
            version, values = read_form(handler)
            playerdata.update_player(handler.server.db, user_id, values, expected_version=version)
            send(handler, 303, "", location=f"/players/{user_id}?saved=1")
        else:
            player = playerdata.get_player(handler.server.db, user_id)
            title, body = player_form(player, saved=url.query == "saved=1")
            send(handler, 200, document(title, body))
    except KeyError:
        send(handler, 404, document("Player not found", "<p>Scan a card in the game to register it first.</p>"))
    except ValueError as error:
        status = 409 if isinstance(error, playerdata.StaleProfile) else 400
        send(handler, status, document("Customization not saved",
             f"<p class=error>{escape(error)}</p><p><a href='{escape(path)}'>Reload player settings</a></p>"))
    return True
