"""Markdown summary rendering for the local song audit."""

from collections import Counter
from pathlib import Path


def write_summary(path: Path, report: dict[str, object],
                  songs: list[dict[str, object]],
                  local_songs: list[dict[str, object]], playable: int,
                  classes: Counter[str], media: Counter[str],
                  anomalies: dict[str, list[object]],
                  regions: dict[str, object]) -> None:
    lines = [
        "# Phoenix Local Song Resource Audit", "",
        f"- Generated: {report['generated']}",
        f"- Valid catalog songs: {len(songs)}",
        f"- Locally backed records: {len(local_songs)}",
        f"- Playable local records: {playable}",
        f"- Missing/remote records: {len(songs) - len(local_songs)}",
        f"- Local ordinary songs: {classes['local_ordinary_song']}",
        f"- Local encoded aliases: {classes['local_encoded_alias']}",
        f"- Local special previews: {classes['local_special_preview_record']}",
        f"- Present local NX references: {media['present_chart_refs']}",
        f"- Missing local NX references: {media['missing_chart_refs']}", "",
        "## Local media coverage", "", "| Resource | Songs |", "|---|---:|",
        f"| Full audio `A/.A` | {media['full_audio']} |",
        f"| Preview audio `P/A/.A` | {media['preview_audio']} |",
        f"| Full video `V/.V2` | {media['full_video']} |",
        f"| Preview video `P/V/.V2` | {media['preview_video']} |",
        f"| Preview graphics `P/.G` | {media['preview_graphics']} |",
        f"| At least one local NX | {media['with_local_nx']} |", "",
        "## Anomaly counts", "", "| Check | Count |", "|---|---:|",
    ]
    lines.extend(f"| `{key}` | {len(values)} |"
                 for key, values in anomalies.items())
    lines.extend(["", "## Regional policy references", ""])
    for region, entries in regions.items():
        demo = entries["DEMO_SONG"]
        start = entries["START_SONG"]
        lines.append(
            f"- **{region}**: START {sum(item['local'] for item in start)}/"
            f"{len(start)} local; DEMO {sum(item['local'] for item in demo)}/"
            f"{len(demo)} local"
        )
    lines.extend(["", "Full per-song records are in the JSON report."])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
