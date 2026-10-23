#!/usr/bin/env python3
"""Независимое восстановление событий текстового журнала."""
import collections
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1)).resolve()


def read_events(text):
    events = []
    for line in text.splitlines():
        match = re.fullmatch(r"\[t=(\d+)\] ([^:]+): (.*)", line)
        assert match, line
        fields = dict(item.split("=", 1) for item in match[3].split() if "=" in item)
        fields = {k: int(v) if v.isdecimal() else v for k, v in fields.items()}
        events.append((int(match[1]), match[2], fields))
    return events


def verify(events):
    run = events[0][2]
    streams, phases, queues, active, arrivals = {}, {}, {}, {}, {}
    green, permits, refused, completed = set(), set(), set(), set()
    waits, peaks = collections.defaultdict(list), collections.defaultdict(int)
    opened, clearance, closed, previous_tick = 0, None, False, 0
    for tick, event, data in events:
        assert tick >= previous_tick
        previous_tick = tick
        name, ident = data.get("поток"), data.get("id")
        if event == "Поток":
            name = data["имя"]
            streams[name], queues[name] = data, collections.deque()
        elif event == "Фаза":
            phases[data["имя"]] = data["маска"]
        elif event == "Начало фазы":
            assert not active
            if clearance is not None:
                assert tick - clearance >= run["интервал"]
            opened, closed = tick, False
            mask = phases[data["имя"]]
            for i, stream in enumerate(streams.values()):
                if mask & (1 << i):
                    assert not stream["конфликты"] & mask & ~(1 << i)
        elif event == "Сигнал":
            if data["зеленый"]:
                green.add(name)
            else:
                assert name not in active
                green.discard(name)
        elif event == "Въезд закрыт":
            assert tick - opened >= run["минимум"]
            closed = True
        elif event == "Конец фазы":
            assert not active and run["минимум"] <= tick - opened <= run["максимум"]
            assert data["длительность"] == tick - opened
        elif event == "Интервал":
            assert not active and not green
            clearance = tick
        elif event == "Прибытие":
            assert ident not in arrivals
            assert not run["период"] or tick < run["период"]
            arrivals[ident] = (name, tick)
        elif event == "Очередь":
            assert arrivals[ident][0] == name
            assert all(ident not in q for q in queues.values())
            queues[name].append(ident)
            assert len(queues[name]) == data["длина"] <= streams[name]["емкость"]
            peaks[name] = max(peaks[name], len(queues[name]))
        elif event == "Отказ":
            assert len(queues[name]) == streams[name]["емкость"]
            refused.add(ident)
        elif event == "Запрос пешехода":
            assert streams[name]["тип"] == "pedestrian" and ident in queues[name]
        elif event == "Разрешение":
            assert not closed and name in green and queues[name][0] == ident
            permits.add(ident)
        elif event == "Въезд":
            assert ident in permits and name in green and name not in active and not closed
            permits.remove(ident)
            assert queues[name].popleft() == ident and data["длина"] == len(queues[name])
            assert tick + streams[name]["пересечение"] <= opened + run["максимум"]
            names = list(streams)
            for other in active:
                assert not streams[name]["конфликты"] & (1 << names.index(other))
            active[name] = (ident, tick)
        elif event == "Выход":
            assert name in green and active[name][0] == ident and ident not in completed
            _, entered = active.pop(name)
            assert tick == entered + streams[name]["пересечение"]
            wait = entered - arrivals[ident][1]
            assert data["ожидание"] == wait
            waits[name].append(wait)
            completed.add(ident)
        elif event == "Состояние":
            assert data["ожидают"] == sum(map(len, queues.values()))
            assert data["пересекают"] == len(active)
        elif event == "Итог потока":
            name = data["имя"]
            assert data["прибыло"] == sum(n == name for n, _ in arrivals.values())
            assert data["прошло"] == len(waits[name])
            assert data["ожидают"] == len(queues[name]) and data["в_зоне"] == int(name in active)
            assert data["отказов"] == sum(arrivals[i][0] == name for i in refused)
            assert data["пик"] == peaks[name]
            assert data["ожидание"] == sum(waits[name])
            assert data["максимум"] == max(waits[name], default=0)
        elif event == "ИТОГО":
            assert data["прибыло"] == len(arrivals) and data["прошло"] == len(completed)
            assert data["отказов"] == len(refused) and data["в_зоне"] == len(active)
            assert data["ожидают"] == sum(map(len, queues.values()))
            all_waits = [w for values in waits.values() for w in values]
            assert data["ожидание"] == sum(all_waits) and data["максимум"] == max(all_waits, default=0)
            mean = sum(all_waits) / len(all_waits) if all_waits else 0
            assert abs(float(data["среднее"]) - mean) <= 0.00501
            assert data["прибыло"] == data["прошло"] + data["отказов"] + data["ожидают"] + data["в_зоне"]
    assert not permits and events[-1][1] == "ИТОГО"
    return events[-1][2]


