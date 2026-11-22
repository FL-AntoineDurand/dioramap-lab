---
id: "2026-04-01-overnight-research-compilation"
date: 2026-04-01
title: "Overnight Research: LiDAR Density Dropout Models — Literature + Experiments"
tags: [physics, sensor, SNR, specular, density-model, literature-review, experiments]
category: finding
status: concluded
refs: ["2026-04-01-lidar-point-density-physics-model", "2026-04-01-lidar-slate-physics-research"]
---

# Overnight Research Compilation

Three parallel research tracks ran overnight:
1. Literature review (30+ references)
2. Fresnel/Brewster/Cook-Torrance experiments (5 model variants)
3. Sigmoid detection + Oren-Nayar experiments (4 model variants)

Full results: `/tmp/lidar-density-literature-review.md`, `/tmp/fresnel-brewster-experiments.md`,
`/tmp/sigmoid-oren-nayar-experiments.md`

---

## Literature Review — Key Findings

### The problem is known but not modeled
- Mandlburger et al. (2015): "south-facing roof surfaces consistently show lower
  point density than north-facing" in Austrian ALS data — attributed to
  "unfavorable sun-target-sensor geometry"
- Kashani et al. (2015): "solar background noise is rarely modeled explicitly in
  point cloud processing" — the community ignores it
- **No published study directly models point density as a function of solar noise
  for building roofs.** This is a gap in the literature.

### Physics foundation confirmed
- Wagner (2010): complete radiometric model for ALS, backscatter cross-section
  includes cos(θ) for both illuminated area and projected solid angle
- Jutzi & Gross (2009): intensity correction requires both 1/R² and 1/cos(θ),
  with empirical deviations from Lambertian at θ > 60°
- Kaasalainen et al. (2011): dark materials show **stronger-than-Lambertian
  falloff** at high incidence (20-40% more loss than cos(θ) predicts at θ > 65°)

### Solar noise specifics
- Thiel & Wehr (2004): solar noise reduces effective range 15-30% in bright conditions
- Vain et al. (2009): up to 10-15% intensity variation between morning/midday flights;
  low-reflectance targets (ρ < 0.1) most vulnerable
- Baltsavias (1999): recommends flights in low-sun or overcast for optimal performance
- Gatziolis & Andersen (2008): "solar noise most pronounced when sun angle is high
  and flight direction places sun near scanner FOV"

### Riegl VQ-1560 II specifics
- Pfennigbauer & Ullrich (2010, Riegl engineers): ~1nm bandpass + 5-10ns temporal
  gating = ~60dB solar rejection, but residual noise affects low-ρ targets
- Dual-channel scanner (forward + aft): each channel sees different solar geometry
  → complex interaction patterns

### Slate at 1064nm
- ρ ~ 0.05-0.15 (among darkest roofing materials)
- Clay tile: 0.15-0.35, concrete: 0.20-0.40 — 2-4× more reflective
- Slate is in the most vulnerable regime for solar noise dropout

### Key recommendation from literature
Detection is a **binary threshold** (point detected or not), not a gradual reduction.
Model should use sigmoid/step function of SNR, not linear penalty.

---

## Experiment Results

### Fresnel/Brewster: DEAD END
- Fresnel reflectance varies only 4-9% across 30-60° incidence range for slate (n=1.5)
- Cosine geometric dilution is a 50% effect over same range
- Adding Fresnel improves RMSE by < 0.01
- Even at n=10 (unrealistic), Fresnel at 36.6° only reflects 67% — can't explain ghost_SE
- **Recommendation: do NOT add Fresnel terms**

### SNR-Subtractive Solar Model: BEST RESULT
The key physics insight from the experiments:

```
D = D₀ × cos(θ)^α × max(0, 1 - k_s × solar_lobe(bisect, σ) / cos(θ)^β)
```

The `1/cos(θ)^β` term is the breakthrough: **solar noise is constant at the detector,
but the return signal weakens with incidence angle.** The penalty should scale
inversely with signal strength, not be independent of it.

Best fit (per-building D₀, 14 points):
- α = 0.30, k_s = 0.72, σ = 23.6°, β = 2.12
- **RMSE = 1.95 pts/m²**

Single-D₀ fit (12 points, excluding ghost_SE):
- α = 0.50, k_s = 43.0, σ = 31°, β = 4.0
- **RMSE = 5.15 pts/m²**

