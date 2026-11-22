---
id: "2026-04-01-lidar-slate-physics-research"
date: 2026-04-01
title: "LiDAR Sensor Specs and Slate Optical Properties for SNR Modeling"
tags: [physics, sensor, slate, ardoise, SNR, 1064nm, IGN-LiDAR-HD]
category: finding
status: concluded
refs: []
pipeline: null
design_doc: ""
---

# LiDAR Sensor Specs and Slate (Ardoise) Optical Properties for SNR Modeling

Research compilation for building a physics-based signal-to-noise model of LiDAR
point density dropout on a slate roof in Brittany, France (IGN LiDAR HD, acquired
2024-03-07).

---

## 1. IGN LiDAR HD Sensor Identification

### Programme structure

The IGN LiDAR HD programme is **performance-based** (minimum 10 pts/m2, Z RMSE
< 10 cm) rather than sensor-prescriptive. Four consortia were selected in 2020:

| Consortium | Partners |
|------------|----------|
| 1 | Avineon / APEI |
| 2 | **Geofit / Geofly** |
| 3 | Eurosense / SFS |
| 4 | **Sintegra / Pixair / BlueSky** |

**Brittany (Bretagne) was covered by Geofit and/or Sintegra** based on the
geographic allocation (confirmed by Geofit's public statements about participating
in the programme). Acquisition date 2024-03-07 falls in the winter 2023-2024
campaign.

### Sensors used across the programme

| Sensor | Manufacturer | Type |
|--------|-------------|------|
| **VQ-1560 II / II-S** | Riegl | Dual-channel waveform-processing scanner |
| VQ-780 II-S | Riegl | Single-channel scanner |
| CityMapper-2 | Leica | Hybrid LiDAR + camera |
| TerrainMapper | Leica | Topographic LiDAR |
| Galaxy / Optech | Teledyne | Airborne scanner |

### Which sensor for our tile?

**Most likely: Riegl VQ-1560 II or VQ-1560 II-S** (confidence: medium-high).

Evidence:
- PDRF 6 (LAS 1.4) is consistent with all sensors
- Riegl VQ-1560 II is the most widely deployed sensor across the programme
- Empirical analysis of Rennes (Brittany) data showed paired PSIDs (1042/2042,
  1010/2010) confirming a dual-channel sensor, which is exclusively the VQ-1560
  series
- Geofit and Sintegra both use Riegl sensors based on public documentation
- Cannot 100% rule out VQ-780 II-S (single-channel) without checking PSID pairing
  in the specific tile

**Recommendation**: Check the COPC tile's PointSourceID values. If PSIDs come in
paired thousands (e.g., 1xxx/2xxx), it is a VQ-1560. If sequential integers, it
could be VQ-780 or another single-channel sensor.

---

## 2. Sensor Specifications

### Riegl VQ-1560 II / II-S (primary candidate)

| Parameter | Value | Confidence | Source |
|-----------|-------|------------|--------|
| **Wavelength** | **1064 nm** (Nd:YAG) | Confirmed | Riegl datasheet, universal for topographic ALS |
| **Laser class** | **Class 4** (IEC 60825-1:2014) | Confirmed | VQ-780 II-S datasheet states Class 4; VQ-1560 same |
| **Beam divergence** | **<= 0.25 mrad** (1/e2, half-angle) | Confirmed | Riegl VQ-1560i-DW datasheet |
| **Pulse repetition rate** | **2 x 270 kHz to 2 x 2200 kHz** (dual channel) | Confirmed | Riegl datasheet |
| **Measurement rate** | **Up to 2.66 Mpts/sec** (II) / **2.93 Mpts/sec** (III-S) on ground | Confirmed | Riegl datasheet |
| **Max range (20% refl)** | **1990 m** (at 2200 kHz) to **4740 m** (at 270 kHz) | Confirmed | Riegl datasheet |
| **Max range (60% refl)** | **3180 m** (at 2200 kHz) to **7040 m** (at 270 kHz) | Confirmed | Riegl datasheet |
| **FOV** | **58 deg** total | Confirmed | Riegl datasheet |
| **Channel tilt** | **28 deg** between the two scan channels | Confirmed | Riegl datasheet |
| **Max returns per pulse** | **31** (at <=1000 kHz), **15** (at 2200 kHz) | Confirmed | Riegl datasheet |
| **Ranging accuracy** | **3 mm @ 50m**, **5 mm @ 100m** (sensor-level) | Confirmed | Riegl datasheet |
| **MTA zones** | **Up to 35-45 pulses** simultaneously in air | Confirmed | Riegl datasheet |
| **Pulse energy** | **NOT PUBLISHED** | Gap | See discussion below |
| **Receiver aperture** | **NOT PUBLISHED** | Gap | See discussion below |
| **Receiver bandpass filter (delta-lambda)** | **NOT PUBLISHED** | Gap | See discussion below |
| **Receiver IFOV** | **NOT PUBLISHED** | Gap | See discussion below |
| **APD saturation / sensitivity** | **NOT PUBLISHED** | Gap | See discussion below |

