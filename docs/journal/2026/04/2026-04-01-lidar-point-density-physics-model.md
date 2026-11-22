---
id: "2026-04-01-lidar-point-density-physics-model"
date: 2026-04-01
title: "LiDAR Point Density Physics Model: Beam Incidence, Solar Noise, Specular Reflection"
tags: [physics, sensor, SNR, beam-angle, solar, specular, density-model, ardoise, IGN-LiDAR-HD]
category: design
status: proposed
refs: ["2026-04-01-lidar-slate-physics-research", "2026-03-31-boundary-next-gen-plan"]
---

# LiDAR Point Density Physics Model

## Context

Point density varies dramatically across roof planes on the dahouet test case
(Pléneuf-Val-André, Brittany). Some planes show 35:1 PSID ratio (SW-facing, P16
nearly absent). Investigation revealed three interacting physical mechanisms:
beam incidence angle, diffuse solar illumination, and specular solar reflection
toward the sensor.

## Acquisition conditions

| Parameter | Value | Source |
|-----------|-------|--------|
| Date | 2024-03-07 | GPS time in COPC |
| Time (UTC) | P16: 12:49, P15: 12:54 | Per-strip GPS time |
| Weather | "Passing clouds", partly sunny | timeanddate.com Saint-Brieuc |
| Temperature | 10-11°C, humidity 58-67% (dropping) | timeanddate.com |
| Visibility | 16 km (clear) | timeanddate.com |
| Roof material | Ardoise (Breton dark slate) | User knowledge (IRL) |
| Roof condition | Likely dry or drying (midday sun, humidity dropping) | Inference |

## Sensor

| Parameter | Value | Source |
|-----------|-------|--------|
| Sensor | Riegl VQ-1560 II or II-S (dual channel) | IGN programme + PSID pattern |
| Wavelength | 1064 nm (Nd:YAG) | Riegl datasheet |
| Beam divergence | ≤ 0.25 mrad → footprint ~0.30 m | Riegl datasheet |
| Pulse energy | ~50-200 µJ (at typical PRR) | Estimated (proprietary) |
| Receiver aperture | ~60-100 mm diameter | Estimated (proprietary) |
| Bandpass filter | ~1-3 nm FWHM | Estimated (industry standard) |
| Receiver IFOV | ~0.5-2.0 mrad | Estimated (proprietary) |

## Flight geometry (measured from data)

| Parameter | Value | Method |
|-----------|-------|--------|
| Altitude | ~1250 m AGL | Scan angle triangulation (P15: 1240m, P16: 1253m) |
| Slant range | ~1280 m | altitude / cos(mean_scan_angle) |
| Scan angle range | 11.7° – 15.7° | From data (building at swath edge) |
| Flight direction | N-S (~0°/180°) | Scan angle gradient fit |
| Cross-track | E-W | Perpendicular to flight |
| Strips | 2 (PSID 15 and 16), 5 min apart | GPS time |

### Sun position (per strip)

| Strip | Time UTC | Elevation | Azimuth |
|-------|----------|-----------|---------|
| P16 | 12:49 | 36.0° | 188.7° (SSW) |
| P15 | 12:54 | 35.8° | 190.4° (SSW) |

### Beam direction reconstruction

Scan angle gradient method: fit `scan_angle = g_x·x + g_z·z + c` per strip.
Gradient direction = cross-track. Per-point beam direction:

```
beam = (sin(θ)·ct_x, -cos(θ), sin(θ)·ct_z)
```

Where θ = scan angle (degrees, from COPC API via Float32 in .bin v2),
ct = cross-track unit vector from gradient fit.

## Material properties — Ardoise at 1064 nm

| Property | Value | Source |
|----------|-------|--------|
| Diffuse reflectance (dry) | ~0.12 (±0.05) | USGS spectral library + estimates |
| Diffuse reflectance (wet) | ~0.06-0.10 | 30-50% reduction from water film |
| Specular fraction | ~5-15% of total | Mica basal planes |
| Specular lobe half-width (σ) | ~5-15° | Surface roughness >> λ |
| BRDF model | Cook-Torrance, moderate roughness | Literature |

