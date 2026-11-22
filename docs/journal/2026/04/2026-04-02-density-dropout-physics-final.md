---
id: "2026-04-02-density-dropout-physics-final"
date: 2026-04-02
title: "LiDAR Point Density Dropout — Complete Physics Model and Fitting Attempts"
tags: [physics, SNR, solar, specular, density-model, beam-angle, ardoise, fitting, theory]
category: finding
status: concluded
refs: ["2026-04-01-lidar-point-density-physics-model", "2026-04-01-lidar-slate-physics-research", "2026-04-01-overnight-research-compilation", "2026-03-31-boundary-next-gen-plan"]
---

# LiDAR Point Density Dropout — Complete Physics Model and Fitting Attempts

## Problem statement

On the dahouet test case (Pléneuf-Val-André, Brittany), 7 ardoise (slate) roof
planes at ~45° tilt show dramatic per-strip density variation. South-facing planes
lose nearly all P16 points (35:1 ratio) while north-facing planes are balanced.
An 8th plane (ghost_SE) is nearly undetectable on both strips.

This document records the complete physics model developed over 2 sessions,
all fitting attempts (successes and failures), and open questions for future work.

---

## Acquisition conditions

| Parameter | Value |
|-----------|-------|
| Date | 2024-03-07 |
| P16 time | 12:49 UTC (13:49 CET) |
| P15 time | 12:54 UTC (13:54 CET) |
| Weather | Partly sunny ("passing clouds"), 10-11°C, humidity 58-67%, vis 16km |
| Sensor | Riegl VQ-1560 II (probable, dual channel) |
| Wavelength | 1064 nm |
| Altitude | 1250 m AGL (measured from scan angle triangulation) |
| Slant range | ~1280 m |
| Scan angle range | 11.7° – 15.7° (building at swath edge) |
| Flight direction | N-S (~0°/180°) |
| Material | Ardoise (Breton dark slate) |

### Sun position (per strip, verified with two independent methods)

| Strip | Elevation | Azimuth |
|-------|-----------|---------|
| P16 | 36.1° | 188.6° (SSW) |
| P15 | 36.0° | 190.1° (SSW) |

Sun position verified via NOAA solar calculator formulas AND simplified
astronomical algorithm — both agree to 0.1°.

---

## The 7 ardoise planes (data)

| Slug | Dir | Tilt | PSID | Pts | Inc° | Bisect° | Solar | Intensity | Density |
|------|-----|------|:----:|----:|-----:|--------:|------:|----------:|--------:|
| rich_ash | NW 312° | 44° | P15 | 518 | 55.0 | 68.3 | 0.124 | 591 | 23.3 |
| rich_ash | NW 312° | 44° | P16 | 406 | 36.8 | 56.9 | 0.113 | 654 | 18.3 |
| high_ace | N 357° | 44° | P15 | 354 | 59.5 | 78.6 | 0.000 | 523 | 11.1 |
| high_ace | N 357° | 44° | P16 | 501 | 32.1 | 64.8 | 0.000 | 692 | 15.8 |
| west_arc | NE 42° | 44° | P15 | 277 | 56.2 | 74.8 | 0.000 | 568 | 9.4 |
| west_arc | NE 42° | 44° | P16 | 435 | 35.9 | 63.0 | 0.000 | 827 | 14.8 |
| lame_fur | NW 312° | 44° | P15 | 331 | 55.2 | 68.4 | 0.122 | 548 | 13.3 |
| lame_fur | NW 312° | 44° | P16 | 238 | 36.9 | 57.0 | 0.110 | 668 | 9.5 |
| zero_gap | S 177° | 45° | P15 | 188 | 29.9 | 12.4 | 0.972 | 408 | 22.8 |
| zero_gap | S 177° | 45° | P16 | 18 | 57.5 | 25.2 | 0.976 | 400 | 2.2 |
| bay_cat | SW 223° | 45° | P15 | 177 | 34.7 | 24.0 | 0.898 | 387 | 27.0 |
| bay_cat | SW 223° | 45° | P16 | 5 | 54.4 | 28.3 | 0.890 | 387 | 0.8 |

### Ghost SE plane (10 manually identified points)