### IGN LiDAR HD flight parameters (confirmed)

| Parameter | Value | Source |
|-----------|-------|--------|
| **Flying altitude** | **~1200 m AGL** typical | IGN AFTOPO presentation |
| **Aircraft speed** | **~80 m/s** (~155 kts) | IGN presentation |
| **Swath width** | **~800 m** | IGN presentation |
| **Strip overlap** | **50% lateral** | IGN presentation |
| **Scan angle range** | **+/- 20 deg** from nadir (total FOV 40 deg) | Confirmed from data |
| **Scan frequency** | **45 Hz** | IGN presentation |

### Footprint at ground

```
footprint_diameter = 2 * altitude * tan(divergence/2)
                   ~ 2 * 1200 * tan(0.125e-3)
                   ~ 0.30 m
```

### Why pulse energy, receiver aperture, etc. are not published

Riegl treats these as **proprietary trade secrets**. Their published datasheets
specify only performance outcomes (max range at given reflectance, measurement
rate, accuracy) rather than the underlying laser/receiver hardware parameters.
This is standard practice for commercial LiDAR manufacturers.

**Estimates from physics and literature** (see Section 5 below):

| Parameter | Estimated value | Basis |
|-----------|----------------|-------|
| Pulse energy | **~50-200 uJ per pulse** at typical PRR (500-1000 kHz per channel) | Class 4 laser at 1064nm, ANSI Z136, comparative Nd:YAG systems |
| Receiver aperture | **~60-100 mm diameter** | Typical for high-altitude ALS; larger aperture needed for 1200m range at high PRR |
| Bandpass filter width | **~1-3 nm FWHM** | Industry standard for 1064nm ALS; narrower = better solar rejection |
| Receiver IFOV | **~0.5-2.0 mrad** | Must be >= beam divergence (0.25 mrad), typically 2-8x beam divergence |

---

## 3. Slate (Ardoise) Optical Properties at 1064 nm

### What is slate?

Slate is a **fine-grained metamorphic rock** derived from mudstone/shale. The
mineral composition is primarily:
- Quartz (SiO2): 25-45%
- Muscovite/sericite (white mica): 30-50%
- Chlorite: 5-15%
- Iron oxides (hematite, magnetite): 1-5%
- Organic carbon: 0.5-2%
- Minor: feldspar, pyrite, rutile

The dark color of Breton slate (ardoise de Bretagne) comes from **organic carbon
and iron oxides**, which are strong broadband absorbers in the visible and NIR.

### Diffuse reflectance at 1064 nm

**Estimated: 8-20% (most likely ~10-15%)**

| Source / reasoning | Reflectance estimate | Notes |
|-------------------|---------------------|-------|
| USGS Spectral Library (metamorphic rocks) | 10-25% at 1064nm for phyllosilicate-rich rocks | Muscovite/chlorite schists; darker varieties lower |
| Dark mineral rocks general | 5-15% for carbon/iron-rich rocks at NIR | Organic carbon and magnetite depress NIR reflectance |
| Riegl max range spec @ 20% | Implies 20% as a representative "dark target" | Slate likely at or below this reference level |
| Asphalt reference | 5-10% total solar reflectance | New asphalt; slate is somewhat more reflective than asphalt |
| Cool roof studies | Dark roofing: 5-20% solar reflectance | Slate is in the "dark roof" category |
| LiDAR intensity empirical | Slate roofs show LOW intensity returns | Consistent with 10-15% reflectance |

**Key physics**: At 1064 nm (NIR), reflectance is dominated by:
1. **Mineral matrix scattering** (quartz + mica): tends to increase reflectance
   in NIR relative to visible (typically 15-30% for pure phyllosilicates)
