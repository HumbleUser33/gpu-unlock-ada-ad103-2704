"""Strict stock-vs-unlocked comparison of bench_all.py JSONs.

Identity requirements (mirrors the fork's comparator intent): same benchmark
script hash, same cuBLAS/cuBLASLt hashes, same torch build, same GPU id, same
problem size and formats. Telemetry (clocks/power/temperature) is allowed to
differ and is reported.
"""
import argparse
import json
from pathlib import Path


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def identity(a, b):
    problems = []
    for key in ('benchmark_sha256', 'cublas_sha256', 'cublasLt_sha256',
                'torch', 'torch_cuda', 'gpu', 'capability', 'math_mode', 'seed'):
        if a[key] != b[key]:
            problems.append(f'{key}: {a[key]!r} != {b[key]!r}')
    ra, rb = a['results'], b['results']
    if [r['mode'] for r in ra] != [r['mode'] for r in rb]:
        problems.append('mode lists differ')
    for x, y in zip(ra, rb):
        if x.get('error') or y.get('error'):
            problems.append(f"{x['mode']}: incomplete ({x.get('error') or y.get('error')})")
            continue
        for key in ('n', 'samples', 'batch', 'output_type'):
            if x[key] != y[key]:
                problems.append(f"{x['mode']}.{key}: {x[key]!r} != {y[key]!r}")
    return problems


def field(telemetry_csv, index):
    # fields: name,bus_id,device_id,driver,vbios,power.limit,power.draw,
    #         temperature.gpu,clocks.sm,clocks.mem,utilization,memory.used
    return telemetry_csv.split(',')[index]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--before', type=Path, required=True)
    ap.add_argument('--after', type=Path, required=True)
    args = ap.parse_args()
    a, b = load(args.before), load(args.after)
    problems = identity(a, b)
    print(f'before: {a["label"]} ({a["utc"]})')
    print(f'after:  {b["label"]} ({b["utc"]})')
    if problems:
        print('IDENTITY MISMATCH (not comparable):')
        for p in problems:
            print('  -', p)
    else:
        print('identity: benchmark/cuBLAS/torch/GPU/format all match')
    print()
    print(f'{"mode":14s} {"stock TF":>9s} {"unlocked TF":>12s} {"ratio":>7s} '
          f'{"clock before":>13s} {"clock after":>12s} {"W before":>9s} {"W after":>8s} {"exact-ok":>8s}')
    for x, y in zip(a['results'], b['results']):
        if 'median_tflops' not in x or 'median_tflops' not in y:
            print(f'{x["mode"]:14s} incomplete: {x.get("error") or y.get("error")}')
            continue
        ok = x.get('full_256_exact') and x.get('sampled_large_exact') and \
             y.get('full_256_exact') and y.get('sampled_large_exact')
        print(f'{x["mode"]:14s} {x["median_tflops"]:9.2f} {y["median_tflops"]:12.2f} '
              f'{y["median_tflops"] / x["median_tflops"]:7.3f} '
              f'{int(field(x["telemetry_after"]["csv"], 8)):10d} MHz '
              f'{int(field(y["telemetry_after"]["csv"], 8)):9d} MHz '
              f'{float(field(x["telemetry_after"]["csv"], 6)):7.1f}W '
              f'{float(field(y["telemetry_after"]["csv"], 6)):6.1f}W '
              f'{"yes" if ok else "NO":>8s}')


if __name__ == '__main__':
    main()