| Slug | Dir | Tilt | PSID | Pts | Inc° | Bisect° | Solar | Intensity |
|------|-----|------|:----:|----:|-----:|--------:|------:|----------:|
| ghost_SE | SE 131° | 45° | P15 | 10 | 36.6 | 36.7 | 0.688 | 283 (mean) |
| ghost_SE | SE 131° | 45° | P16 | 0 | 53.8 | 38.4 | 0.704 | — |

All 10 ghost points are P15 only. Intensity 144-352, min=144. Classifications
mixed: 3× cls 6 (building), 4× cls 1 (unclassified), 3× cls 5 (vegetation).
Points saved in `test-cases-v2/dahouet-ghost-se-points.json`.

---

## Column definitions

| Column | Definition |
|--------|-----------|
| **Inc°** | Beam-surface incidence angle. 0°=beam normal to surface, 90°=grazing. Computed from reconstructed beam direction (scan angle + cross-track gradient) dotted with plane normal. |
| **Bisect°** | Specular proximity. Angle between plane normal and the bisector of (toSensor, toSun). 0°=plane normal IS the bisector=perfect specular reflection into sensor. |
| **Solar** | Diffuse solar illumination. `max(0, dot(plane_normal, toSun))`. 0=plane faces away from sun, 1=plane faces directly at sun. |
| **Intensity** | Mean LiDAR return amplitude (Uint16, Riegl calibrated). After sensor's internal background subtraction. |
| **Density** | Point count / plane area (pts/m²). |

---

## The complete physics model

### Signal chain

```
SIGNAL (per-pulse return)

  S_energy = Ep × ρ × A_recv × T²_atm / (π × R²)
  
  For EXTENDED targets (roof plane >> footprint):
    cos(θ_i) cancels between Lambert cosine and illuminated area increase.
    Total return ENERGY is constant regardless of incidence angle.
  
  BUT: the return PULSE BROADENS at grazing incidence.
    Footprint elongates by 1/cos(θ_i) → pulse stretches temporally.
    Peak amplitude: S_peak ∝ S_energy × cos(θ_i)
    
  The Riegl detector measures PEAK amplitude, not total energy.
  Therefore: S_effective ∝ cos(θ_i)
```

### Noise chain

```
NOISE (solar background in receiver per gate)

  Three components:
  
  1. DIFFUSE: sun illuminates surface, Lambertian scatter into hemisphere,
     fraction Ω/π enters receiver.
     N_diff ∝ E_sun × Δλ × cos(θ_sun) × ρ_diff × A_recv × Ω / π
     where cos(θ_sun) = dot(normal, toSun) = "solar" column
  
  2. SPECULAR: sun illuminates surface, specular lobe reflects toward sensor.
     N_spec ∝ E_sun × Δλ × cos(θ_sun) × ρ_spec × BRDF(α_bisect) × A_recv × Ω
     where BRDF(α) = exp(-α²/(2σ²)) for Gaussian lobe
     σ ≈ 5-15° for ideal smooth slate
     σ ≈ 24-31° from our empirical fits (broader than expected)
  
  3. DETECTOR: constant electronic noise floor.
     N_det = constant (independent of geometry)
  
  Total: N = N_diff + N_spec + N_det
```

### Detection

```
SNR = S_peak / N_total

Point detected if SNR > threshold.

Detection probability: P_detect = sigmoid(steepness × (SNR - threshold))
  Sharp transition: P_detect ≈ 1 above threshold, ≈ 0 below.

Observed density = D_nominal × cos(θ_i) × P_detect
  where cos(θ_i) = geometric scan pattern dilution (beam footprint spreading)
  and P_detect = fraction of pulses exceeding SNR threshold
```

### Ardoise tile surface normal correction

Individual slate tiles overlap like fish scales. Each tile is ~2-4° more
upright than the mathematical roof plane:
  - Slate thickness: 3-5mm
  - Hook thickness: 2-3mm
  - Pureau (exposed length): 100-140mm
  - Lift angle: atan(buildup / pureau) ≈ 2-4°

Effect: shifts bisector angle by 2-4°. Too small to explain model discrepancies.

### Sensor parameters (estimated)

