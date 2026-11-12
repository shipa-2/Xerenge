#!/usr/bin/env python3
"""A race, from a log written with XERENGE_AI_PACING_TRACE=1, in plain words.

    tools/race_report.py [logs/burnoutN.log]     (the newest log by default)

Who was where relative to the player, second by second; when the AI's speed
cap was lowered or raised (its timetable, src/ai_pacing.cpp); and every
teleport, with how far it was from the player before and after.
"""
import glob
import math
import os
import re
import sys

FIELD = re.compile(r"ai pacing: field at ([\d.]+)(.*)")
ENTRY = re.compile(r"([0-9A-F]{8}) (AI|other) ([\d.]+) mph at \((-?[\d.]+), (-?[\d.]+)\) "
                   r"heading \((-?[\d.e+-]+|-?nan|-?inf), (-?[\d.e+-]+|-?nan|-?inf)\)")
RACER = re.compile(r"ai pacing: racer (\d+) vehicle ([0-9A-F]{8}) clock ([\d.]+) lap (-?\d+) seg (-?\d+) "
                   r"speed (\d+) mph cap (\d+) mph corr ([+-][\d.]+) matching (\d)")
JUMP = re.compile(r"ai pacing: JUMP (.+?) racer \d+ vehicle ([0-9A-F]{8}) moved ([\d.]+) m in "
                  r"(-?[\d.]+) s at (\d+) mph \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\) -> "
                  r"\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\) clock ([\d.]+)")
LIMITS = re.compile(r"ai pacing: vehicle ([0-9A-F]{8}) limits\?(.*)")