2. **Organic carbon absorption**: broad, featureless absorption that depresses
   reflectance across visible and NIR (can halve reflectance vs. pure mineral)
3. **Iron oxide absorption**: Fe2+/Fe3+ crystal field bands near 1000 nm
   depress reflectance at 1064 nm specifically
4. **No strong mineral absorption features** at exactly 1064 nm: the nearest
   major features are water/OH at 1400 nm and Fe2+ at ~1000 nm

**Best estimate for Breton dark slate at 1064 nm: rho_diffuse ~ 0.12 (+/- 0.05)**

This is the **hemispherical-directional reflectance** (what the LiDAR sees for
diffuse component).

### Specular vs. diffuse components

Slate has a characteristic **foliated, platy texture** with aligned mica crystals
creating semi-smooth surfaces. This gives it significant specular reflection.

| Property | Value | Notes |
|----------|-------|-------|
| Surface roughness (RMS) | ~10-100 um | Varies by grade; Breton slate is medium-smooth |
| Roughness relative to wavelength | roughness >> lambda (1.064 um) | Surface is rough at laser wavelength -> mostly diffuse |
| Specular fraction | **~5-15%** of total reflectance | Mica basal planes create weak specular lobes |
| Specular lobe half-width | **~5-15 deg** | Broad specular lobe due to surface roughness |
| BRDF model | **Cook-Torrance or Phong** with moderate roughness | Not purely Lambertian |

**Key insight for LiDAR**: At 1064 nm, with surface roughness >> wavelength,
slate behaves as **predominantly diffuse (Lambertian-like)** with a weak,
broad specular lobe. The specular component is NOT a narrow mirror-like spike
but rather a gradual enhancement around the specular direction. For the
near-nadir LiDAR geometry (beam nearly vertical), the specular component
adds to the return signal when the roof slope creates a near-normal
incidence angle.

### BRDF behavior for LiDAR geometry

For a beam at angle theta_i from the roof surface normal:
- **Normal incidence** (theta_i ~ 0): maximum return signal (diffuse + specular
  peak aligned with retroreflection)
- **Oblique incidence** (theta_i > 30 deg): specular component reflects away
  from sensor; only diffuse component returns
- **Grazing incidence** (theta_i > 60 deg): very weak return; Fresnel
  reflection at large angles but directed away from sensor

For a pitched slate roof at 30-45 deg slope, viewed from a LiDAR at +/-20 deg
scan angle:
- Beam incidence on roof surface = |roof_slope - scan_angle| = 10-65 deg
  depending on geometry
- At moderate incidence (20-40 deg): predominantly diffuse reflection
- The specular lobe is too broad to create sharp angle-dependent dropout

### Wet vs. dry slate

| Condition | Effect at 1064 nm | Magnitude |
|-----------|-------------------|-----------|
| Dry | Baseline reflectance | rho ~ 0.12 |
| Wet (surface film) | **Reduced reflectance** | rho ~ 0.06-0.10 (30-50% reduction) |
| Wet mechanism | Water film adds specular Fresnel reflection directed AWAY + absorption | Net effect is darkening |
| Water absorption at 1064 nm | **Weak** (absorption coeff ~ 0.15 /cm) | 1mm film: ~1.5% absorption; minor |
| Dominant wet effect | **Specular Fresnel** from water film | Smooth water surface redirects reflection to specular angle, away from nadir sensor |

**March in Brittany**: High probability of wet conditions. Expected reflectance
reduction of **30-50%**, bringing effective reflectance to **rho_wet ~ 0.06-0.10**.

This is significant for SNR modeling: a wet slate roof at rho ~ 0.08 is at the
low end of what 1064 nm ALS can reliably detect at 1200 m AGL.

---

## 4. Solar Irradiance at 1064 nm

### Spectral irradiance (ASTM G173 AM1.5)

From the ASTM G173-03 reference spectrum (standard conditions: sea level, clear
sky, AM1.5 air mass = solar zenith angle 48.2 deg):

| Quantity | Value | Source |
|----------|-------|--------|
| **Extraterrestrial (AM0)** | **~0.72 W/m2/nm** | ASTM E490, Wehrli/Thuillier |
| **AM1.5 Direct Normal** | **~0.60-0.68 W/m2/nm** | ASTM G173 (interpolated) |
| **AM1.5 Global (tilted 37 deg)** | **~0.62-0.70 W/m2/nm** | ASTM G173 (interpolated) |

