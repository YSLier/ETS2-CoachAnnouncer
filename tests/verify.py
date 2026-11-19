"""Load the probe in a simulated SDK host and validate the resulting JSON Lines."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def load_logs(paths, pid):
    return [
        [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
        for path in sorted(paths)
        if path.stem.split("-")[2] == pid
    ]


def verify(sessions):
    assert len(sessions) == 4, f"Expected four sessions, got {len(sessions)}"
    assert all(s[0]["sdk_headers"] == "1.16" and s[0]["telemetry_api"] == [1, 1]
               for s in sessions)
    failed, trips, limits, burst = sessions
    assert [r["result"] for r in failed[1:]] == [0, -6]
    assert any(r.get("id") == "game.time" and r.get("result") == 0 for r in trips)
    assert any(r.get("id") == "game.time" and r.get("result") == -4 for r in limits)
    assert any(r.get("id") == "truck.world.placement" and r.get("result") == 0 for r in trips)
    assert any(r.get("id") == "local.scale" and r.get("result") == 0 for r in trips)
    if os.name != "nt":
        assert next(r for r in trips if r["kind"] == "announcer")["station_map_points"] == 1
    assert [r for r in trips if r["kind"] == "station_location"] == [
        {"kind": "station_location", "station_id": "ingolstadt", "status": "learned"}
    ]
    assert all("shutdown" == s[-1]["kind"] for s in (trips, limits, burst))
    assert trips[-1]["dropped_records"] == limits[-1]["dropped_records"] == 0
    assert all(r.get("id") != "truck" for r in trips)
    nav = next(r for r in trips if r["kind"] == "navigation")
    assert {a["name"]: a["value"] for a in nav["attributes"]} == {
        "speed_mps": 12, "distance_m": 2100, "eta_s": 120, "map_scale": 19}

    company = [r for r in trips if r.get("bus", {}).get("market") == "company_job"]
    assert [r["bus"]["stage_current"] for r in company] == [0, 0, 1]
    assert [r["bus"]["duplicate"] for r in company] == [False, True, False]
    assert company[0]["game_minutes_last_seen"] == 100
    origin, final = company[0]["bus"]["stages"]
    assert origin["source"]["name"] == "Nürnberg"
    assert origin["source"]["entering_passengers.planned"] == 12
    assert origin["destination"]["arrival_time.planned"] == 0
    assert "arrival_time.actual" not in origin["destination"]
    assert final["destination"]["name"] == 'München "ZOB"'
    assert final["destination"]["future_vector"] == [1.25, 0.0, -3.5]
    assert final["destination"]["leaving_passengers.actual"] == 8
    future = {a["name"]: a for a in company[0]["attributes"]}
    assert future["future.value"] == {"name": "future.value", "index": 470, "type": 81,
                                      "value": None}
    assert future["future.non_finite"]["value"] == "Infinity"
    assert company[1]["seq"] - company[0]["seq"] == 2  # Empty truck job between updates.
    assert len({r["seq"] for r in company}) == 3

    tourist = [r for r in trips if r.get("bus", {}).get("market") == "tourist_job"]
    assert [r["bus"]["stage_current"] for r in tourist] == [0, 1, 1, 1]
    assert tourist[1]["bus"]["stages"][0]["destination"]["type"] == "viewpoints"
    assert "arrival_time.actual" not in tourist[1]["bus"]["stages"][1]["destination"]
    assert tourist[2]["bus"]["stages"][1]["destination"]["arrival_time.actual"] == 300
    first_completed, second_completed, cancelled = [r for r in trips if "bus_result" in r]
    assert [e["bus_result"] for e in (first_completed, second_completed, cancelled)] == [
        "completed", "completed", "cancelled"]
    assert first_completed["attributes"][0]["value"] == 9007199254741001
    assert cancelled["attributes"][0]["value"] == -500
    assert next(r for r in trips if r.get("seq") == first_completed["seq"] - 1)["bus"]["active"] is False
    assert first_completed["paused"] is True
    assert any(r.get("id") == "bus_job.future_event" and "bus_result" not in r for r in trips)
    assert any(r.get("id") == "job" and len(r["attributes"]) == 1 for r in trips)
    invalid = next(r for r in trips if r.get("attributes_missing"))
    assert invalid["bus"]["active"] is None

    sample = next(r for r in limits if r.get("bus", {}).get("stage_limit_exceeded"))
    assert sample["bus"]["stage_count"] == 9999999
    assert sample["bus"]["stage_current"] is None
    assert sample["bus"]["skipped_stop_fields"] == sample["bus"]["unindexed_stop_fields"] == 1
    assert [stage["index"] for stage in sample["bus"]["stages"]] == [0, 127]
    assert sample["bus"]["stages"][1]["source"]["name"].endswith("�")
    assert any(a["index"] == 128 for a in sample["attributes"])
    assert sample["text_limit"] is True
    assert sample["game_minutes_last_seen"] is None
    malformed = next(r for r in limits if r.get("bus", {}).get("current_in_range") is False)
    assert malformed["bus"]["stages"][0]["in_declared_range"] is False
    resets = [r for r in limits if r.get("bus", {}).get("market") == "tourist_job"]
    assert len(resets) == 2 and all(not r["bus"]["duplicate"] for r in resets)
    capped = next(r for r in limits if r.get("attribute_limit"))
    assert len(capped["attributes"]) == 4096
    assert any(sum(a.get("truncated", False) for a in r.get("attributes", [])) >= 15
               for r in limits)
    assert burst[-1]["last_attempted_seq"] == 5000
    assert burst[-1]["dropped_records"] > 0  # Large synchronous burst cannot block the producer.
    records = [r for r in burst if "seq" in r]
    assert len(records) + burst[-1]["dropped_records"] == 5000
    assert burst[-1]["last_written_seq"] == records[-1]["seq"]
    assert len(records) >= 16
    assert sorted({r["seq"] for r in records}) == [r["seq"] for r in records]
    print("PASS: SDK negotiation, callback registration and rollback, simulated company/tourist "
          "routes, cancellation/completion, unknown fields, reinit, truncation, and queue overflow.")


def main():
    host, dll = (Path(arg).resolve() for arg in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="coach-sdk-test-") as tmp:
        env = os.environ.copy()
        if os.name == "nt":
            directory = Path(os.environ["LOCALAPPDATA"]) / "CoachAnnouncer" / "logs"
        else:
            directory = Path(tmp) / "logs"
            env["COACH_PROBE_TEST_DIR"] = str(directory)
            (directory.parent / "station-map-ets2-1.62.tsv").write_text(
                "# coach-stations-v1 ets2-1.62\nline\tduisburg\t-13047\t-6172\n", encoding="utf-8"
            )
        command = [str(host), str(dll)]
        old_files = set(directory.glob("CoachAnnouncer-*.jsonl"))
        run = subprocess.run(command, env=env, capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=55)
        print(run.stdout, end="")
        if run.returncode:
            raise RuntimeError(run.stderr)
        pid = next(line[4:] for line in run.stdout.splitlines() if line.startswith("PID="))
        new_files = set(directory.glob("CoachAnnouncer-*.jsonl")) - old_files
        verify(load_logs(new_files, pid))


if __name__ == "__main__":
    main()
