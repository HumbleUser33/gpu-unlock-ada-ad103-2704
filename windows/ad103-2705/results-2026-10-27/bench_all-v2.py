"""Reference bench: FP32-accumulate tensor-core rates (stock baseline).

Extended from gpu-unlock-ada/windows/ad103-2705/benchmark.py (fork original kept
untouched for its own benchmark_sha256 identity). Same discipline: dyadic exact
inputs, full float64 correctness checks, nvidia-smi telemetry, refusal to
overwrite an existing measurement. Adds the e4m3 (FP8) input with FP32
accumulate mode, which the fork's three-mode bench does not cover.

Modes:
  fp32_pedantic  control path, must NOT change after unlock (clock control)
  fp16_fp32      FP16 in, FP32 acc - expected ~2x after unlock
  bf16_fp32      BF16 in, FP32 acc - expected ~2x after unlock
  e4m3_fp32      FP8 in, FP32 acc - the open question (torch._scaled_mm via
                 cuBLASLt; e4m3 inputs chosen k/8 so quantization is lossless
                 and FP32 sums are exact)
  e4m3_fp16      FP8 in, FP16 acc (torch._scaled_mm use_fast_accum=True, cuBLASLt
                 FAST_ACCUM). Inherently inexact: correctness is checked
                 against a documented per-MMA-stage rounding bound
                 (ceil(k/32) stages x half FP16 ULP at |C|max), plus a 6%
                 relative sanity cap.
"""
import argparse
import ctypes as ct
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time
from datetime import datetime, timezone

import numpy as np
import torch


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def telemetry():
    fields = 'name,pci.bus_id,pci.device_id,driver_version,vbios_version,power.limit,power.draw,temperature.gpu,clocks.sm,clocks.mem,utilization.gpu,memory.used'
    p = subprocess.run(['nvidia-smi', '--query-gpu=' + fields,
                        '--format=csv,noheader,nounits'], capture_output=True, text=True)
    return {'fields': fields.split(','), 'csv': p.stdout.strip(),
            'returncode': p.returncode, 'error': p.stderr.strip()}


class Blas:
    def __init__(self):
        self.path = Path(torch.__file__).parent / 'lib' / 'cublas64_12.dll'
        if not self.path.is_file():
            raise RuntimeError('Requires the installed PyTorch CUDA 12 cuBLAS DLL: ' + str(self.path))
        self.dll_dir = os.add_dll_directory(str(self.path.parent))
        self.lib = ct.WinDLL(str(self.path))
        p, i = ct.c_void_p, ct.c_int
        signatures = {
            'cublasCreate_v2': [ct.POINTER(p)],
            'cublasDestroy_v2': [p],
            'cublasSetStream_v2': [p, p],
            'cublasSetMathMode': [p, i],
            'cublasGetVersion_v2': [p, ct.POINTER(i)],
            'cublasGemmEx': [p, i, i, i, i, i, p, p, i, i, p, i, i, p, p, i, i, i, i],
        }
        for name, args in signatures.items():
            f = getattr(self.lib, name)
            f.argtypes, f.restype = args, i
        self.handle = p()
        self.check(self.lib.cublasCreate_v2(ct.byref(self.handle)))
        self.check(self.lib.cublasSetStream_v2(self.handle, torch.cuda.current_stream().cuda_stream))
        # CUBLAS_MATH_DISALLOW_REDUCED_PRECISION_REDUCTION.
        self.check(self.lib.cublasSetMathMode(self.handle, 16))
        version = i()
        self.check(self.lib.cublasGetVersion_v2(self.handle, ct.byref(version)))
        self.version = version.value

    @staticmethod
    def check(status):
        if status != 0:
            raise RuntimeError(f'cuBLAS status {status}')

    def gemm(self, a, b, c, input_type, compute):
        n = a.shape[0]
        alpha, beta = ct.c_float(1.0), ct.c_float(0.0)
        # cuBLAS column-major: C_row=A_row@B_row is C_col=B_col@A_col.
        self.check(self.lib.cublasGemmEx(
            self.handle, 0, 0, n, n, n, ct.byref(alpha),
            b.data_ptr(), input_type, n, a.data_ptr(), input_type, n,
            ct.byref(beta), c.data_ptr(), 0, n, compute, -1))

    def close(self):
        self.check(self.lib.cublasDestroy_v2(self.handle))
        self.dll_dir.close()