def newest_log():
    logs = sorted(glob.glob(os.path.join(os.path.dirname(__file__), "..", "logs", "burnout*.log")),
                  key=os.path.getmtime)
    return logs[-1] if logs else None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else newest_log()
    if not path:
        sys.exit("no log")
    fields, racers, jumps, limits = [], {}, [], {}
    for line in open(path, errors="replace"):
        m = FIELD.search(line)
        if m:
            clock = float(m.group(1))
            cars = {}
            for e in ENTRY.finditer(m.group(2)):
                try:
                    heading = (float(e.group(6)), float(e.group(7)))
                except ValueError:
                    heading = (0.0, 0.0)
                cars[e.group(1)] = dict(ai=e.group(2) == "AI", mph=float(e.group(3)),
                                        x=float(e.group(4)), z=float(e.group(5)), heading=heading)
            fields.append((clock, cars))
            continue
        m = RACER.search(line)
        if m:
            racers.setdefault(m.group(2), []).append(dict(
                clock=float(m.group(3)), lap=int(m.group(4)), seg=int(m.group(5)),
                mph=int(m.group(6)), cap=int(m.group(7)), corr=float(m.group(8)),
                matching=m.group(9) == "1"))
            continue
        m = JUMP.search(line)
        if m and float(m.group(12)) > 0.5 and float(m.group(4)) > 0:
            jumps.append(dict(vehicle=m.group(2), metres=float(m.group(3)), mph=int(m.group(5)),
                              before=(float(m.group(6)), float(m.group(8))),
                              after=(float(m.group(9)), float(m.group(11))),
                              clock=float(m.group(12))))
            continue
        m = LIMITS.search(line)
        if m:
            limits[m.group(1)] = m.group(2).strip()

    # One race only: the last stretch of field lines whose clock kept rising.
    stretches, race = [], []
    for clock, cars in fields:
        if not cars:
            continue
        if race and clock + 5 < race[-1][0]:
            stretches.append(race)
            race = []
        race.append((clock, cars))
    stretches.append(race)
    race = max(stretches, key=len)
    if not race:
        sys.exit(f"{path}: no 'field' lines - was it run with XERENGE_AI_PACING_TRACE=1?")
    start, end = race[0][0], race[-1][0]
    jumps = [j for j in jumps if start - 1 <= j["clock"] <= end + 1]

    # The player: the vehicle that is not the AI's and moved the most.
    travelled = {}
    previous = {}
    for _, cars in race:
        for v, c in cars.items():
            if c["ai"]:
                continue
            if v in previous:
                travelled[v] = travelled.get(v, 0.0) + math.dist(previous[v], (c["x"], c["z"]))
            previous[v] = (c["x"], c["z"])
    player = max(travelled, key=travelled.get) if travelled else None
    if not player:
        # The player's car goes through the AI update for a few steps before the
        # start (the rolling start drives it), so it can be marked as an AI's:
        # the one of those the AI update saw only briefly.
        seen = {v for _, cars in race for v in cars}
        brief = [v for v in seen if len(racers.get(v, [])) < 20]
        player = brief[0] if len(brief) == 1 else None
    ai = sorted({v for _, cars in race for v, c in cars.items() if c["ai"]} - {player})

    name = {v: f"AI {i + 1}" for i, v in enumerate(ai)}

    print(f"Race in {os.path.basename(path)}: {end - start:.0f} s, {len(ai)} AI cars,"
          f" player's car {player or 'not found'}\n")
    if not player:
        print("Could not tell which car is the player's: no non-AI car moved.")
        return

    def gap(cars, v):
        """Metres from the player, + when ahead along the player's direction of travel."""
        p, c = cars.get(player), cars.get(v)
        if not p or not c:
            return None
        dx, dz = c["x"] - p["x"], c["z"] - p["z"]
        hx, hz = p["heading"]
        norm = math.hypot(hx, hz)
        distance = math.hypot(dx, dz)
        if norm < 1.0:
            return distance  # the player standing still: no direction to measure along
        return distance if (dx * hx + dz * hz) >= 0 else -distance

    # Second by second, condensed: every 10 seconds.
    print("Every 10 seconds - each AI car's distance from you (+ ahead, - behind) and speed:")
    print("  time  you      " + "".join(f"{name[v]:>18}" for v in ai))
    next_at = start
    for clock, cars in race:
        if clock < next_at:
            continue
        next_at = clock + 10
        p = cars.get(player)
        row = f"  {clock:4.0f}s {p['mph'] if p else 0:4.0f} mph"
        for v in ai:
            g = gap(cars, v)
            c = cars.get(v)
            row += f"{'?':>18}" if g is None else f"{g:+8.0f} m {c['mph']:4.0f}mph"
        print(row)

    print("\nPer AI car:")
    for v in ai:
        gaps = [g for _, cars in race if (g := gap(cars, v)) is not None]
        ahead = sum(1 for g in gaps if g > 0)
        mph = [cars[v]["mph"] for _, cars in race if v in cars and cars[v]["mph"] < 250]
        r = [s for s in racers.get(v, []) if start - 1 <= s["clock"] <= end + 1]
        held = [s for s in r if s["cap"] < 150 and s["corr"] > 0.5]
        pushed = [s for s in r if s["corr"] < -0.5]
        matching = next((s["clock"] for s in r if s["matching"]), None)
        print(f"  {name[v]} ({v}): ahead of you {100 * ahead / max(len(gaps), 1):.0f}% of the time,"
              f" gap from {min(gaps):+.0f} to {max(gaps):+.0f} m;"
              f" top {max(mph, default=0):.0f} mph, typical {sorted(mph)[len(mph) // 2] if mph else 0:.0f} mph")
        print(f"      timetable: held back {len(held)} s (cap lowered while ahead of schedule),"
              f" pushed {len(pushed)} s (cap raised while behind it)"
              + (f"; speed matching from {matching:.0f} s" if matching is not None else ""))
        if v in limits:
            print(f"      car's limits?{limits[v][:120]}")
    if player in limits:
        print(f"  Your car's limits?{limits[player][:120]}")

    print(f"\nTeleports ({len(jumps)}):")
    for j in jumps:
        cars_before = min(race, key=lambda f: abs(f[0] - (j["clock"] - 0.5)))[1]
        cars_after = min(race, key=lambda f: abs(f[0] - (j["clock"] + 0.5)))[1]
        p0, p1 = cars_before.get(player), cars_after.get(player)
        if p0 and p1:
            d0 = math.dist((p0["x"], p0["z"]), j["before"])
            d1 = math.dist((p1["x"], p1["z"]), j["after"])
            towards = "towards you" if d1 < d0 else "away from you"
            print(f"  {j['clock']:6.1f}s {name.get(j['vehicle'], j['vehicle'])} jumped {j['metres']:.0f} m:"
                  f" {d0:.0f} m from you -> {d1:.0f} m ({towards})")
        else:
            print(f"  {j['clock']:6.1f}s {name.get(j['vehicle'], j['vehicle'])} jumped {j['metres']:.0f} m")


if __name__ == "__main__":
    main()
