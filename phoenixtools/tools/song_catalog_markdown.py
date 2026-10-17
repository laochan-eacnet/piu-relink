"""Markdown rendering for dump_songlist.py."""

import datetime as dt
from pathlib import Path

from song_catalog_common import markdown_escape


def write_catalog(path: Path, songs: list[dict[str, object]], list_path: Path,
                  language: int, song_count: int, chart_count: int) -> None:
    local_count = sum(bool(song["local"]) for song in songs)
    missing_count = len(songs) - local_count
    chart_total = sum(len(song["charts"]) for song in songs)  # type: ignore[arg-type]
    chart_local = sum(
        bool(chart["file_present"])
        for song in songs for chart in song["charts"]  # type: ignore[union-attr]
    )
    lines = [
        "# Pump It Up Phoenix 2024 Song Catalog", "",
        f"- Generated: {dt.datetime.now().astimezone().isoformat(timespec='seconds')}",
        f"- Source: `{markdown_escape(str(list_path))}`",
        f"- Language table: `LIST.{language}`",
        f"- Catalog records: {song_count} songs, {chart_count} charts",
        f"- Exported: {len(songs)} ({local_count} local, {missing_count} missing/remote)",
        f"- Unique charts: {chart_total} ({chart_local} local NX files)", "",
        "| ID | Title | Artist | BPM | Local media | Charts |",
        "|---:|---|---|---:|---|---|",
    ]
    for song in songs:
        media = "missing/remote"
        if song["local"]:
            parts = [name for name, key in (
                ("A", "full_audio"), ("V", "full_video"),
                ("P/A", "preview_audio"), ("P/V", "preview_video"),
            ) if song[key]]
            media = ", ".join(parts) if parts else "directory only"
            if song["uses_base_media"]:
                media += " (base-ID fallback)"
        charts = []
        for chart in song["charts"]:  # type: ignore[union-attr]
            missing = "" if chart["file_present"] else " missing"
            charts.append(f"{chart['label']} (`{chart['name']}`){missing}")
        chart_text = ", ".join(charts) if charts else "—"
        lines.append(
            f"| `{int(song['id']):X}` | {markdown_escape(str(song['title']))} | "
            f"{markdown_escape(str(song['artist']))} | "
            f"{markdown_escape(str(song['bpm']))} | {markdown_escape(media)} | "
            f"{markdown_escape(chart_text)} |"
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {path}")
    print(f"songs={len(songs)} local={local_count} missing={missing_count}")
    print(f"charts={chart_total} local_nx={chart_local}")
