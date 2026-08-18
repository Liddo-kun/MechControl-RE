"""Analyze a Blackwell thermal pulse capture (frida jsonl) with optional NVIDIA telemetry csv.

Usage: python tools\analyze_pulse.py <capture.jsonl> [nvidia.csv] [--load-start SEC --load-end SEC]

Prints per-channel baseline/peak/cooldown stats, pairwise alias checks, and a
downsampled timeline. Times are seconds from first thermal sample.
"""
import json
import sys
import csv


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    jsonl_path = args[0]
    csv_path = args[1] if len(args) > 1 else None
    load_start = load_end = None
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == '--load-start':
            load_start = float(argv[i + 1])
        if a == '--load-end':
            load_end = float(argv[i + 1])

    samples = {}  # offset -> list of (t_sec, tempC)
    t0 = None
    with open(jsonl_path) as f:
        for line in f:
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if rec.get('t') != 'blackwell-temp' or not rec.get('valid'):
                continue
            if t0 is None:
                t0 = rec['timeMs']
            t = (rec['timeMs'] - t0) / 1000.0
            samples.setdefault(rec['offset'], []).append((t, rec['tempC']))

    for k in samples:
        samples[k].sort()

    offsets = sorted(samples)
    total = max(t for k in offsets for t, _ in samples[k])
    print('capture span: %.1f s, channels: %d' % (total, len(offsets)))

    # Per-channel summary
    print('\n%-9s %4s %9s %7s %8s %9s %10s' % (
        'offset', 'N', 'base(C)', 'peak(C)', 'rise(C)', 't_peak(s)', 'end(C)'))
    for k in offsets:
        pts = samples[k]
        base_pts = [c for t, c in pts if t < (load_start or 55)]
        base = sum(base_pts) / len(base_pts)
        peak_t, peak_c = max(pts, key=lambda p: p[1])
        end_c = pts[-1][1]
        print('%-9s %4d %9.2f %7.2f %8.2f %9.1f %10.2f' % (
            k, len(pts), base, peak_c, peak_c - base, peak_t, end_c))

    # Pairwise alias checks
    pairs = [('0xad0ae0', '0xad0adc'), ('0xad0ae0', '0xad0a90'),
             ('0xad0ad4', '0xad0a94'), ('0xad0ad8', '0xad0a98'),
             ('0xad0a9c', '0xad0aa0')]
    print('\npairwise (index-aligned):')
    for a, b in pairs:
        if a not in samples or b not in samples:
            continue
        n = min(len(samples[a]), len(samples[b]))
        diffs = [samples[a][i][1] - samples[b][i][1] for i in range(n)]
        exact = sum(1 for d in diffs if d == 0)
        mean = sum(diffs) / n
        mx = max(diffs, key=abs)
        # worst disagreement during rapid transitions (rise + early cooldown)
        print('  %s - %s: mean %+.4f  maxabs %+.4f  exact-equal %d/%d' % (
            a, b, mean, mx, exact, n))

    # Downsampled timeline
    print('\ntimeline (every 3rd batch):')
    hdr = 'time(s)  ' + '  '.join(k[-3:] for k in offsets)
    print(hdr)
    ref = samples[offsets[0]]
    for i in range(0, len(ref), 3):
        t = ref[i][0]
        row = ['%7.1f' % t]
        for k in offsets:
            row.append('%5.1f' % samples[k][i][1] if i < len(samples[k]) else '    -')
        print('  '.join(row))

    # NVIDIA telemetry summary
    if csv_path:
        print('\nnvidia telemetry:')
        with open(csv_path) as f:
            rows = list(csv.reader(f))
        hdr = [h.strip() for h in rows[0]]
        p_i = next(i for i, h in enumerate(hdr) if 'power' in h)
        t_i = next(i for i, h in enumerate(hdr) if 'temperature' in h)
        u_i = next(i for i, h in enumerate(hdr) if h.startswith('utilization.gpu'))
        pw = []
        for r in rows[1:]:
            try:
                pw.append((float(r[p_i].replace(' W', '')), int(r[t_i]),
                           int(r[u_i].replace(' %', ''))))
            except (ValueError, IndexError):
                continue
        peak_w = max(p for p, _, _ in pw)
        peak_t = max(t for _, t, _ in pw)
        print('  peak reported power: %.1f W (actual ~%.0f W with 1.337 shunt factor)'
              % (peak_w, peak_w * 1.337))
        print('  peak public temp: %d C, samples at 100%% util: %d'
              % (peak_t, sum(1 for _, _, u in pw if u >= 99)))


if __name__ == '__main__':
    main()
