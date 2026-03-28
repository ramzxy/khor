// Visual preset configurations — each music preset shifts the visualization character.

export interface VisualPreset {
  baseSpeed: number         // flow field force multiplier
  turbulenceScale: number   // unused currently, reserved
  trailDecay: number        // unused currently (trails use ring buffer length)
  particleSize: number      // world-space size for gl_PointSize
  burstMultiplier: number   // particles per exec spike
  accentHue: number         // HSL hue for accent color
  noiseFrequency: number    // curl-noise spatial frequency
  driftSpeed: number        // ambient particle rate (0-1)
  particleSpread: number    // spawn radius multiplier
}

const presets: Record<string, VisualPreset> = {
  ambient: {
    baseSpeed: 0.4,
    turbulenceScale: 0.8,
    trailDecay: 0.97,
    particleSize: 0.06,
    burstMultiplier: 0.7,
    accentHue: 190,
    noiseFrequency: 0.5,
    driftSpeed: 0.4,
    particleSpread: 0.8,
  },
  percussive: {
    baseSpeed: 0.9,
    turbulenceScale: 1.4,
    trailDecay: 0.88,
    particleSize: 0.04,
    burstMultiplier: 1.5,
    accentHue: 0,
    noiseFrequency: 1.0,
    driftSpeed: 0.15,
    particleSpread: 0.4,
  },
  arp: {
    baseSpeed: 0.6,
    turbulenceScale: 1.0,
    trailDecay: 0.93,
    particleSize: 0.05,
    burstMultiplier: 1.0,
    accentHue: 270,
    noiseFrequency: 0.8,
    driftSpeed: 0.3,
    particleSpread: 0.5,
  },
  drone: {
    baseSpeed: 0.25,
    turbulenceScale: 0.5,
    trailDecay: 0.985,
    particleSize: 0.08,
    burstMultiplier: 0.4,
    accentHue: 30,
    noiseFrequency: 0.3,
    driftSpeed: 0.2,
    particleSpread: 0.9,
  },
}

export function getPreset(name: string): VisualPreset {
  return presets[name] ?? presets.ambient
}