def matrices(n, dtype, seed):
    rng = np.random.default_rng(seed)
    # Dyadic inputs exactly representable in FP16, BF16 and FP32.
    a = rng.integers(-16, 17, size=(n, n), dtype=np.int16).astype(np.float32) / 32
    b = rng.integers(-16, 17, size=(n, n), dtype=np.int16).astype(np.float32) / 32
    return a, b, torch.from_numpy(a).to('cuda', dtype), torch.from_numpy(b).to('cuda', dtype)


def fp8_matrices(n, seed):
    rng = np.random.default_rng(seed)
    # k/8 with |k| <= 15: |v| < 2 so every value is an e4m3 normal with step 1/8
    # (exact). Products are multiples of 1/64 with |kl| <= 225, so any sum of up
    # to 4096 of them is M/64 with |M| < 2**20: exactly representable in FP32
    # whatever accumulation order the tensor cores use.
    a = rng.integers(-15, 16, size=(n, n), dtype=np.int16).astype(np.float32) / 8
    b = rng.integers(-15, 16, size=(n, n), dtype=np.int16).astype(np.float32) / 8
    at = torch.from_numpy(a).to('cuda').to(torch.float8_e4m3fn)
    bcol = torch.from_numpy(b).t().contiguous().to('cuda').to(torch.float8_e4m3fn)
    bt = bcol.t()
    assert torch.equal(at.float(), torch.from_numpy(a).to('cuda')), 'fp8 A quantization lost values'
    assert torch.equal(bt.float(), torch.from_numpy(b).to('cuda')), 'fp8 B quantization lost values'
    return a, b, at, bt


def fp8_mm(a, b, scale, fast_accum=False):
    out = torch.float16 if fast_accum else torch.float32
    return torch._scaled_mm(a, b, scale_a=scale, scale_b=scale,
                            out_dtype=out, use_fast_accum=fast_accum)


def fp16_ulp(x):
    x = abs(float(x))
    if x < 2 ** -14:
        return 2.0 ** -24
    import math
    return 2.0 ** (math.floor(math.log2(x)) - 10)


def fp16_acc_bound(k, c_ref):
    """Conservative rounding bound for Ada FP8 with FP16 accumulate: k/32
    tensor-core MMA stages, each rounding into an FP16 accumulator by at most
    half an ULP of the largest reference magnitude."""
    import math
    m = float(np.max(np.abs(c_ref))) if c_ref.size else 1.0
    return math.ceil(k / 32) * 0.5 * fp16_ulp(m)


def check_fp16_acc(actual, expected, k, tag):
    err = float(np.max(np.abs(actual.astype(np.float64) - expected)))
    bound = fp16_acc_bound(k, expected)
    sanity = 0.06 * float(np.max(np.abs(expected)))
    if not np.all(np.isfinite(actual)) or err > min(bound, sanity):
        raise RuntimeError(f'{tag}: FP16-accumulate error {err} exceeds bound '
                           f'{min(bound, sanity):.3f} (stage {bound:.3f}, sanity {sanity:.3f})')
    return err, bound, sanity


def correctness(blas, dtype, input_type, compute):
    ah, bh, a, b = matrices(256, dtype, 20261024)
    c = torch.empty((256, 256), device='cuda', dtype=torch.float32)
    blas.gemm(a, b, c, input_type, compute)
    torch.cuda.synchronize()
    actual = c.cpu().numpy()
    expected = ah.astype(np.float64) @ bh.astype(np.float64)
    err = float(np.max(np.abs(actual - expected)))
    if not np.array_equal(actual, expected):
        raise RuntimeError(f'Full 256x256 correctness failure, maximum error {err}')
    return {'full_256_exact': True, 'full_256_max_abs_error': err}


