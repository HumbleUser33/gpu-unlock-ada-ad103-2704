# AD103 (10de:2704) Dell OEM RTX 4080: stock → unlocked, 2026-10-27

One machine, one boot, one card, same discipline as the
[2705 report](ada-2705-report.md). This is the second physical AD103 result and
the first on a Dell OEM board SKU with a Dell subsystem.

## Verified configuration

| | |
|---|---|
| GPU | AD103, `10de:2704`, Dell subsystem `1028:5113`, 16376 MiB |
| VBIOS | `95.03.0E.80.06` (Dell OEM) |
| Board | ASRock B550M Steel Legend, Secure Boot off |
| PCI | `0000:05:00.0`, BAR0 `0xfb000000` |
| OS / driver | Windows 11, NVIDIA 610.43.03 assets, desktop driver 621.01 |
| Route | manual USB boot (`EFI/BOOT/BOOTX64.EFI`), one `U` press |

Board/card pins were re-derived from this machine's own probe capture with the
`diagnostic/` probe and the profile generator; the branch constants from 2705
were **not** reused for card values. Ported deltas are in
`windows/ad103-2705/results-2026-10-27/bruno-uncommitted-20261027.patch`
(against `c794f3e`): pin re-pinning (`10de:2704`, subsystem `1028:5113`, bus 5,
PMC `0x193000a1` gates kept), test-expectation updates for this machine's
handle graph, and boot-io adaptations (AUTO.TXT, ESP boot allowed, boot
manager hash reported instead of refused). No admission gate value was
weakened: every pinned register value comes from this card's capture.

Host test suite on this machine's data (logs in `windows/ad103-2705/build/`):
guards 17 admission checks, `efi_transaction` 90 scenarios, `app_flow` 272
checks, `msi_release` 849,535 checks across 47 ownership modes, `boot_io` 590
checks across 8 cases. All green.

## Transaction

Full journal: `windows/ad103-2705/results-2026-10-27/RUN0001.TXT`. Key lines:

```
entry: plm=0xffffff8f ss0=0x66025066 ss1=0x2 wpr2_hi=0
... chip=0x193000a1 pci=0x270410de
AD102 payload result: marker_ok=1 required_permissions=1 halted=1
selector result: SS0=0x88888888 expected=0x88888888 matched=1
selector result: SS1=0x8 expected=0x8 matched=1
AD102 selectors-applied: readout1=0 ... full=1
post-FLR ... SS0=88888888 SS1=00000008
Transaction result=0 safe=1 retained=0 started=1 quiesced=1 flrs=1 DMA_live=0
TRANSACTION OK
```

Cold state matched the vendor gate (`ss1=2`); after FLR both selectors read
back unlocked. Windows booted by itself after the transaction. No flash, ESP,
BCD or `bootmgfw.efi` change was ever made; the unlock is volatile and clears
on AC power loss.

## Measured (cuBLAS 4096², identical script/library hashes both phases)

`benchmark_sha256=0af8b8ea1d2e3ab44561cea2eac97be479c48ab25ecf2ccca2fae93bb7fe0bfe`,
`cublas_sha256=01d8172b25d8e2d2...`, `cublasLt_sha256=0d59d264782cec27...`,
torch 2.14.1+cu126, Python 3.14.3, math mode `DISALLOW_REDUCED_PRECISION_REDUCTION`,
seed 20261024, 15 samples × 5 GEMMs, 1 s warmup. Strict full-output FP64
correctness at 256² and 1,024 sampled elements at 4096² passed in both phases,
including FP8.

| mode | stock TF | unlocked TF | ratio | SM clock | power |
|---|---:|---:|---:|---|---|
| FP32 pedantic (control) | 37.35 | 37.33 | **1.000** | 2715 both | 302→311 W |
| FP16/FP32 | 101.44 | 188.47 | **1.858** | 2805→2625 MHz | 212→299 W |
| BF16/FP32 | 101.16 | 192.62 | **1.904** | 2805→2670 MHz | 210→260 W |
| e4m3/FP32 (`_scaled_mm`) | 191.36 | 324.19 | **1.694** | 2805→2340 MHz | 276→285 W |

Clock-normalized (per-SM-cycle) ratios: 1.98× / 2.00× / 1.90×, matching the
register-resident issue-rate ratios recorded on the 2705 card (2.01× / 2.00× /
1.91×). Wall ratios sit below 2.0 because unlocked tensor load pushes the card
against its 320 W power ceiling and clocks drop; power stays capped, performance
per watt improves 32-65 %. The FP32 CUDA-core control moved 0.02 TF (0.05 %),
so the change is tensor-path only.

Stock GEMM rates (93 % of the half-rate ceiling, 76 SM × 256 FMA/cycle) match
the 4070 Ti stock signature; the AD103 fuse behaves exactly like AD104 despite
NVIDIA spec quoting 97.6 TF dense (the spec quotes the half rate).

## Caveats

Single cold A/B, single boot, no A/B/A yet, point telemetry samples, no
long-duration soak. Gaming gains: none expected or observed. The e4m3 number is
cuBLASLt GEMM throughput, not an issue rate. Sharing note: NVIDIA firmware
assets are not redistributed here; regenerate them with `firmware/` against the
pinned `610.43.03` manifest (the five USB asset SHA256s match the upstream
`firmware/artifacts-610.43.03.json` exactly - see
`results-2026-10-27/assets-usb-manifest.sha256`).
