// GPU particle system with per-particle trail ring buffers and per-signal colors.
// Each particle carries a "signal source" index that maps to a color in the shader.

import * as THREE from 'three'
import { sampleFlow, type FlowParams } from './flowfield'
import type { KhorSignals } from './signals'
import type { VisualPreset } from './presets'

const TRAIL_LENGTHS = [6, 14, 24]

// Signal source IDs — each gets a distinct color
export const SRC_DRIFT = 0
export const SRC_EXEC = 1
export const SRC_NET = 2
export const SRC_IO = 3
export const SRC_RETX = 4
export const SRC_IRQ = 5
// 6 signal sources total

// Default signal colors (HSL hues mapped to RGB in the engine)
export const SIGNAL_COLORS = [
  [0.75, 0.78, 0.85],  // drift: cool white
  [0.40, 0.85, 1.00],  // exec: cyan
  [0.30, 1.00, 0.45],  // network: green
  [1.00, 0.65, 0.20],  // io: amber/orange
  [1.00, 0.25, 0.30],  // retx: red
  [0.70, 0.40, 1.00],  // irq: purple
]

export interface ParticleSystemOptions {
  maxParticles: number
  trailLength: number
}

export class ParticleSystem {
  private px: Float32Array
  private py: Float32Array
  private vx: Float32Array
  private vy: Float32Array
  private life: Float32Array
  private maxLife: Float32Array
  private brightness: Float32Array
  private source: Uint8Array  // signal source ID per particle
  private alive: Uint8Array

  private trails: Float32Array
  private trailHead: Uint8Array
  private trailLen: number

  private maxParticles: number
  private time = 0

  pointsGeom: THREE.BufferGeometry
  pointsMesh: THREE.Points
  trailGeom: THREE.BufferGeometry
  trailMesh: THREE.LineSegments

  constructor(opts: ParticleSystemOptions) {
    this.maxParticles = opts.maxParticles
    this.trailLen = TRAIL_LENGTHS[opts.trailLength] ?? TRAIL_LENGTHS[1]

    const n = this.maxParticles

    this.px = new Float32Array(n)
    this.py = new Float32Array(n)
    this.vx = new Float32Array(n)
    this.vy = new Float32Array(n)
    this.life = new Float32Array(n)
    this.maxLife = new Float32Array(n)
    this.brightness = new Float32Array(n)
    this.source = new Uint8Array(n)
    this.alive = new Uint8Array(n)

    this.trails = new Float32Array(n * this.trailLen * 2)
    this.trailHead = new Uint8Array(n)

    // ── Points geometry ──
    const posArr = new Float32Array(n * 3)
    const alphaArr = new Float32Array(n)
    const sizeArr = new Float32Array(n)
    const colorArr = new Float32Array(n * 3)

    this.pointsGeom = new THREE.BufferGeometry()
    this.pointsGeom.setAttribute('position', new THREE.BufferAttribute(posArr, 3))
    this.pointsGeom.setAttribute('aAlpha', new THREE.BufferAttribute(alphaArr, 1))
    this.pointsGeom.setAttribute('aSize', new THREE.BufferAttribute(sizeArr, 1))
    this.pointsGeom.setAttribute('aColor', new THREE.BufferAttribute(colorArr, 3))

    const pointsMat = new THREE.ShaderMaterial({
      vertexShader: /* glsl */ `
        attribute float aAlpha;
        attribute float aSize;
        attribute vec3 aColor;
        varying float vAlpha;
        varying vec3 vColor;
        void main() {
          vAlpha = aAlpha;
          vColor = aColor;
          vec4 mv = modelViewMatrix * vec4(position, 1.0);
          gl_PointSize = aSize * (800.0 / -mv.z);
          gl_PointSize = clamp(gl_PointSize, 1.0, 12.0);
          gl_Position = projectionMatrix * mv;
        }
      `,
      fragmentShader: /* glsl */ `
        varying float vAlpha;
        varying vec3 vColor;
        void main() {
          float d = length(gl_PointCoord - vec2(0.5));
          if (d > 0.5) discard;
          float soft = 1.0 - smoothstep(0.1, 0.5, d);
          gl_FragColor = vec4(vColor, vAlpha * soft);
        }
      `,
      transparent: true,
      depthWrite: false,
      blending: THREE.AdditiveBlending,
    })
    this.pointsMesh = new THREE.Points(this.pointsGeom, pointsMat)

    // ── Trail geometry ──
    const maxSegs = n * (this.trailLen - 1)
    const trailPosArr = new Float32Array(maxSegs * 2 * 3)
    const trailAlphaArr = new Float32Array(maxSegs * 2)
    const trailColorArr = new Float32Array(maxSegs * 2 * 3)

    this.trailGeom = new THREE.BufferGeometry()
    this.trailGeom.setAttribute('position', new THREE.BufferAttribute(trailPosArr, 3))
    this.trailGeom.setAttribute('aAlpha', new THREE.BufferAttribute(trailAlphaArr, 1))
    this.trailGeom.setAttribute('aColor', new THREE.BufferAttribute(trailColorArr, 3))

    const trailMat = new THREE.ShaderMaterial({
      vertexShader: /* glsl */ `
        attribute float aAlpha;
        attribute vec3 aColor;
        varying float vAlpha;
        varying vec3 vColor;
        void main() {
          vAlpha = aAlpha;
          vColor = aColor;
          gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
        }
      `,
      fragmentShader: /* glsl */ `
        varying float vAlpha;
        varying vec3 vColor;
        void main() {
          gl_FragColor = vec4(vColor, vAlpha);
        }
      `,
      transparent: true,
      depthWrite: false,
      blending: THREE.AdditiveBlending,
    })
    this.trailMesh = new THREE.LineSegments(this.trailGeom, trailMat)
  }