def correctness_fp8(fast_accum=False):
    ah, bh, a, b = fp8_matrices(256, 20261024)
    scale = torch.ones((), device='cuda', dtype=torch.float32)
    c = fp8_mm(a, b, scale, fast_accum)
    torch.cuda.synchronize()
    actual = c.cpu().numpy()
    expected = ah.astype(np.float64) @ bh.astype(np.float64)
    if not fast_accum:
        err = float(np.max(np.abs(actual - expected)))
        if not np.array_equal(actual, expected):
            raise RuntimeError(f'FP8 full 256x256 correctness failure, maximum error {err}')
        return {'full_256_exact': True, 'full_256_max_abs_error': err}
    err, bound, sanity = check_fp16_acc(actual, expected, 256, 'FP8/FP16-acc 256x256')
    return {'full_256_within_bound': True, 'full_256_max_abs_error': err,
            'fp16_acc_error_bound': bound, 'fp16_acc_sanity_cap': sanity}


def run_mode(blas, mode, n, samples, batch, warmup):
    name, dtype, input_type, compute = mode
    result = correctness(blas, dtype, input_type, compute)
    ah, bh, a, b = matrices(n, dtype, 20261024)
    c = torch.empty((n, n), device='cuda', dtype=torch.float32)
    torch.cuda.synchronize()
    deadline = time.perf_counter() + warmup
    while time.perf_counter() < deadline:
        for _ in range(batch):
            blas.gemm(a, b, c, input_type, compute)
        torch.cuda.synchronize()
    before = telemetry()
    times = []
    for _ in range(samples):
        start, end = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
        start.record()
        for _ in range(batch):
            blas.gemm(a, b, c, input_type, compute)
        end.record()
        end.synchronize()
        times.append(start.elapsed_time(end) / batch)
    after = telemetry()
    result.update(telemetry_before=before, telemetry_after=after)
    finish(blas, name, c, ah, bh, times, result, n, samples, batch)
    return result


def run_fp8(n, samples, batch, warmup, fast_accum=False, name='e4m3_fp32'):
    result = correctness_fp8(fast_accum)
    ah, bh, a, b = fp8_matrices(n, 20261024)
    scale = torch.ones((), device='cuda', dtype=torch.float32)
    c = fp8_mm(a, b, scale, fast_accum)
    torch.cuda.synchronize()
    deadline = time.perf_counter() + warmup
    while time.perf_counter() < deadline:
        for _ in range(batch):
            c = fp8_mm(a, b, scale, fast_accum)
        torch.cuda.synchronize()
    before = telemetry()
    times = []
    for _ in range(samples):
        start, end = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
        start.record()
        for _ in range(batch):
            c = fp8_mm(a, b, scale, fast_accum)
        end.record()
        end.synchronize()
        times.append(start.elapsed_time(end) / batch)
    after = telemetry()
    result.update(telemetry_before=before, telemetry_after=after)
    finish(None, name, c, ah, bh, times, result, n, samples, batch,
           exact=not fast_accum)
    result['path'] = ('torch._scaled_mm cuBLASLt FP8 use_fast_accum=True'
                      if fast_accum else 'torch._scaled_mm cuBLASLt FP8')
    return result