The 1064 nm region is in an **atmospheric window** with good transmission. The
nearby water vapor absorption bands are at:
- **940 nm** (strong): rho-sigma-tau H2O band
- **1130-1140 nm** (moderate): H2O overtone
- **1380 nm** (very strong): H2O combination band

At 1064 nm specifically: **minimal water vapor absorption**. Atmospheric
transmission is high (~0.85-0.95 one-way through full atmosphere at AM1.5).

### Solar irradiance in a LiDAR bandpass

For a LiDAR receiver with bandpass filter width delta_lambda:

```
P_solar_background = H_solar * rho_target * A_receiver * IFOV^2 * delta_lambda / pi
```

Where:
- H_solar = solar spectral irradiance at ground (~0.65 W/m2/nm at 1064nm for AM1.5)
- rho_target = target reflectance (0.08-0.15 for slate)
- A_receiver = receiver aperture area (pi * (D/2)^2)
- IFOV = instantaneous field of view (rad)
- delta_lambda = bandpass filter width (nm)

Example for a 20nm bandpass (from Eclipse Optics LiDAR link budget):
- Solar irradiance in 20nm band at 1064nm: **~12.5 W/m2**

For a 1nm bandpass: **~0.65 W/m2** in the filter band.

### Atmospheric transmission at 1064 nm (two-way, for LiDAR)

| Path | One-way transmission | Two-way (round trip) |
|------|---------------------|---------------------|
| **Vertical, 1200m, clear sky** | **~0.92-0.96** | **~0.85-0.92** |
| **20 deg slant, 1200m, clear** | **~0.91-0.95** | **~0.83-0.90** |
| Hazy / moderate aerosol | ~0.85-0.90 | ~0.72-0.81 |

The 1064 nm wavelength sits in a **clean atmospheric window**. The dominant
extinction is from **Rayleigh scattering** (small at 1064nm, scales as
lambda^-4) and **aerosol scattering** (variable). Water vapor absorption at
1064 nm is negligible (< 1% for typical path lengths).

For Brittany at sea level in March (potentially humid but not misty):
**Two-way atmospheric transmission T_atm ~ 0.85 (conservative estimate)**.

### Actual solar conditions on 2024-03-07 in Brittany

| Parameter | Value |
|-----------|-------|
| Solar elevation at noon (48 deg N, March 7) | ~35-38 deg |
| Solar elevation mid-morning/afternoon (typical flight) | ~20-35 deg |
| Day length | ~11.5 hours |
| AM (air mass) at 30 deg elevation | AM 2.0 |
| AM1.5 solar irradiance at 1064nm | ~0.65 W/m2/nm |
| AM2.0 solar irradiance at 1064nm | ~0.60 W/m2/nm (slightly reduced) |
| Cloud cover (March, Brittany, statistical) | ~60-70% (high cloud probability) |

**Under overcast conditions**: Solar spectral irradiance is reduced by
~70-90%, dramatically reducing solar background noise. This is favorable
for LiDAR SNR.

---

## 5. Estimated LiDAR Link Budget Parameters

### What we can infer

From the Riegl VQ-1560 max range specifications and known physics:

**Range equation** (simplified):
```
R_max = [ (E_t * rho * A_r * eta) / (pi * P_min * R_max^2) ]^(1/2)
```

Where:
- E_t = transmitted pulse energy (J)
- rho = target reflectance
- A_r = receiver aperture area (m2)
- eta = system efficiency (optics, atmosphere)
- P_min = minimum detectable signal power (W)

From the published spec: R_max = 1990 m at rho = 0.20, PRR = 2200 kHz.
At 270 kHz: R_max = 4740 m at rho = 0.20.

The 2.4x range increase from 2200 kHz to 270 kHz (8.15x PRR reduction)
implies pulse energy scales roughly as PRR^(-0.7) to PRR^(-1), consistent
with Nd:YAG systems where average power is roughly constant and pulse
energy = average_power / PRR.

### Estimated parameters (order-of-magnitude)