| Parameter | Value | Confidence |
|-----------|-------|------------|
| E_sun at 1064nm | 0.65 W/m²/nm | Good (ASTM G173) |
| Ep (pulse energy) | 5-100 µJ (PRR dependent) | Low (proprietary) |
| A_recv | ~50 cm² (80mm aperture) | Medium |
| Δλ (bandpass) | ~1 nm | Medium |
| IFOV | ~1 mrad | Low |
| T_atm (two-way) | ~0.85 | Good |
| R (range) | 1280 m | Measured |
| t_gate | ~10 ns | Medium |
| ρ_diff (dry slate) | ~0.12 | Medium |
| ρ_spec (specular fraction) | ~0.10 | Low |

### Material properties — Ardoise at 1064 nm

| Property | Value |
|----------|-------|
| Diffuse reflectance (dry) | ~0.12 (±0.05) |
| Diffuse reflectance (wet) | ~0.06-0.10 |
| Specular lobe half-width | ~5-15° (literature) |
| BRDF model | Cook-Torrance with moderate roughness |
| Refractive index | ~1.5-1.6 |
| Brewster angle | ~56° |
| Fresnel at 30-60° | 4-9% (negligible) |

---

## Absolute S/N computation — the paradox

Using the standard LiDAR link budget with our sensor parameters:

```
S (per pulse, 100µJ) = 9.9 fJ = 53,000 photons
N (worst case solar, per 10ns gate) = 0.005 fJ = 24 photons
S/N = 2,200 — solar noise should be negligible!
```

Even at extreme parameters (5µJ pulse, 5mrad IFOV, 20nm bandpass), S/N ≈ 10.
The **continuous solar background model cannot explain the observed dropout**
at any reasonable sensor parameters.

### Possible explanations for the paradox

1. **Pulse energy at operational PRR is much lower than assumed**. At 2MHz
   effective PRR, pulse energy may be ~5µJ. This brings S down 20×.

2. **Specular BRDF peak is much stronger than Gaussian**. Cook-Torrance for
   smooth wet slate (α=0.05) gives specular peak 42× diffuse. At bisector=0°,
   specular noise could reach 0.16 fJ vs 0.5 fJ signal (at 5µJ) → S/N ≈ 3.

3. **The mechanism is not continuous background but transient effects**:
   APD saturation, AGC gain reduction, waveform processing artifacts, or
   MTA (multiple time around) zone conflicts.

4. **The IFOV or effective receiver solid angle is larger than estimated**.

This paradox remains **unresolved**. The dropout is real and repeatable
(confirmed in literature: Mandlburger et al. 2015) but the standard link
budget says it shouldn't happen.

---

## Fitting attempts — chronological summary

### Attempt 1: Simple interaction model (no physics)

```
D = D₀ × cos(θ)^α × max(0, 1 - S_penalty × (1-cos(θ))^β)
```

Best fit (12 points): D₀=20.5, α=0.5, k_s=43, σ=31°, β=4.0, RMSE=5.15

- Captures S/SW dropout qualitatively
- Fails on ghost_SE (predicts 17.8, actual 0.2)
- Per-plane variance unexplained (rich_ash 23 vs west_arc 9 at similar angles)

### Attempt 2: SNR-subtractive model (key insight from overnight experiments)

```
D = D₀ × cos(θ)^α × max(0, 1 - k_s × sol × lobe(bis,σ) / cos(θ)^β)
```

The `1/cos(θ)^β` divides the penalty by signal strength — solar noise is
constant but return signal weakens at grazing angles. The SNR ratio degrades.

Best fit (per-building D₀, 14 points): α=0.30, k_s=0.72, σ=23.6°, β=2.12,
**RMSE=1.95** — best achieved.

- Correctly predicts bay_cat P16 (0.8) and zero_gap P16 (2.2)
- Still fails on ghost_SE

### Attempt 3: Fresnel/Brewster reflection

Tested whether Fresnel reflection at grazing angles explains additional signal
loss. For slate (n=1.5), Fresnel reflectance varies only 4-9% across 30-60°.
Even at n=10, can't explain ghost_SE. **Dead end.**

### Attempt 4: Oren-Nayar rough surface model

