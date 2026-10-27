# Résultats 2026-10-27 — Dell RTX 4080 (AD103, 10de:2704) / Results

## FR — Procédure complète (carte identique uniquement)

**Avertissement** : le port est épinglé sur *notre* carte (subystème Dell
`1028:5113`, PMC `0x193000a1`, bus PCI 5, BAR0 `0xfb000000`, VRAM 16 Go). Une
carte/système différent **doit** d'abord exécuter sa propre sonde
(`windows/ad103-2705/diagnostic/`) et régénérer son profil ; ne forcez jamais
ces épingles sur une autre machine. Rien n'est écrit en flash : l'effet est
volatile et disparaît à la coupure secteur.

1. **Rebuild** (Windows, gcc MinGW/UCRT + GNU Make) : se reporter au README de
   `windows/ad103-2705/`. Appliquer d'abord
   `bruno-uncommitted-20261027.patch` sur le commit amont `c794f3e` (ou
   utiliser la branche de ce dépôt).
2. **Assets NVIDIA** : les générer/télécharger avec l'outil `firmware/` du
   dépôt (NVIDIA 610.43.03 épinglé, vérification SHA256). Les 5 fichiers et
   leurs hachages attendus : `assets-usb-manifest.sha256` (identiques au
   manifeste amont). Ne sont **pas** redistribués ici.
3. **Clé USB** (FAT32, GPT ou MBR, mode UEFI) :
   - `EFI\BOOT\BOOTX64.EFI` ← `build/AD104-EXPERIMENTAL.EFI` (notre bati :
     `5bb375c237e336eb92a46cda7369ffee3653fc6e4ded9480e4baf4536b300023`)
   - `AD104-ASSETS\` ← les 5 fichiers d'assets
   - la sonde optionnelle : `AD104-PROBE.EFI` (à renommer en `EFI\BOOT\BOOTX64.EFI` pour la capture)
4. **Cold boot** : extinction, **débrancher le secteur ~10 s** (seule la perte
   AC remet la carte à l'état stock), rebrancher, power-on, menu de boot
   (F11 sur cette carte mère), boot USB, appuyer sur `U`, attendre 30-40 s :
   le log est écrit sur la clé et Windows démarre seul.
5. **Vérification** : `AD104-UNLOCK\RUNxxxx.TXT` doit contenir
   `result=0 safe=1 ... flrs=1`, `SS0=88888888`, `SS1=00000008`,
   `readout1=0` et `TRANSACTION OK`. Exemple du nôtre : `RUN0001.TXT`.
   Si `STOP` en cours de transaction : éteindre, re-débrancher le secteur,
   re-capter avec la sonde avant toute nouvelle tentative.
6. **Bench** : Python 3.14 + PyTorch CUDA (ici `torch 2.14.1+cu126` - la build
   par défaut de PyPI est CPU-only, utiliser l'index `whl/cu126`) :
   ```
   python bench_all.py --out stock.json --label stock --size 4096
   python bench_all.py --out unlocked.json --label unlocked --size 4096
   python compare.py --before stock.json --after unlocked.json
   ```
   `compare.py` exige les mêmes hachages script/cuBLAS/torch/GPU avant/après.

## EN — What this folder is

| file | role |
|---|---|
| `RUN0001.TXT` | full EFI transaction journal of the successful unlock |
| `bench_all.py` | 4-mode reference bench (fp32 pedantic / fp16-fp32 / bf16-fp32 / e4m3-fp32 via `torch._scaled_mm`), dyadic-exact inputs, full FP64 correctness |
| `compare.py` | strict before/after comparator (identity of hashes + format, ratios, clocks, power) |
| `stock-20261027*.json` | 3 stock runs (first two hit a bench-script bug in the FP8 path, kept for transparency; run `-c` is the reference) |
| `unlocked-20261027.json` | post-unlock run, same script hash as `-c` |
| `compare-output.txt` | comparator output incl. per-mode power draw |
| `assets-usb-manifest.sha256` | SHA256 of the 5 NVIDIA asset files on our USB (all match upstream pinned manifest) |
| `bruno-uncommitted-20261027.patch` | our full delta vs upstream `c794f3e` (pins, tests, boot-io adaptations) |

## v2 runs (same day, 5 modes incl. FP8/FP16-accumulate)

`bench_all-v2.py` adds the `e4m3_fp16` mode (`torch._scaled_mm(...,
use_fast_accum=True, out_dtype=fp16)`), whose correctness is checked against a
documented per-MMA-stage FP16 rounding bound instead of exactness;
`compare-v2.py` accepts both check kinds. A clean AC reset cleared the unlock,
so both phases were re-run fresh the same day with identical hashes
(`benchmark_sha256=ce37f017b5ce629639c30d7b46217a2222a3feb4dd9cb74413a989ea90412250`):
`stock-20261027-v2.json` + `unlocked-20261027-v2.json` + `compare-output-v2.txt`
(ratios 0.995 / 1.868 / 1.964 / 1.706 / 1.672 wall, clock-normalized 2.00 /
2.10 / 1.82 / 1.95). The FP16-accumulate FP8 path is gated by the fuse too.

Summary (clock-normalized): f16/f32 **×1.98**, bf16/f32 **×2.00**,
e4m3/f32 **×1.90**, FP32 CUDA-core control **×1.00**. Details:
`docs/ad103-2704-dell-report.md`.