def finish(blas, name, c, ah, bh, times, result, n, samples, batch, exact=True):
    if not bool(torch.isfinite(c).all().item()):
        raise RuntimeError(name + ': nonfinite output')
    idx = np.unique(np.linspace(0, n - 1, min(32, n), dtype=np.int64))
    expected = ah[idx, :].astype(np.float64) @ bh[:, idx].astype(np.float64)
    actual = c[torch.as_tensor(idx, device='cuda')[:, None],
               torch.as_tensor(idx, device='cuda')[None, :]].cpu().numpy()
    if exact:
        err = float(np.max(np.abs(actual - expected)))
        if not np.array_equal(actual, expected):
            raise RuntimeError(name + f': sampled {n}x{n} correctness failure, error {err}')
        result.update(sampled_large_exact=True,
                      sampled_large_max_abs_error=err)
    else:
        err, bound, sanity = check_fp16_acc(actual, expected, n,
                                            f'{name} sampled {n}x{n}')
        result.update(sampled_large_within_bound=True,
                      sampled_large_max_abs_error=err,
                      sampled_large_error_bound=min(bound, sanity))
    median = statistics.median(times)
    # telemetry captured by callers into result before calling this helper
    result.update(mode=name, n=n, samples=samples, batch=batch, milliseconds=times,
                  median_ms=median, median_tflops=2 * n ** 3 / (median * 1e9),
                  min_ms=min(times), max_ms=max(times),
                  output_type='FP16' if not exact else 'FP32',
                  sampled_large_elements=int(len(idx) ** 2),
                  output_sha256=hashlib.sha256(c.cpu().numpy().tobytes()).hexdigest())
    print(f'{name}: {result["median_tflops"]:.2f} TFLOP/s; numerical checks passed', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--label', default='stock')
    ap.add_argument('--size', type=int, default=4096, choices=[256, 1024, 2048, 4096, 8192])
    ap.add_argument('--samples', type=int, default=15)
    ap.add_argument('--batch', type=int, default=5)
    ap.add_argument('--warmup', type=float, default=1.0)
    ap.add_argument('--skip-fp8', action='store_true')
    args = ap.parse_args()
    if args.samples < 3 or args.batch < 1 or args.warmup < 0:
        ap.error('samples >= 3, batch >= 1, warmup >= 0 required')
    if os.name != 'nt' or not torch.cuda.is_available():
        raise RuntimeError('Requires Windows with the existing CUDA-enabled PyTorch install')
    if args.out.exists():
        raise FileExistsError('Refusing to replace an existing measurement: ' + str(args.out))
    torch.cuda.set_device(0)
    torch.set_num_threads(4)
    torch.backends.cuda.matmul.allow_fp16_reduced_precision_reduction = False
    blas = Blas()
    try:
        data = dict(schema=2, label=args.label, utc=datetime.now(timezone.utc).isoformat(),
                    python=platform.python_version(), torch=torch.__version__,
                    torch_cuda=torch.version.cuda, gpu=torch.cuda.get_device_name(0),
                    capability=list(torch.cuda.get_device_capability(0)),
                    cublas_version=blas.version, cublas_sha256=sha256(blas.path),
                    cublasLt_sha256=sha256(blas.path.parent / 'cublasLt64_12.dll'),
                    benchmark_sha256=sha256(__file__),
                    math_mode=16, seed=20261024, warmup_seconds=args.warmup,
                    note='cuBLAS GEMM throughput, not register-resident instruction rate or proof of unlock',
                    initial_telemetry=telemetry(), results=[])
        modes = [('fp32_pedantic', torch.float32, 0, 69),
                 ('fp16_fp32', torch.float16, 2, 68),
                 ('bf16_fp32', torch.bfloat16, 14, 68)]
        for mode in modes:
            data['results'].append(run_mode(blas, mode, args.size, args.samples, args.batch, args.warmup))
        if not args.skip_fp8:
            for fast, mode_name in ((False, 'e4m3_fp32'), (True, 'e4m3_fp16')):
                try:
                    data['results'].append(run_fp8(args.size, args.samples,
                                                   args.batch, args.warmup,
                                                   fast_accum=fast, name=mode_name))
                except Exception as exc:  # record, do not lose the other modes
                    data['results'].append({'mode': mode_name, 'error': repr(exc)})
                    print(mode_name + ' FAILED:', repr(exc), flush=True)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(data, indent=2), encoding='utf-8')
        print('Saved:', args.out)
    finally:
        blas.close()


if __name__ == '__main__':
    main()