class Scenarios(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.directory = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_model(self, scenario="demo", extra="", expected=0, text=None):
        source = (ROOT / "data" / (scenario + ".conf")).read_text() if text is None else text
        config, journal = self.directory / "input.conf", self.directory / "output.log"
        config.write_text(source + "\n" + extra)
        result = subprocess.run([str(BINARY), str(config), str(journal)], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, expected, result.stderr)
        if expected not in (0, 3):
            return result
        self.assertEqual(result.stdout, journal.read_text())
        events = read_events(result.stdout)
        return verify(events), events, result.stdout

    def test_hand_calculation_and_overflow(self):
        summary, events, _ = self.run_model("hand")
        self.assertEqual([summary[k] for k in ("прибыло", "прошло", "отказов", "ожидают", "в_зоне")],
                         [16, 3, 10, 2, 1])
        self.assertEqual((summary["ожидание"], summary["среднее"], summary["максимум"]), (6, "2.00", 5))
        self.assertEqual([(t, d["id"]) for t, e, d in events if e == "Выход"], [(2, 1), (4, 3), (7, 2)])

    def test_both_strategies_and_seeds(self):
        for strategy in ("fixed", "adaptive"):
            for seed in range(6):
                self.run_model(extra=f"strategy {strategy}\nseed {seed}\nticks 80")

    def test_reproducibility(self):
        self.assertEqual(self.run_model()[2], self.run_model()[2])
        self.assertNotEqual(self.run_model()[2], self.run_model(extra="seed 40")[2])

    def test_target_and_empty_data(self):
        summary, events, _ = self.run_model(extra="ticks 0\ntarget 30")
        self.assertEqual(summary["прошло"], 30)
        self.assertEqual(summary["в_зоне"], 0)
        self.assertEqual(next(d["причина"] for _, e, d in events if e == "Завершение"), "цель")
        empty = "stream A car north straight 1 1 0\nphase a A\n"
        self.assertEqual(self.run_model(text=empty)[0]["прибыло"], 0)
        self.run_model(text=empty, extra="ticks 0\ntarget 1", expected=3)

    def test_safe_priority_and_adaptive_response(self):
        def first_b(events):
            return next(t for t, e, d in events if e == "Въезд" and d["поток"] == "B")
        _, priority, _ = self.run_model("emergency")
        _, normal, _ = self.run_model("emergency", extra="priority 0")
        _, adaptive, _ = self.run_model("emergency", extra="priority 0\nstrategy adaptive")
        self.assertEqual(first_b(priority), 3)
        self.assertEqual(first_b(normal), 10)
        self.assertLess(first_b(adaptive), first_b(normal))

    def test_compatible_streams(self):
        _, events, _ = self.run_model("compatible")
        self.assertEqual([d["поток"] for t, e, d in events if e == "Въезд" and t == 0], ["N", "S"])

    def test_invalid_input_and_cli(self):
        prefix = "stream A car north straight 2 2 0\nphase a A\n"
        for text in ("", "unknown 1", prefix + "min_green 5\nmax_green 4",
                     prefix + "clearance 0", prefix + "seed -1", prefix + "ticks 1e2",
                     prefix + "stream A car east left 1 1 0", prefix + "phase b missing",
                     "stream X car north walk 1 1 0\nphase x X", prefix + "conflict A A 0",
                     "stream A car north straight 65 1 0\nphase a A",
                     "stream A car north straight 1 1 0\nstream B car east left 1 1 0\nphase unsafe A B"):
            with self.subTest(text=text):
                self.run_model(text=text, expected=2)
        for args, expected in (([], 2), (["--help"], 0), (["/no-such-config"], 2), (["a", "b", "c"], 2)):
            result = subprocess.run([str(BINARY)] + args, capture_output=True, timeout=3)
            self.assertEqual(result.returncode, expected)
        self.run_model(text=prefix.replace("\n", "\r\n").rstrip())

    def test_file_boundaries_and_input_protection(self):
        names = [f"S{i:02d}_" + "x" * 27 for i in range(16)]
        lines = ["ticks 65"] + [f"stream {name} car north straight 64 1 1000" for name in names]
        lines += [f"phase P{i} {name}" for i, name in enumerate(names)]
        self.run_model(text="\n".join(lines))
        config = self.directory / "input.conf"
        for contents in (b"#" * 16385, b"stream A\0"):
            config.write_bytes(contents)
            result = subprocess.run([str(BINARY), str(config)], capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 2)
        config.write_text((ROOT / "data/demo.conf").read_text())
        alias = self.directory / "alias.conf"
        os.link(config, alias)
        original = config.read_bytes()
        result = subprocess.run([str(BINARY), str(config), str(alias)], capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(config.read_bytes(), original)

    def test_signal_completion(self):
        self.run_model(extra="ticks 2")
        for sig in (signal.SIGINT, signal.SIGTERM):
            config, journal = self.directory / "input.conf", self.directory / "signal.log"
            journal.unlink(missing_ok=True)
            config.write_text((ROOT / "data/demo.conf").read_text() + "\nticks 0\ndelay_ms 20\n")
            with subprocess.Popen([str(BINARY), str(config), str(journal)], stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE, text=True) as process:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if journal.exists() and "Состояние:".encode() in journal.read_bytes():
                        break
                    time.sleep(0.01)
                else:
                    process.kill()
                    self.fail("Модель не начала выполнение")
                process.send_signal(sig)
                _, stderr = process.communicate(timeout=3)
                self.assertEqual(process.returncode, 128 + sig, stderr)
                events = read_events(journal.read_text())
                verify(events)
                self.assertEqual(next(d["причина"] for _, e, d in events if e == "Завершение"), "прерывание")

    def test_closed_stdout_preserves_log(self):
        journal = self.directory / "closed.log"
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            result = subprocess.run([str(BINARY), str(ROOT / "data/demo.conf"), str(journal)],
                                    stdout=write_fd, stderr=subprocess.PIPE, timeout=3)
        finally:
            os.close(write_fd)
        self.assertEqual(result.returncode, 1)
        events = read_events(journal.read_text())
        verify(events)
        self.assertEqual(next(d["причина"] for _, e, d in events if e == "Завершение"), "ошибка_вывода")

    @unittest.skipUnless(Path("/dev/full").exists(), "/dev/full доступен в Linux")
    def test_log_failure_preserves_console(self):
        result = subprocess.run([str(BINARY), str(ROOT / "data/demo.conf"), "/dev/full"],
                                capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 1)
        verify(read_events(result.stdout))


if __name__ == "__main__":
    unittest.main(verbosity=2)