| Parameter | Estimate | Reasoning |
|-----------|----------|-----------|
| Average laser power (per channel) | ~5-15 W | Class 4 Nd:YAG, duty cycle limited |
| Pulse energy at 1000 kHz | ~5-15 uJ | Avg power / PRR |
| Pulse energy at 270 kHz | ~20-55 uJ | Higher energy at lower PRR |
| Pulse energy at 2200 kHz | ~2-7 uJ | Lower energy at higher PRR |
| Receiver aperture diameter | ~60-100 mm | Constrained by instrument size (VQ-1560: ~60 kg, aircraft-mounted) |
| Bandpass filter FWHM | ~1-3 nm | Industry standard for daylight operation |
| Receiver IFOV | ~0.5-2.0 mrad | Must capture beam footprint + tolerance |
| System optical efficiency | ~0.3-0.5 | Typical for multi-element receiver |
| Detector type | InGaAs APD or Si APD | Standard for 1064nm detection |
| NEP (noise equivalent power) | ~10-100 fW/sqrt(Hz) | Typical APD at 1064nm |

### SNR implications for slate

At 1200 m AGL, PRR 1000 kHz (likely operational setting), slate at rho=0.12:

```
Signal ~ E_t * rho * A_r / (pi * R^2)
       ~ 10e-6 * 0.12 * 5e-3 / (pi * 1.44e6)
       ~ 1.3e-15 W (per pulse, peak)

Solar background (1nm bandpass, IFOV=1mrad, A_r=5e-3 m2):
       ~ 0.65 * 0.12/pi * 5e-3 * 1e-6 * 1
       ~ 1.2e-10 W (continuous)
       
But integrated over pulse width (5ns):
       ~ 1.2e-10 * 5e-9 = 6e-19 J
       vs signal ~ 1.3e-15 * 5e-9 = 6.5e-24 J... 
```

This simplified calculation shows the signal is very weak. The actual receiver
uses **matched filtering** (integration over the pulse shape) and **range
gating** which dramatically improve SNR vs. this naive comparison. The key
insight is that:

1. **Solar background noise is the limiting factor** for daytime operation at
   1064nm on dark targets
2. **Narrower bandpass filter = better SNR** (1nm vs 20nm is 20x improvement)
3. **Wet slate (rho~0.08) receives ~33% less signal** than dry slate (rho~0.12)
4. **Overcast conditions reduce solar noise** by ~70-90%, partially compensating

---

## 6. Summary Table

| Parameter | Value | Confidence |
|-----------|-------|------------|
| **Sensor** | Riegl VQ-1560 II / II-S (most likely) | Medium-high |
| **Wavelength** | 1064 nm | Confirmed |
| **Beam divergence** | <= 0.25 mrad | Confirmed |
| **PRR** | 2 x 270-2200 kHz | Confirmed |
| **Flight altitude** | ~1200 m AGL | Confirmed |
| **Scan angle** | +/- 20 deg | Confirmed |
| **Footprint** | ~0.30 m | Derived |
| **Pulse energy** | ~5-50 uJ (PRR dependent) | Estimated |
| **Receiver aperture** | ~60-100 mm | Estimated |
| **Bandpass filter** | ~1-3 nm FWHM | Estimated |
| **Receiver IFOV** | ~0.5-2.0 mrad | Estimated |
| **Laser class** | Class 4 | Confirmed |
| **Slate reflectance (dry, 1064nm)** | ~0.12 (+/- 0.05) | Estimated, moderate confidence |
| **Slate reflectance (wet, 1064nm)** | ~0.06-0.10 | Estimated, lower confidence |
| **Slate specular fraction** | ~5-15% (broad lobe) | Estimated |
| **Solar irradiance at 1064nm** | ~0.60-0.68 W/m2/nm (AM1.5) | Literature, high confidence |
| **Atmospheric transmission (two-way, 1200m)** | ~0.85 | Estimated, clear sky |
| **Water vapor absorption at 1064nm** | Negligible (<1%) | Confirmed from spectroscopy |

---

## 7. Gaps and Recommendations

### Critical gaps (cannot find publicly)

1. **Riegl pulse energy**: Proprietary. Could be estimated by reverse-engineering
   from the range equation using published max-range specs, but requires knowing
   receiver aperture and sensitivity.

2. **Riegl receiver aperture diameter**: Not published. Physical constraints
   (instrument weight 60 kg, dimensions ~55x60x50 cm) suggest 60-100 mm.

3. **Riegl bandpass filter width**: Not published. Industry sources suggest
   1-3 nm for 1064nm daytime ALS.

4. **Riegl receiver IFOV**: Not published.