  // ── Spawning ────────────────────────────────────────────────────────────

  private findDead(): number {
    for (let i = 0; i < this.maxParticles; i++) {
      if (!this.alive[i]) return i
    }
    return -1
  }

  private spawn(x: number, y: number, svx: number, svy: number, ml: number, bright: number, src: number) {
    const i = this.findDead()
    if (i < 0) return
    this.alive[i] = 1
    this.px[i] = x
    this.py[i] = y
    this.vx[i] = svx
    this.vy[i] = svy
    this.life[i] = 0
    this.maxLife[i] = ml
    this.brightness[i] = bright
    this.source[i] = src
    this.trailHead[i] = 0
    const base = i * this.trailLen * 2
    for (let t = 0; t < this.trailLen; t++) {
      this.trails[base + t * 2] = x
      this.trails[base + t * 2 + 1] = y
    }
  }

  spawnExecBurst(signals: KhorSignals, preset: VisualPreset) {
    const count = Math.floor(signals.exec_s * 12 * preset.burstMultiplier)
    if (count <= 0) return
    for (let i = 0; i < count; i++) {
      const angle = Math.random() * Math.PI * 2
      const r = Math.random() * 0.3 * preset.particleSpread
      const speed = (0.3 + Math.random() * 0.6) * preset.baseSpeed
      this.spawn(
        Math.cos(angle) * r, Math.sin(angle) * r,
        Math.cos(angle) * speed, Math.sin(angle) * speed,
        3 + Math.random() * 3,
        0.5 + Math.random() * 0.5,
        SRC_EXEC,
      )
    }
  }

  spawnNetworkParticles(signals: KhorSignals, preset: VisualPreset) {
    const net = (signals.rx_kbs + signals.tx_kbs) * 0.5
    const count = Math.floor(net * 6)
    if (count <= 0) return
    for (let i = 0; i < count; i++) {
      const angle = Math.random() * Math.PI * 2
      const r = 0.5 + Math.random() * 1.5
      const speed = (0.2 + Math.random() * 0.4) * preset.baseSpeed
      this.spawn(
        Math.cos(angle) * r, Math.sin(angle) * r,
        Math.cos(angle) * speed, Math.sin(angle) * speed,
        3 + Math.random() * 4,
        0.4 + Math.random() * 0.4,
        SRC_NET,
      )
    }
  }

  spawnIOParticles(signals: KhorSignals) {
    const io = (signals.blk_r_kbs + signals.blk_w_kbs) * 0.5
    const count = Math.floor(io * 4)
    if (count <= 0) return
    for (let i = 0; i < count; i++) {
      const angle = Math.random() * Math.PI * 2
      const r = Math.random() * 0.8
      this.spawn(
        Math.cos(angle) * r, Math.sin(angle) * r,
        (Math.random() - 0.5) * 0.15, (Math.random() - 0.5) * 0.15,
        2 + Math.random() * 3,
        0.6 + Math.random() * 0.4,
        SRC_IO,
      )
    }
  }