## Solar irradiance at 1064 nm

| Quantity | Value |
|----------|-------|
| AM1.5 direct normal | ~0.65 W/m²/nm |
| Atmospheric window | Clean at 1064nm (no H₂O absorption) |
| Two-way atmospheric transmission | ~0.85 |

---

## Physics model

### Signal: laser return power

The power received by the sensor from a single pulse reflection:

```
P_return = (P_laser · ρ_total · cos(θ_i) · A_recv · T²_atm) / (π · R²)
```

Where:
- `P_laser` = emitted pulse energy (~50-200 µJ)
- `ρ_total` = total surface reflectance at 1064nm (~0.12 for dry slate)
- `θ_i` = beam-surface incidence angle
- `cos(θ_i)` = geometric dilution (beam spreads over larger area) + Lambert cosine
- `A_recv` = receiver aperture area
- `T_atm` = one-way atmospheric transmission (~0.92)
- `R` = slant range (~1280 m)
- `π` = Lambertian assumption for diffuse component

### Noise: solar background power in receiver

Two components reaching the sensor:

**Diffuse solar noise** (mechanism #2):
```
P_diffuse = E_sun · cos(θ_sun) · ρ_diffuse · Ω_recv · Δλ · A_recv / π
```

Where:
- `E_sun` = solar spectral irradiance at 1064nm (~0.65 W/m²/nm)
- `θ_sun` = angle between plane normal and sun direction
- `cos(θ_sun)` = how much sun illuminates the surface = `dot(normal, toSun)`
- `ρ_diffuse` = diffuse reflectance component
- `Ω_recv` = receiver solid angle (IFOV²)
- `Δλ` = bandpass filter width (nm)

**Specular solar noise** (mechanism #3):
```
P_specular = E_sun · cos(θ_sun) · ρ_specular · BRDF(α) · Ω_recv · Δλ · A_recv
```

Where:
- `α` = angle between plane normal and bisector of (sun direction, sensor direction)
- `BRDF(α) = exp(-α² / (2σ²))` — Gaussian specular lobe
- `σ` = specular lobe half-width (~5-15°)
- When α → 0°: normal IS the bisector → perfect specular reflection into sensor

### Detection condition

A return is detected when:

```
SNR = P_return / (P_diffuse + P_specular + P_detector_noise) > SNR_threshold
```

Or equivalently, the sensor saturates when:

```
P_diffuse + P_specular > P_saturation
```

### Effective density model

Lumping unknown hardware constants into effective parameters:

```
D(strip, plane) = D₀ · cos(θ_i) · max(0, 1 - k_d · S_diffuse - k_s · S_specular)
```

Where:
- `D₀` = baseline density at normal incidence, no solar (pts/m²)
- `θ_i` = beam-surface incidence angle
- `cos(θ_i)` = geometric + Lambert factor (mechanism #1)
- `S_diffuse = max(0, dot(normal, toSun))` = solar illumination (mechanism #2)
- `S_specular = exp(-α² / (2σ²))` = specular lobe (mechanism #3)
- `α = angle(normal, bisector(toSensor, toSun))` = specular proximity
- `σ` = specular lobe half-width (fit parameter)
- `k_d` = diffuse solar sensitivity (fit parameter)
- `k_s` = specular solar sensitivity (fit parameter)

### Parameters to fit (5 total)

| Parameter | Description | Expected range |
|-----------|-------------|---------------|
| D₀ | Baseline density | 15-30 pts/m² |
| k_d | Diffuse solar penalty | 0-1 |
| k_s | Specular solar penalty | 0-50 (can be large, multiplied by small Gaussian) |
| σ | Specular lobe half-width | 5-20° |
| (α exponent) | Optional: cos(θ_i) exponent | 0.5-2.0 |

---

## Empirical data (7 ardoise planes, 2 strips = 14 data points)

### Per-plane per-strip measurements

| Slug | Dir | Tilt | PSID | Pts | Inc° | cosI | Bisect° | Density |
|------|-----|------|------|-----|------|------|---------|---------|
| rich_ash | NW 312° | 44° | P15 | 518 | 55.0° | 0.573 | 68.3° | 23.3 |
| rich_ash | NW 312° | 44° | P16 | 406 | 36.8° | 0.801 | 56.9° | 18.3 |
| high_ace | N 357° | 44° | P15 | 354 | 59.5° | 0.507 | 78.6° | 11.1 |
| high_ace | N 357° | 44° | P16 | 501 | 32.1° | 0.847 | 64.8° | 15.8 |
| west_arc | NE 42° | 44° | P15 | 277 | 56.2° | 0.556 | 74.8° | 9.4 |
| west_arc | NE 42° | 44° | P16 | 435 | 35.9° | 0.810 | 63.0° | 14.8 |
| lame_fur | NW 312° | 44° | P15 | 331 | 55.2° | 0.571 | 68.4° | 13.3 |
| lame_fur | NW 312° | 44° | P16 | 238 | 36.9° | 0.800 | 57.0° | 9.5 |
| zero_gap | S 177° | 45° | P15 | 188 | 29.9° | 0.867 | 12.4° | 22.8 |
| zero_gap | S 177° | 45° | P16 | 18 | 57.5° | 0.538 | 25.2° | 2.2 |
| bay_cat | SW 223° | 45° | P15 | 177 | 34.7° | 0.822 | 24.0° | 27.0 |
| bay_cat | SW 223° | 45° | P16 | 5 | 54.4° | 0.583 | 28.3° | 0.8 |
| odd_lab | SE 131° | 52° | P15 | 72 | 43.0° | 0.732 | 41.0° | 16.4 |
| odd_lab | SE 131° | 52° | P16 | 38 | 60.4° | 0.494 | 44.3° | 8.6 |

### Additional: undetected SE plane (virtual)

Same azimuth as odd_lab (SE 131°), 45° tilt, near-zero density both strips.
Predicted: P15 inc=36.6° bisect=36.7°, P16 inc=53.8° bisect=38.4°.
Both strips in moderate specular zone + odd_lab is partially shadowed by lame_fur.

### Predicted worst specular planes

| Strip | Worst plane (bisector=0°) |
|-------|--------------------------|
| P15 | S 188°, tilt 35° |
| P16 | SSW 192°, tilt 21° |

### Key observations

1. **Bisector < 30° AND incidence > 50° → dropout** (0.8-2.2 pts/m²)
2. **Bisector < 30° alone → survivable** (zero_gap P15: bisect 12°, inc 30° → 22.8 pts/m²)
3. **Incidence > 55° alone → survivable** (high_ace P15: inc 60°, bisect 79° → 11.1 pts/m²)
4. **Neither alone kills the signal — it's the product**
5. North-facing planes: bisect > 55°, no solar issue regardless of incidence
6. The specular lobe width matters: dropout at bisect 25-28° suggests σ ≈ 15-20°

### Per-plane density anomalies (unexplained by angle/solar)

- rich_ash: 23.3 pts/m² (P15) — highest density, same geometry as lame_fur (13.3)
  → possible strip overlap zone inflating density
- lame_fur P16: 9.5 pts/m² — lower than expected for NW at 37° incidence
  → local point thinning or processing artifact
- west_arc: systematically low density (9.4 / 14.8) vs high_ace (11.1 / 15.8)
  → similar geometry, different area of building — possibly shadowing or local effect

---

## Implementation plan

### Step 1: Compute per-plane per-strip features

For each plane and each PSID strip, compute:
- `θ_i` = mean beam-surface incidence angle
- `S_diffuse = max(0, dot(normal, toSun))` — using per-strip sun position
- `α = angle(normal, bisector(toSensor, toSun))` — specular proximity
- `S_specular = exp(-α² / (2σ²))` — Gaussian lobe value

### Step 2: Fit model parameters

Grid search or least-squares fit of `D₀, k_d, k_s, σ` (and optionally cos exponent)
from the 14 data points. The model is:

```
D_predicted = D₀ · cos(θ_i)^α_exp · max(0, 1 - k_d · S_diffuse - k_s · S_specular)
```

### Step 3: Validate

- Check RMSE and per-point residuals
- Verify the model correctly predicts:
  - Dropout on zero_gap P16 and bay_cat P16
  - Normal density on north-facing planes
  - Reduced density on odd_lab
  - Near-zero on undetected SE plane

### Step 4: Apply to pipeline

- For each plane, predict expected density per strip
- Flag planes where predicted density < threshold as "solar-degraded"
- Use this to:
  - Explain sparse voids (not random — physically determined)
  - Adjust boundary detection confidence on degraded planes
  - Predict which orientations will be problematic before processing

---

## Fitting results — inconclusive

### Best fit (12 points, 6 ardoise planes × 2 strips, ghost_SE excluded)

```
D = 20.5 × cos(θᵢ)^0.5 × max(0, 1 - 43 × exp(-α²/(2×31²)) × (1-cosθᵢ)⁴)
RMSE = 5.15 pts/m²
```

| Parameter | Value | Interpretation |
|-----------|-------|---------------|
| D₀ | 20.5 pts/m² | Baseline density |
| α_exp | 0.5 | Sub-linear cosine (weaker than Lambert) |
| k_d | 0.0 | **No diffuse solar term** — dropped out |
| k_s | 43.0 | Strong specular penalty |
| σ | 31° | Very broad specular lobe |
| β | 4.0 | Sharp grazing interaction (4th power) |

### What the model captures

- **zero_gap P16** (S-facing, grazing): predicted 0.0 ✓ (actual 2.2)
- **bay_cat P16** (SW-facing, grazing): predicted 2.1 ✓ (actual 0.8)
- **North-facing planes**: predicted 13-19, actual 9-16 (reasonable)

### What the model fails on

- **rich_ash P15**: predicted 13.6, actual 23.3 (+9.7 error)
- **bay_cat P15**: predicted 18.0, actual 27.0 (+9.0 error)
- **lame_fur P16**: predicted 18.1, actual 9.5 (-8.6 error)
- **ghost_SE**: predicted 17.8/6.9, actual ~0.2/0.0 (**complete miss**)

### What's missing

The model explains the **dramatic dropout** (S/SW P16 → near zero) through the
specular×grazing interaction, but fails on:

1. **Per-plane density variation**: rich_ash has 2× the density of west_arc despite
   identical geometry. Likely strip overlap zones or local point thinning.

2. **Ghost SE plane**: bisector 37° is outside the specular peak but density is ~0.
   This plane has additional physics we haven't modeled — possibly:
   - **Fresnel reflection at grazing angles**: at high incidence, surface reflects
     laser energy away (not back to sensor) — reduces return signal beyond cos(θ)
   - **Geometric self-shadowing**: micro-roughness of slate blocks light at grazing angles
   - **Physical obstruction**: building geometry creates a coverage blind spot
   - **A 4th reflection mechanism we haven't considered**

3. **Diffuse solar term collapsed to zero**: the model prefers to explain everything
   through the specular lobe (σ=31°, very broad). This may mean the specular and
   diffuse effects are degenerate with only 12 data points — can't separate them.

4. **Only 12 data points from 1 building, 1 acquisition**: severely underdetermined
   for 5 parameters. Need data from other buildings, other orientations, other times
   of day, other materials to break degeneracies.

### Conclusion

The physics is correct (three mechanisms interact) but we can't reliably fit the
constants with this dataset alone. The model is useful qualitatively (predicts which
planes will have dropout) but not quantitatively. More buildings needed for calibration.

The ghost SE plane suggests a mechanism beyond our current model. Candidates:
Fresnel reflection (angle-dependent surface reflectance), geometric self-shadowing
(Cook-Torrance masking term), or Brewster angle effects at ~56° for slate.

---

## Fitting code (pseudocode)

```javascript
// Input: array of { incDeg, bisectDeg, solarDot, density } per plane per strip
// Output: best-fit { D0, alpha, kd, ks, sigma, beta }

// Step 1: Precompute
for (const d of data) {
  d.cosI = cos(d.incDeg * PI / 180);
  d.bisectRad = d.bisectDeg * PI / 180;
}

// Step 2: Two-pass grid search
// Coarse pass
for D0 in [10, 12, ..., 35]:
  for alpha in [0.3, 0.6, ..., 2.4]:
    for kd in [0, 0.2, ..., 2.0]:
      for ks in [0, 2, ..., 40]:
        for sigma in [5, 8, ..., 40]:  // degrees
          for beta in [0, 0.5, ..., 4.0]:
            sigmaRad = sigma * PI / 180
            sse = 0
            for each data point d:
              Sspec = exp(-d.bisectRad² / (2 * sigmaRad²))
              Stotal = kd * d.solarDot + ks * Sspec
              grazingAmp = (1 - d.cosI) ^ beta
              predicted = D0 * d.cosI^alpha * max(0, 1 - Stotal * grazingAmp)
              sse += (d.density - predicted)²
            if sse < bestErr: save parameters

// Fine pass: refine around coarse best within ±1 step in each dimension

// Step 3: Evaluate
// For each data point, compute predicted density and residual
// Report RMSE, per-point errors, physical interpretation of parameters

// Step 4: Predict for new planes
// Given plane normal + beam direction + sun direction:
//   θ_i = acos(|dot(beam, normal)|)
//   toSensor = -beam (normalized)
//   toSun = sun direction (normalized, toward sun)
//   bisector = normalize(toSensor + toSun)
//   α = acos(dot(normal, bisector))
//   S_diffuse = max(0, dot(normal, toSun))
//   S_specular = exp(-α² / (2σ²))
//   D_predicted = D0 * cos(θ_i)^alpha * max(0, 1 - (kd*S_diffuse + ks*S_specular) * (1-cos(θ_i))^beta)
```

### What to try next

- Add Fresnel reflection term: `F(θ_i) = ((n-1)/(n+1))² × (1 + (1-cos²θ_i)⁵ / ...)` (Schlick approximation)
- Add Cook-Torrance geometric masking term: `G(θ_i) = min(1, 2·cos(θ_i)·cos(θ_n)/cos(θ_h), ...)`
- Get more data: run on other real test cases with different orientations/sensors/times
- Check if Brewster angle (~56° for slate, n≈1.5) explains the ghost SE P16 (incidence 53.8° ≈ Brewster)
- Try multiplicative model instead of additive penalty: `D = D0 * f(incidence) * g(solar) * h(specular)`

---

## Notation reference

| Symbol | Meaning |
|--------|---------|
| θ_i | Beam-surface incidence angle (0° = normal, 90° = grazing) |
| θ_sun | Sun-surface angle = acos(dot(normal, toSun)) |
| α | Specular proximity = angle(normal, bisector(toSensor, toSun)) |
| σ | Specular lobe half-width (degrees) |
| S_diffuse | max(0, cos(θ_sun)) = solar illumination factor |
| S_specular | exp(-α²/(2σ²)) = specular reflection into sensor |
| D₀ | Baseline density (pts/m²) |
| k_d | Diffuse solar penalty coefficient |
| k_s | Specular solar penalty coefficient |
| R | Slant range sensor-to-surface (~1280m) |
| toSun | Unit vector from surface toward sun |
| toSensor | Unit vector from surface toward sensor = -beam |
