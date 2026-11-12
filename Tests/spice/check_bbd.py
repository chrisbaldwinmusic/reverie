#!/usr/bin/env python3
"""Checks Reverie's bucket-brigade model (Source/DSP/BucketBrigade.h) against the same signal path built as a
circuit and solved by ngspice.

    python3 Tests/spice/check_bbd.py            (needs clang++ and ngspice; looks for ngspice on the PATH and
                                                 in ~/Library/Caches/ngspice-*)

The circuit:

    NE570 compressor > 4th-order Butterworth anti-aliasing filter (two unity-gain Sallen-Key stages)
      > chip clipping > sampled at each clock tick, N ticks of delay (with the charge-transfer smear's neighbours),
      held until the next tick > the same Butterworth as reconstruction filter > NE570 expander > output clipping

The filters and the compander are circuits solved in continuous time by ngspice, from an analog signal source;
the chips are sampled at the exact clock ticks, and nothing is sampled at the host rate. So this checks the parts of the
model that could quietly be wrong: the discretisation of the filters, the timing of each clock tick between host
samples, the zero-order hold and its images, aliasing when the clock runs slow, and the compander's envelope
following. The NE570 is its block diagram (a full-wave rectifier into 10 kOhm x 1 uF, driving a gain cell), not a
transistor-level model, and the smear and clipping curves are the model's own: those are design choices, checked
by ear and by Tests/Main.cpp, not by this script.

Errors are given as the level of the difference between the model and the circuit, relative to the circuit's
output. The chips' hiss is random, so the linear cases subtract a run with no input (the path after the hiss is
linear at these levels); with the compander on, the hiss stays in and sets the floor.
"""
import glob, math, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DSP = os.path.join(HERE, "..", "..", "Source", "DSP")
SR = 48000.0
BUCKETS = 4096
REF, FLOOR, RAIL = 0.25, 0.25 / 31.6, 2.0

# test signals, as ngspice expressions in time; BbdModel.cpp computes the same ones
TONES = "0.003*(sin(2*pi*220*time)+sin(2*pi*1700*time)+sin(2*pi*3900*time)+sin(2*pi*6300*time))"
BURSTS = "((time - 0.2*floor(time/0.2)) < 0.1 ? 0.3 : 0.01) * sin(2*pi*1000*time)"


def find_ngspice():
    found = shutil.which("ngspice")
    if found:
        return found
    for path in sorted(glob.glob(os.path.expanduser("~/Library/Caches/ngspice-*/bin/ngspice")), reverse=True):
        return path
    sys.exit("ngspice not found: install it (brew install ngspice) or put it on the PATH")


def chip_settings(compander, character, delay):
    """The values BucketBrigade.h derives; kept in step with it by hand."""
    c = min(max(character, 0.0), 1.0)
    fc = 7000.0 - 4500.0 * c if compander else 9500.0 - 5000.0 * c
    smear = 2.0 * BUCKETS * (0.25e-4 + 1.75e-4 * c) / 8.0
    drive = 1.0 + 2.0 * c
    clock = min(max(BUCKETS * SR / delay, 10000.0), 100000.0)
    tap = delay * clock / SR
    assert abs(tap - round(tap)) < 1e-6, "pick a delay that puts the tap on a whole bucket"
    return fc, smear, drive, clock, int(round(tap))


def sallen_key(name, inp, out, fc):
    """Fourth-order Butterworth low-pass: two unity-gain Sallen-Key stages, equal resistors, ideal op-amps."""
    lines, w, r = [], 2 * math.pi * fc, 10e3
    node = inp
    for k, q in enumerate((0.5411961, 1.3065630)):
        a, b = f"{name}a{k}", f"{name}b{k}"
        o = out if k == 1 else f"{name}o{k}"
        lines += [f"R{name}{k}1 {node} {a} {r}", f"R{name}{k}2 {a} {b} {r}",
                  f"C{name}{k}1 {a} {o} {2 * q / (w * r):.12g}", f"C{name}{k}2 {b} 0 {1 / (2 * q * w * r):.12g}",
                  f"E{name}{k} {o} 0 {b} 0 1"]
        node = o
    return lines