Tested whether rough-surface BRDF correction improves on Lambert cosine.
The Oren-Nayar B term **vanishes identically for monostatic LiDAR** because
cos(φ_r - φ_i) = cos(π) = -1, max(0, -1) = 0. Reduces to scaled Lambert.
**Dead end.**

### Attempt 5: Sigmoid detection probability

```
D = D₀ × cos(θ_i) × sigmoid(steep × (SNR - threshold))
```

Various SNR formulations tested. Without per-building parameters,
RMSE ≈ 5.0-7.0. Cannot beat the simple model.

### Attempt 6: Sigmoid + per-building albedo

```
D = D_max × sigmoid(steep × (albedo × cos(θ)^α - k × noise - threshold))
```

With per-building albedo as free parameter (12 params for 14 points):
**RMSE=2.49**, correctly predicts ghost_SE near zero.

Fitted albedos split cleanly:
- Bright: rich_ash (1.98), zero_gap (1.78), bay_cat (1.36)
- Dark: lame_fur (0.14), high_ace (0.10), west_arc (0.08), ghost_SE (0.08)

**Mechanistically correct but severely overfitted.** The albedo proxy could
come from LiDAR return intensity, but intensity is contaminated by the same
solar noise we're modeling (circular dependency).

### Attempt 7: Pure geometry S/N ratio

```
S = Q × cos(θ_i)  (pulse peak broadening)
N = sol × (1 + k × exp(-bis²/(2σ²))) + N0
SNR = S / N
```

Calibrated Q so that bay_cat P16 → SNR ≈ 1 (barely surviving).

Result: **zero_gap P15 gets SNR=0.89 yet has density 22.8** (highest!).
The model says it should be dead because its noise is high (bisect=12.4°,
very close to specular peak). But it survives fine.

**The fundamental problem**: on the same south-facing plane, both strips
see similar noise (similar solar dot and similar bisector angles), but P15
survives (cos=0.87) and P16 dies (cos=0.54). The cos(θ) signal difference
is only 1.6× — not enough for the sharp dropout.

### Summary of what works and what doesn't

| Approach | RMSE-12 | ghost_SE | Problem |
|----------|---------|----------|---------|
| cos^α + interaction penalty | 5.15 | 17.8 (miss) | Per-plane variance |
| SNR-subtractive (per-bldg D₀) | 1.95 | OK-ish | Overfitted |
| Fresnel/Brewster | — | — | Negligible effect |
| Oren-Nayar | 6.62 | 16.7 (miss) | Irrelevant for monostatic |
| Sigmoid + albedo | 2.49 | 0.2 (hit!) | 12 params / 14 points |
| Pure geometry S/N | — | — | Can't separate P15 vs P16 |

---

## Strong evidence (confirmed)

1. **Beam incidence drives strip asymmetry**: P16 has higher incidence on
   S/SW planes, lower on N/NE. The asymmetry is purely geometric (different
   cross-track position of each strip).

2. **Solar geometry correlates with dropout**: dropout occurs on planes with
   high solar illumination (south-facing, solar > 0.89) AND high incidence
   (>54°). Neither alone causes dropout.

3. **Bisector proximity tracks the dropout direction**: dropout planes have
   bisect 12-28° (specular zone), normal planes have 55-79°.

4. **Min intensity shifts with solar exposure**: healthy planes min=48,
   dropout planes min=144. The noise floor rises 3× on sun-illuminated planes.

5. **Intensity tracks cos(θ_i) on clean planes**: confirms peak-amplitude
   detection (not total-energy).

6. **Fresnel/Brewster negligible**: 4-9% effect, can't explain dropout.

7. **Oren-Nayar irrelevant for monostatic**: B term vanishes at θ_r = θ_i.

8. **Extended target signal cancellation**: cos(θ) in Lambert cancels with
   1/cos(θ) in illuminated area. Return energy is constant, but peak amplitude
   drops as cos(θ) due to pulse broadening.

9. **Ardoise tile surface ~2-4° more upright than roof plane**: from overlap
   geometry (slate thickness / pureau). Too small to explain discrepancies.