### ghost_SE: ANOMALY
All models (Fresnel, Cook-Torrance, SNR-subtractive, sigmoid) predict 7-18 pts/m²
for ghost_SE where actual is ~0. The plane has:
- Moderate incidence (37°/54°)
- Moderate solar (0.69-0.70)
- Moderate bisector (37-38°)

No combination of beam physics, solar noise, and specular reflection can explain
near-zero density on BOTH strips. Must be an external factor:
- Physical obstruction from adjacent building/vegetation
- Local surface condition anomaly
- Coverage gap in flight path

---

## Revised Model Recommendation

### For production use (practical)

```
D = D₀ × cos(θᵢ)^α × max(0, 1 - k_s × G(α_bisect, σ) / cos(θᵢ)^β)

where:
  G(α, σ) = exp(-α² / (2σ²))     Gaussian specular lobe
  α_bisect = angle(normal, bisector(toSensor, toSun))
  
Parameters (from dahouet ardoise fit):
  D₀ ≈ 20 pts/m² (building-dependent)
  α  ≈ 0.3-0.5
  k_s ≈ 0.7-43 (depends on D₀ formulation)
  σ  ≈ 24-31°
  β  ≈ 2.1-4.0
```

### For research (more physically grounded)

Replace `max(0, 1-penalty)` with sigmoid detection probability:
```
SNR_eff = cos(θ)^α × (1 - Fresnel(θ)) / (k_d × solar_illum + k_s × specular_lobe + noise_floor)
D = D₀ × sigmoid(steepness × (SNR_eff - threshold))
```

This models the actual physics: each pulse has a probability of detection
determined by SNR. The sigmoid creates sharp transitions from "all detected"
to "none detected" around the threshold — matching the observed binary
dropout behavior.

### Sigmoid Detection + Oren-Nayar Experiments

| Model | RMSE-12 | RMSE-14 | ghost_SE pred | Verdict |
|-------|---------|---------|---------------|---------|
| Oren-Nayar | 6.62 | 8.23 | 16.7/12.3 | Worse — irrelevant for LiDAR |
| Sigmoid(cos·Fresnel - k·solar) | 5.46 | 7.21 | 16.4/10.3 | No ghost help |
| Full SNR/noise sigmoid | 5.04 | 7.04 | 18.0/8.5 | Marginal |
| Pure Fresnel + sigmoid | 7.14 | 8.58 | 15.3/13.8 | Much worse |
| **Sigmoid + per-building albedo** | **2.69** | **2.49** | **0.2/0.1** | **Best fit** (overfitted) |

#### Oren-Nayar: DEAD END for monostatic LiDAR
The roughness correction term B vanishes identically for backscatter geometry
because cos(φ_r - φ_i) = cos(π) = -1, and max(0, -1) = 0. Oren-Nayar reduces
to `A × cos(θ)` — a slightly attenuated Lambert. Adds nothing.

#### Per-building albedo is the missing variable
The sigmoid + albedo model (experiment 5a) correctly predicts ghost_SE near zero.
Fitted albedos reveal:
- rich_ash: 1.98 (bright), zero_gap: 1.78 (bright), bay_cat: 1.36 (medium)
- lame_fur: 0.14, high_ace: 0.10, west_arc: 0.08, **ghost_SE: 0.08** (very dark)

ghost_SE has the darkest surface. Combined with moderate solar noise → below
detection threshold → ~0 density. The mechanism is physically correct but the
model is overfitted (12 params for 14 points).

**Practical path**: extract albedo proxy from LiDAR return intensity (available
per-point) or NIR imagery. This would make the model predictive rather than
retrospective.

---

### What to try next
1. **Extract per-plane mean intensity from LiDAR returns** — this IS the
   albedo proxy, already in the data (return amplitude/intensity field)
2. **More buildings**: fit on 5-10 buildings across different orientations,
   materials, and acquisition times to break parameter degeneracies
3. **Per-strip analysis on other test cases**: the 220 test cases include
   many real buildings — extract per-strip density for all of them
4. **Overcast vs sunny**: compare buildings acquired on cloudy days
   (solar noise negligible) vs sunny days to isolate the solar effect
5. **Clay tile vs slate**: different reflectance → different vulnerability
   → validate the ρ dependence
6. **Morning vs afternoon**: compare E vs W asymmetry for morning flights
   vs N vs S for afternoon flights
7. **Check if LiDAR intensity is stored**: if so, compute mean intensity
   per plane as the albedo proxy — this may be the key to a predictive model