def spice_lines(ngspice, work, name, lines, seconds, node):
    """Solves a netlist and returns the solver's time points and the voltage at one node."""
    out, cir = os.path.join(work, name + ".txt"), os.path.join(work, name + ".cir")
    with open(cir, "w") as f:
        f.write("\n".join([f"* Reverie bucket-brigade check: {name}"] + lines + [
            ".options reltol=1e-6 abstol=1e-12 vntol=1e-9",
            f".tran 1u {seconds} 0 1u uic",
            ".control", "run", f"wrdata {out} v({node})", ".endc", ".end"]) + "\n")
    subprocess.run([ngspice, "-b", cir], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t, v = [], []
    with open(out) as f:
        for line in f:
            a, b = line.split()[:2]
            t.append(float(a)); v.append(float(b))
    return t, v


def interpolate(t, v, times):
    out, j = [], 0
    for ti in times:
        while j + 2 < len(t) and t[j + 1] < ti:
            j += 1
        f = 0.0 if t[j + 1] == t[j] else (ti - t[j]) / (t[j + 1] - t[j])
        out.append(v[j] + min(max(f, 0.0), 1.0) * (v[j + 1] - v[j]))
    return out


def run_spice(ngspice, work, compander, character, delay, seconds, signal):
    """The circuit's output at each host sample instant.

    Solved in two parts around the chips. The chips are an ideal chain of N buckets, so what leaves them at clock
    tick k is exactly what the input side delivered at tick k - N (with the smear's neighbours): ngspice solves the
    input side, its output is read at every tick, and the delayed, held steps drive a second ngspice run through
    the output side. (A transmission line as the delay would do the same in one run, but ngspice's lossless line
    blurs the held steps over its solver points and adds several microseconds of error over a long delay.)"""
    fc, smear, drive, clock, tap = chip_settings(compander, character, delay)
    period = 1.0 / clock
    ticks = int(seconds * clock) + 2
    # The model's clock ticks fall at k / clock less one host sample (its clock's phase, which only moves the
    # images, not the echo); the circuit's clock is given the same phase.
    tick = [k * period - 1.0 / SR for k in range(ticks)]

    first = [f"Bx x 0 V = {signal}"]
    if compander:
        first += [f"By y 0 V = v(x) * {REF} / max(v(ce), {FLOOR})",
                  "Bce 0 ce I = (abs(v(y)) - v(ce)) / 10k", "Cce ce 0 1u", f".ic v(ce)={FLOOR}"]
    else:
        first += ["By y 0 V = v(x)"]
    first += sallen_key("in", "y", "s", fc)
    first += [f"Bq q 0 V = v(s) >= 0 ? tanh({drive} * v(s)) / {drive} : v(s) / sqrt(1 + 0.6 * {drive}^2 * v(s)^2)",
              # makes the solver land on every clock tick, where the chips take their samples
              f"Vclk clk 0 PULSE(0 1 {(period - 1.0 / SR) % period:.15g} 1n 1n 1n {period:.15g})", "Rclk clk 0 1k"]
    t, v = spice_lines(ngspice, work, "input", first, seconds, "q")
    q = interpolate(t, v, [max(x, 0.0) for x in tick])

    def bucket(j):
        return q[j] if 0 <= j < len(q) and tick[j] > 0.0 else 0.0
    held = [(1 - 2 * smear) * bucket(k - tap) + smear * (bucket(k - tap + 1) + bucket(k - tap - 1)) for k in range(ticks)]
    points = ["0 0"]
    for k in range(1, ticks):
        if tick[k] > 1e-9:
            points.append(f"{tick[k] - 0.5e-9:.15g} {held[k - 1]:.12g} {tick[k] + 0.5e-9:.15g} {held[k]:.12g}")

    second = ["Vz z 0 PWL(" + points[0]] + ["+ " + p for p in points[1:]] + ["+ )"]
    second += sallen_key("out", "z", "u", fc)
    if compander:
        second += ["Bee 0 ee I = (abs(v(u)) - v(ee)) / 10k", "Cee ee 0 1u",
                   f"Bout out 0 V = {RAIL} * tanh(v(u) * max(v(ee), {FLOOR}) / {REF} / {RAIL})", f".ic v(ee)={FLOOR}"]
    else:
        second += [f"Bout out 0 V = {RAIL} * tanh(v(u) / {RAIL})"]
    t, v = spice_lines(ngspice, work, "output", second, seconds, "out")
    return interpolate(t, v, [i / SR for i in range(int(seconds * SR))])


def run_model(exe, compander, character, delay, seconds, signal):
    r = subprocess.run([exe, str(int(compander)), str(character), str(delay), str(seconds), signal],
                       check=True, capture_output=True, text=True)
    return [float(x) for x in r.stdout.split()]


def db(x):
    return 20 * math.log10(max(x, 1e-12))


def rms(xs):
    return math.sqrt(sum(x * x for x in xs) / max(len(xs), 1))


def main():
    ngspice = find_ngspice()
    work = tempfile.mkdtemp(prefix="reverie-bbd-")
    exe = os.path.join(work, "BbdModel")
    subprocess.run(["clang++", "-std=c++17", "-O2", "-I", DSP, os.path.join(HERE, "BbdModel.cpp"), "-o", exe],
                   check=True)

    failures = 0
    # (what, compander, character, delay samples, seconds, signal, pass if the difference is below this, dB)
    cases = [
        ("short echo, clock 98 kHz",       False, 0.4, 2000,  0.08, "tones", -50),
        ("medium echo, clock 16 kHz",      False, 0.4, 12000, 0.30, "tones", -50),
        ("long echo, clock 10 kHz (aliasing)", False, 0.4, 30000, 0.68, "tones", -50),
        ("long echo, dark character",      False, 1.0, 30000, 0.68, "tones", -50),
        ("compander on, -40/-10 dBFS bursts", True, 0.4, 2000, 0.50, "bursts", -50),
    ]
    for what, compander, character, delay, seconds, signal, limit in cases:
        fc, smear, drive, clock, tap = chip_settings(compander, character, delay)
        count = int(seconds * SR)
        ref = run_spice(ngspice, work, compander, character, delay, seconds, TONES if signal == "tones" else BURSTS)
        model = run_model(exe, compander, character, delay, seconds, signal)
        if not compander:
            hiss = run_model(exe, compander, character, delay, seconds, "silence")
            model = [a - b for a, b in zip(model, hiss)]
        start = int((tap / clock) * SR)                    # from the first echo on
        diff = db(rms([a - b for a, b in zip(model[start:], ref[start:])]) / rms(ref[start:]))
        # level agreement in 5 ms windows, to show the compander tracks through the attacks and releases
        win, worst = int(0.005 * SR), 0.0
        for i in range(start, count - win, win):
            a, b = rms(model[i:i + win]), rms(ref[i:i + win])
            if b > 1e-6:
                worst = max(worst, abs(db(a) - db(b)))
        ok = diff < limit
        failures += not ok
        print(f"  {'PASS' if ok else 'FAIL'}  {what}: clock {clock:.0f} Hz, {tap} buckets, corner {fc:.0f} Hz; "
              f"difference {diff:.1f} dB, level within {worst:.2f} dB in every 5 ms")
    shutil.rmtree(work, ignore_errors=True)
    print("All passed" if failures == 0 else f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