10. **Absolute S/N paradox**: standard link budget says S/N >> 1000 even in
    worst case. Continuous solar background model can't explain dropout.

---

## Open questions

1. **What mechanism causes the actual dropout?** The continuous background
   model fails by 3 orders of magnitude. The real mechanism may be transient
   (specular flash saturating APD), systemic (AGC reducing gain), or related
   to waveform processing (elevated background variance confusing peak
   detection).

2. **Why does zero_gap P15 survive at bisect=12.4°?** This is the closest to
   perfect specular of any measurement, yet has the highest density. The noise
   SHOULD be maximal here. Either the bisector calculation has a subtle error,
   or the specular model is wrong, or something compensates (maybe the strong
   signal at low incidence overwhelms even specular noise).

3. **What explains ghost_SE?** All physics models predict 7-18 pts/m² where
   actual is ~0. Per-building albedo model correctly predicts it (albedo=0.08)
   but is overfitted. Could be: different slate condition, local obstruction,
   or an acquisition geometry blind spot.

4. **Per-plane density variance on clean planes**: rich_ash (23.3) vs west_arc
   (9.4) at similar geometry. Strip overlap zones? Local point thinning?
   Different slate weathering/condition per face?

5. **Can LiDAR return intensity serve as albedo proxy?** The intensity IS
   contaminated by solar noise (south-facing planes show 30% lower intensity).
   But north-facing planes (solar=0) give clean cos(θ) calibration. The
   difference on south-facing planes measures the solar contamination.

---

## Recommended next steps

1. **More buildings**: fit on 5-10 buildings across different orientations,
   materials, acquisition times. The 42 real test cases + new imports provide
   the data. This breaks parameter degeneracies.

2. **Overcast vs sunny comparison**: buildings acquired on cloudy days should
   show no directional dropout. This isolates the solar effect cleanly.

3. **Different materials**: clay tile (ρ~0.25) vs slate (ρ~0.12) — the higher
   reflectance should show less dropout (stronger signal, better SNR).

4. **Multi-return analysis**: are second returns preferentially lost on dropout
   planes? This would confirm the SNR threshold mechanism.

5. **Investigate the absolute S/N paradox**: the standard link budget says
   dropout shouldn't happen. Either our sensor parameters are very wrong, or
   the mechanism isn't continuous background. Riegl waveform data (if
   accessible) would show the failed detections directly.

6. **Reverse-engineer pulse energy from intensity**: the measured intensity
   at known geometry (north-facing, solar=0) constrains the product
   Ep × ρ × A_recv. Combined with the dropout threshold, this constrains
   the noise level needed to cause dropout.

---

## Code and data locations

| Item | Path |
|------|------|
| Beam incidence step | `lib/pipeline/steps/beam-incidence.js` |
| Plane slug utility | `lib/pipeline/plane-slug.js` |
| Ghost SE points | `test-cases-v2/dahouet-ghost-se-points.json` |
| Static analysis page | `ui/public/dahouet-analysis.html` |
| Analysis data JSON | `ui/public/dahouet-planes-analysis.json` |
| .bin format (v2, Float32 angles + Uint16 intensity) | `lib/test-case-binary.js` |
| Overnight experiment scripts | `/tmp/fresnel-experiment*.js` |
| Literature review | `/tmp/lidar-density-literature-review.md` |
| Fresnel experiments | `/tmp/fresnel-brewster-experiments.md` |
| Sigmoid experiments | `/tmp/sigmoid-oren-nayar-experiments.md` |
| Intensity model fit | `/tmp/intensity-model-fit.md` |
| Intensity distributions | `/tmp/intensity-distributions.md` |

---

## Key references from literature

- Wagner (2010): radiometric calibration model, backscatter cross-section
- Jutzi & Gross (2009): intensity correction, deviations from Lambert at >60°
- Kaasalainen et al. (2011): dark materials show stronger-than-Lambert falloff
- Mandlburger et al. (2015): south-facing roofs show lower density (Austrian ALS)
- Pfennigbauer & Ullrich (2010, Riegl): ~1nm bandpass, ~60dB solar rejection
- Hopkinson (2007): detection threshold causes faster-than-geometric density drop
- **No published study models point density as f(solar noise) for buildings**
