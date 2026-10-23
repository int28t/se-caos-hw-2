"""Восстановление модели и проверка правил по журналу событий."""
import collections
import re


def read_events(text):
    events = []
    for line in text.splitlines():
        match = re.fullmatch(r"\[t=(\d+)\] ([^:]+): (.*)", line)
        assert match, line
        fields = dict(item.split("=", 1) for item in match[3].split() if "=" in item)
        fields = {k: int(v) if v.isdecimal() else v for k, v in fields.items()}
        events.append((int(match[1]), match[2], fields))
    return events


def random_inputs(seed, stream, tick):
    mask = (1 << 64) - 1
    initial = (seed << 32) | stream
    values = []
    for offset in (1, 2):
        value = (initial + (2 * tick + offset) * 0x9E3779B97F4A7C15) & mask
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & mask
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & mask
        values.append((value ^ (value >> 31)) % 1000)
    return values


def verify(events):
    run = events[0][2]
    streams, phases, queues, active, arrivals = {}, {}, {}, {}, {}
    green, permits, refused, completed = set(), set(), set(), set()
    waits, peaks = collections.defaultdict(list), collections.defaultdict(int)
    arrival_ticks = {}
    opened, clearance, closed, previous_time = 0, None, False, 0
    unit = run["такт_мс"] * 1_000_000
    snapshot = None
    for number, (tick, event, data) in enumerate(events, 1):
        now = data["время_нс"]
        assert data["событие"] == number and now >= previous_time
        assert tick == now // unit
        previous_time = now
        name, ident = data.get("поток"), data.get("id")
        if event == "Поток":
            name = data["имя"]
            streams[name], queues[name] = data, collections.deque()
        elif event == "Фаза":
            phases[data["имя"]] = data["маска"]
        elif event == "Начало фазы":
            assert not active
            if clearance is not None:
                assert now - clearance >= run["интервал"] * unit
            opened, closed = now, False
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
            assert data["пересекают"] == len(active)
            assert now - opened >= run["минимум"] * unit
            closed = True
        elif event == "Конец фазы":
            assert not active and now - opened >= run["минимум"] * unit
            assert data["длительность"] == (now - opened) // unit
        elif event == "Интервал":
            assert not active and not green
            clearance = now
        elif event == "Прибытие":
            assert ident == len(arrivals) + 1
            assert not run["период"] or now < run["период"] * unit
            assert tick > arrival_ticks.get(name, -1)
            arrival_ticks[name] = tick
            stream = streams[name]
            arrival, emergency = random_inputs(run["seed"], list(streams).index(name), tick)
            assert arrival < stream["интенсивность"]
            assert data["экстренный"] == (stream["тип"] == "car" and emergency < run["экстренные"])
            arrivals[ident] = (name, now)
        elif event == "Очередь":
            assert arrivals[ident][0] == name
            assert all(ident not in q for q in queues.values())
            queues[name].append(ident)
            assert len(queues[name]) == data["длина"] <= streams[name]["емкость"]
            peaks[name] = max(peaks[name], len(queues[name]))
        elif event == "Отказ":
            assert len(queues[name]) == streams[name]["емкость"] or data.get("причина") == "ошибка_потока"
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
            assert now < opened + run["максимум"] * unit
            assert not run["период"] or now < run["период"] * unit
            names = list(streams)
            for other in active:
                assert not streams[name]["конфликты"] & (1 << names.index(other))
            active[name] = (ident, now)
        elif event == "Выход":
            assert name in green and active[name][0] == ident and ident not in completed
            _, entered = active.pop(name)
            assert now >= entered + streams[name]["пересечение"] * unit
            assert not run["период"] or now <= run["период"] * unit
            wait = (entered - arrivals[ident][1]) // unit
            assert data["ожидание"] == wait
            waits[name].append(wait)
            completed.add(ident)
        elif event == "Состояние":
            if snapshot is not None:
                if run["задержка_мс"]:
                    assert now - snapshot >= run["задержка_мс"] * 1_000_000
                else:
                    assert now // unit > snapshot // unit
            snapshot = now
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
            assert data["потоков"] == data["прибыло"] - data["отказов"]
        assert not run["цель"] or len(completed) + len(active) <= run["цель"]
    assert not permits and events[-1][1] == "ИТОГО"
    return events[-1][2]