  spawnRetxParticles(signals: KhorSignals) {
    if (signals.retx_s < 0.05) return
    const count = Math.floor(signals.retx_s * 8)
    for (let i = 0; i < count; i++) {
      const angle = Math.random() * Math.PI * 2
      const speed = 0.5 + Math.random() * 1.0
      this.spawn(
        0, 0,
        Math.cos(angle) * speed, Math.sin(angle) * speed,
        1 + Math.random() * 1.5,
        0.7 + Math.random() * 0.3,
        SRC_RETX,
      )
    }
  }

  spawnIRQParticles(signals: KhorSignals) {
    if (signals.irq_s < 0.03) return
    const count = Math.floor(signals.irq_s * 4)
    for (let i = 0; i < count; i++) {
      const angle = Math.random() * Math.PI * 2
      const r = 1 + Math.random() * 2
      this.spawn(
        Math.cos(angle) * r, Math.sin(angle) * r,
        (Math.random() - 0.5) * 0.2, (Math.random() - 0.5) * 0.2,
        2 + Math.random() * 3,
        0.3 + Math.random() * 0.3,
        SRC_IRQ,
      )
    }
  }

  spawnDrift(preset: VisualPreset) {
    if (Math.random() > preset.driftSpeed) return
    const angle = Math.random() * Math.PI * 2
    const r = 2 + Math.random() * 3
    const x = Math.cos(angle) * r
    const y = Math.sin(angle) * r
    this.spawn(
      x, y,
      -x * 0.02, -y * 0.02,
      4 + Math.random() * 5,
      0.35 + Math.random() * 0.3,
      SRC_DRIFT,
    )
  }

  // ── Update ──────────────────────────────────────────────────────────────

  update(signals: KhorSignals, preset: VisualPreset, dtS: number) {
    this.time += dtS

    const flowParams: FlowParams = {
      frequency: preset.noiseFrequency * (1 + signals.csw_s * 2),
      speed: 0.15 + signals.csw_s * 0.25,
      strength: preset.baseSpeed * 0.8,
      gravityStrength: 0.05,
    }

    const netExpand = (signals.rx_kbs + signals.tx_kbs) * 0.5
    const irqJit = signals.irq_s * 0.15

    for (let i = 0; i < this.maxParticles; i++) {
      if (!this.alive[i]) continue

      this.life[i] += dtS
      if (this.life[i] >= this.maxLife[i]) {
        this.alive[i] = 0
        continue
      }

      const [fx, fy] = sampleFlow(this.px[i], this.py[i], this.time, flowParams)
      this.vx[i] += fx * dtS
      this.vy[i] += fy * dtS

      if (netExpand > 0.01) {
        const dist = Math.sqrt(this.px[i] ** 2 + this.py[i] ** 2) || 0.01
        this.vx[i] += (this.px[i] / dist) * netExpand * 0.3 * dtS
        this.vy[i] += (this.py[i] / dist) * netExpand * 0.3 * dtS
      }

      if (irqJit > 0.01) {
        this.vx[i] += (Math.random() - 0.5) * irqJit
        this.vy[i] += (Math.random() - 0.5) * irqJit
      }

      this.vx[i] *= 0.96
      this.vy[i] *= 0.96

      this.px[i] += this.vx[i] * dtS
      this.py[i] += this.vy[i] * dtS

      if (Math.abs(this.px[i]) > 6 || Math.abs(this.py[i]) > 6) {
        this.alive[i] = 0
        continue
      }

      const head = this.trailHead[i]
      const base = i * this.trailLen * 2
      this.trails[base + head * 2] = this.px[i]
      this.trails[base + head * 2 + 1] = this.py[i]
      this.trailHead[i] = (head + 1) % this.trailLen
    }
  }

  // ── GPU sync ────────────────────────────────────────────────────────────