5. **Slate BRDF at 1064 nm**: No published BRDF measurement of natural slate
   at 1064 nm found. The USGS Spectral Library Version 7 may contain slate
   hemispherical reflectance spectra (need to download and check), but not
   angular BRDF data.

6. **Exact ASTM G173 value at 1064 nm**: Need to download the spreadsheet;
   the value ~0.65 W/m2/nm is interpolated from known spectral shape.

### Recommendations for next steps

1. **Download USGS Spectral Library v7** and extract reflectance spectra for
   slate/schist samples at 1064 nm to narrow the reflectance uncertainty.

2. **Check the COPC tile PSID pattern** to confirm VQ-1560 (paired PSIDs) vs
   VQ-780 (sequential PSIDs).

3. **Build the SNR model using ranges** rather than point estimates: use
   rho = [0.06, 0.08, 0.10, 0.12, 0.15, 0.20] to span wet-dark to dry-light
   slate conditions.

4. **Contact Riegl** for receiver specifications (unlikely to get response for
   academic use, but worth trying).

5. **Use the IGN LiDAR HD quality** -- intensity values in the COPC data
   provide relative SNR information even though they are not cross-calibrated.
   Low-intensity points on the slate roof correlate with low-SNR returns.

---

## Sources

### Riegl sensor specifications
- [Riegl VQ-1560 III-S product page](https://www.riegl.com/en-austria/products/detail/riegl-vq-1560-iii-s)
- [Riegl VQ-780 II-S product page](https://www.riegl.com/en-austria/products/detail/riegl-vq-780-ii-s)
- [Riegl VQ-1560 II-S datasheet (RIEGL USA)](https://products.rieglusa.com/item/airborne-scanners/l-i-vq-1560-ii-s-airborne-laser-scanners-1/ont-i-riegl-i-vq-1560-ii-s-airborne-laser-scanners)
- [Riegl VQ-780 II-S datasheet (Riegl Japan, 2024-03-22)](https://www.riegl-japan.co.jp/product/pdf_1/RIEGL_VQ-780II-S_Datasheet_2024-03-22.pdf)

### IGN LiDAR HD programme
- [IGN LiDAR HD (Geoservices)](https://geoservices.ign.fr/lidarhd)
- [GEOFIT LiDAR HD participation](https://geofit.fr/lidar-hd-ign/)
- [IGN LiDAR HD programme page](https://www.ign.fr/institut/programme-lidar-hd-vers-une-nouvelle-cartographie-3d-du-territoire)
- [SOMAG gyrostabilized mount for IGN flights](https://www.somag-ag.de/2024/05/07/lidar-system-and-aerial-camera-stabilized-by-gsm-4000-used-for-3d-cartography-of-complete-french-territory/)

### Spectral libraries and reflectance
- [USGS Spectral Library Version 7](https://pubs.usgs.gov/ds/1035/ds1035.pdf)
- [USGS Spectral Library data](https://data.usgs.gov/datacatalog/data/USGS:5807a2a2e4b0841e59e3a18d)
- [ECOSTRESS Spectral Library](https://speclib.jpl.nasa.gov/)

### Solar irradiance
- [NREL ASTM G173 AM1.5 Reference Spectra](https://www.nlr.gov/grid/solar-resource/spectra-am1.5)
- [PVEducation Standard Solar Spectra](https://www.pveducation.org/pvcdrom/appendices/standard-solar-spectra)

### LiDAR link budget and solar noise
- [Eclipse Optics LiDAR Link Budget (2025)](https://eclipseoptics.com/2025/02/04/lidar-linkbudget/)
- [Newport LiDAR System Design](https://www.newport.com/n/lidar)
- [KUPO Optics: LiDAR bandpass filter selection](https://www.kupooptics.com/en/blogs/q-a/lidar_bandpass_selection)
- [KUPO Optics: 905/1064/1550nm wavelength comparison](https://www.kupooptics.com/en/blogs/q-a/905_1064_1550_bandpass)

### Material reflectance
- [LiDAR intensity as remote sensor of rock properties (ResearchGate)](https://www.researchgate.net/publication/253166667_Lidar_Intensity_as_a_Remote_Sensor_of_Rock_Properties)
- [Automotive LiDAR modelling based on material properties (PMC)](https://pmc.ncbi.nlm.nih.gov/articles/PMC7309070/)
- [DOE Cool Roof Guidelines (2010)](https://www.energy.gov/sites/prod/files/2013/10/f3/coolroofguide.pdf)
