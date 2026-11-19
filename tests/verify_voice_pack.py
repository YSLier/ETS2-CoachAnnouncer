"""Verify packaged city coverage, audio dimensions, levels and duration bounds."""
import csv
import json
from array import array
from pathlib import Path
import sys
import wave


def read_clip(path, max_seconds=45, silence_ok=False):
    with wave.open(str(path), "rb") as recording:
        assert (recording.getnchannels(), recording.getsampwidth(),
                recording.getframerate(), recording.getcomptype()) == (1, 2, 22050, "NONE"), path
        count = recording.getnframes()
        frames = recording.readframes(count)
    assert len(frames) == 2 * count and 0 < count <= max_seconds * 22050, path
    samples = array("h", frames)
    if sys.byteorder != "little":
        samples.byteswap()
    peak = max(map(abs, samples))
    assert (0 if silence_ok else 1000) <= peak < 31000, path
    return count


def main(root):
    languages = ("de", "en", "zh", "ja")
    actions = ("welcome", "next", "approaching", "finish")
    names = json.loads((root / "voice/stations.json").read_text(encoding="utf-8"))
    with (root / "voice/city-coverage.csv").open(encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))
    assert len(rows) == len(names) == 398 and {r["sdk_id"] for r in rows} == set(names)
    assert sum(r["scope"] == "map" for r in rows) == 388
    assert sum(r["map_country"] == "iceland" for r in rows) == 14
    assert all(all(r[key] == names[r["sdk_id"]][key] for key in languages) for r in rows)

    chime = read_clip(root / "audio/chime.wav")
    times = {action: 0 for action in actions}
    for language in languages:
        directory = root / "audio" / language
        found = {p.stem: p for p in (directory / "stations").glob("*.wav")}
        assert set(found) == set(names), (language, sorted(set(names) - set(found)))
        lengths = [read_clip(path, max_seconds=15) for path in found.values()]
        for action in actions:
            before = read_clip(directory / "parts" / (action + ".before.wav"))
            parts = directory / "parts"
            tail = parts / (action + ".after.wav")
            after = read_clip(tail, silence_ok=action == "next") if tail.is_file() else 0
            repeated = parts / (action + ".between.wav")
            between = read_clip(repeated) + max(lengths) + 4410 if repeated.is_file() else 0
            generic = read_clip(directory / (action + ".wav"))
            longest = max(max(lengths) + before + after + between + (4410 if after else 2205), generic)
            assert longest <= 45 * 22050, (language, action, longest)
            times[action] += longest
    times = {action: round((n + chime + 3 * 9923) / 22050, 2) for action, n in times.items()}
    print(f"{len(names)} IDs × {len(languages)} languages = {len(names) * len(languages)} WAVs")
    print("Upper bounds for full four-language broadcasts (seconds):", times)


if __name__ == "__main__":
    main(Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent)