  syncToGPU(particleSize: number, colors: number[][]) {
    const posAttr = this.pointsGeom.attributes.position as THREE.BufferAttribute
    const alphaAttr = this.pointsGeom.attributes.aAlpha as THREE.BufferAttribute
    const sizeAttr = this.pointsGeom.attributes.aSize as THREE.BufferAttribute
    const colorAttr = this.pointsGeom.attributes.aColor as THREE.BufferAttribute
    const pos = posAttr.array as Float32Array
    const alpha = alphaAttr.array as Float32Array
    const size = sizeAttr.array as Float32Array
    const col = colorAttr.array as Float32Array

    for (let i = 0; i < this.maxParticles; i++) {
      const i3 = i * 3
      if (!this.alive[i]) {
        pos[i3] = 0; pos[i3+1] = 0; pos[i3+2] = -100
        alpha[i] = 0
        size[i] = 0
        col[i3] = 0; col[i3+1] = 0; col[i3+2] = 0
        continue
      }
      pos[i3] = this.px[i]
      pos[i3+1] = this.py[i]
      pos[i3+2] = 0

      const lf = this.life[i] / this.maxLife[i]
      let a = 1
      if (lf < 0.1) a = lf / 0.1
      else if (lf > 0.75) a = 1 - (lf - 0.75) / 0.25

      alpha[i] = a * Math.max(0.4, this.brightness[i])
      size[i] = particleSize

      const c = colors[this.source[i]] ?? colors[0]
      col[i3] = c[0]
      col[i3+1] = c[1]
      col[i3+2] = c[2]
    }

    posAttr.needsUpdate = true
    alphaAttr.needsUpdate = true
    sizeAttr.needsUpdate = true
    colorAttr.needsUpdate = true

    // ── Trail segments ──
    const tPos = this.trailGeom.attributes.position as THREE.BufferAttribute
    const tAlpha = this.trailGeom.attributes.aAlpha as THREE.BufferAttribute
    const tColor = this.trailGeom.attributes.aColor as THREE.BufferAttribute
    const tp = tPos.array as Float32Array
    const ta = tAlpha.array as Float32Array
    const tc = tColor.array as Float32Array

    let segIdx = 0
    const segsPerParticle = this.trailLen - 1

    for (let i = 0; i < this.maxParticles; i++) {
      if (!this.alive[i]) {
        for (let s = 0; s < segsPerParticle; s++) {
          const vi = (segIdx + s) * 6
          tp[vi] = 0; tp[vi+1] = 0; tp[vi+2] = -100
          tp[vi+3] = 0; tp[vi+4] = 0; tp[vi+5] = -100
          const ai = (segIdx + s) * 2
          ta[ai] = 0; ta[ai+1] = 0
          const ci = (segIdx + s) * 6
          tc[ci] = 0; tc[ci+1] = 0; tc[ci+2] = 0
          tc[ci+3] = 0; tc[ci+4] = 0; tc[ci+5] = 0
        }
        segIdx += segsPerParticle
        continue
      }

      const head = this.trailHead[i]
      const base = i * this.trailLen * 2
      const lf = this.life[i] / this.maxLife[i]
      let particleAlpha = 1
      if (lf < 0.1) particleAlpha = lf / 0.1
      else if (lf > 0.75) particleAlpha = 1 - (lf - 0.75) / 0.25

      const c = colors[this.source[i]] ?? colors[0]

      for (let s = 0; s < segsPerParticle; s++) {
        const t0 = (head - 1 - s + this.trailLen) % this.trailLen
        const t1 = (head - 2 - s + this.trailLen) % this.trailLen

        const vi = (segIdx + s) * 6
        tp[vi]   = this.trails[base + t0 * 2]
        tp[vi+1] = this.trails[base + t0 * 2 + 1]
        tp[vi+2] = 0
        tp[vi+3] = this.trails[base + t1 * 2]
        tp[vi+4] = this.trails[base + t1 * 2 + 1]
        tp[vi+5] = 0

        const frac = s / segsPerParticle
        const segAlpha = (1 - frac) * 0.15 * particleAlpha * this.brightness[i]
        const ai = (segIdx + s) * 2
        ta[ai] = segAlpha
        ta[ai+1] = segAlpha * 0.5

        const ci = (segIdx + s) * 6
        tc[ci] = c[0]; tc[ci+1] = c[1]; tc[ci+2] = c[2]
        tc[ci+3] = c[0]; tc[ci+4] = c[1]; tc[ci+5] = c[2]
      }

      segIdx += segsPerParticle
    }

    tPos.needsUpdate = true
    tAlpha.needsUpdate = true
    tColor.needsUpdate = true

    this.trailGeom.setDrawRange(0, segIdx * 2)
    this.pointsGeom.setDrawRange(0, this.maxParticles)
  }

  dispose() {
    this.pointsGeom.dispose()
    ;(this.pointsMesh.material as THREE.Material).dispose()
    this.trailGeom.dispose()
    ;(this.trailMesh.material as THREE.Material).dispose()
  }
}
