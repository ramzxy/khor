// Three.js render engine — orchestrates scene, particles, bloom, and signal mapping.

import * as THREE from 'three'
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js'
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js'
import { UnrealBloomPass } from 'three/addons/postprocessing/UnrealBloomPass.js'
import { ParticleSystem, SIGNAL_COLORS } from './particles'
import type { SignalStore } from './signals'
import { getPreset } from './presets'

export interface EngineOptions {
  particleDensity: number  // 0=low, 1=medium, 2=high
  trailLength: number      // 0=short, 1=medium, 2=long
  accentHue: number | null // null = auto (unused now, each signal has its own color)
  showLabels: boolean
  paused: boolean
}

const DENSITY_MAP = [3000, 8000, 20000]
const BLOOM_MAP = [0.15, 0.3, 0.5]

export class RenderEngine {
  private renderer: THREE.WebGLRenderer
  private scene: THREE.Scene
  private camera: THREE.PerspectiveCamera
  private composer: EffectComposer
  private bloomPass: UnrealBloomPass

  private particles: ParticleSystem
  private signals: SignalStore

  private rafId = 0
  private lastTime = 0
  private frameCount = 0

  private labelCanvas: HTMLCanvasElement | null = null
  private labelCtx: CanvasRenderingContext2D | null = null

  options: EngineOptions
  readonly domElement: HTMLCanvasElement

  constructor(container: HTMLElement, signals: SignalStore, options: EngineOptions) {
    this.signals = signals
    this.options = options

    this.renderer = new THREE.WebGLRenderer({ antialias: false, alpha: false })
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))
    this.renderer.setClearColor(0x000000, 1)
    this.domElement = this.renderer.domElement

    this.scene = new THREE.Scene()

    this.camera = new THREE.PerspectiveCamera(50, 1, 0.1, 100)
    this.camera.position.set(0, 0, 6)
    this.camera.lookAt(0, 0, 0)

    this.composer = new EffectComposer(this.renderer)
    this.composer.addPass(new RenderPass(this.scene, this.camera))
    this.bloomPass = new UnrealBloomPass(
      new THREE.Vector2(1, 1),
      BLOOM_MAP[options.trailLength] ?? 0.3,
      0.4,
      0.6,
    )
    this.composer.addPass(this.bloomPass)

    this.particles = this.createParticles()

    this.labelCanvas = document.createElement('canvas')
    this.labelCanvas.style.cssText = 'position:absolute;inset:0;width:100%;height:100%;pointer-events:none;'
    this.labelCtx = this.labelCanvas.getContext('2d')
    container.appendChild(this.labelCanvas)
  }

  private createParticles(): ParticleSystem {
    const ps = new ParticleSystem({
      maxParticles: DENSITY_MAP[this.options.particleDensity] ?? DENSITY_MAP[1],
      trailLength: this.options.trailLength,
    })
    this.scene.add(ps.pointsMesh)
    this.scene.add(ps.trailMesh)
    return ps
  }

  resize(w: number, h: number) {
    this.renderer.setSize(w, h)
    this.composer.setSize(w, h)
    this.camera.aspect = w / h
    this.camera.updateProjectionMatrix()
    this.bloomPass.resolution.set(w, h)

    if (this.labelCanvas) {
      const dpr = Math.min(window.devicePixelRatio, 2)
      this.labelCanvas.width = w * dpr
      this.labelCanvas.height = h * dpr
      this.labelCanvas.style.width = `${w}px`
      this.labelCanvas.style.height = `${h}px`
      this.labelCtx?.setTransform(dpr, 0, 0, dpr, 0, 0)
    }
  }

  updateOptions(opts: Partial<EngineOptions>) {
    const needsRebuild =
      (opts.particleDensity !== undefined && opts.particleDensity !== this.options.particleDensity) ||
      (opts.trailLength !== undefined && opts.trailLength !== this.options.trailLength)

    Object.assign(this.options, opts)

    if (opts.trailLength !== undefined) {
      this.bloomPass.strength = BLOOM_MAP[opts.trailLength] ?? 0.3
    }

    if (needsRebuild) {
      this.scene.remove(this.particles.pointsMesh)
      this.scene.remove(this.particles.trailMesh)
      this.particles.dispose()
      this.particles = this.createParticles()
    }
  }

  start() {
    this.lastTime = performance.now()
    this.loop(this.lastTime)
  }

  stop() {
    if (this.rafId) cancelAnimationFrame(this.rafId)
    this.rafId = 0
  }

  dispose() {
    this.stop()
    this.particles.dispose()
    this.renderer.dispose()
    this.labelCanvas?.remove()
  }

  private loop = (now: number) => {
    this.rafId = requestAnimationFrame(this.loop)
    const dt = Math.min(now - this.lastTime, 50)
    this.lastTime = now
    if (this.options.paused || dt === 0) return

    this.frameCount++
    const dtS = dt * 0.001
    this.signals.tick()

    const preset = getPreset(this.signals.preset)
    const s = this.signals.smoothed

    // Spawn from each signal source
    if (s.exec_s > 0.02) this.particles.spawnExecBurst(s, preset)
    if ((s.rx_kbs + s.tx_kbs) > 0.04) this.particles.spawnNetworkParticles(s, preset)
    if ((s.blk_r_kbs + s.blk_w_kbs) > 0.04) this.particles.spawnIOParticles(s)
    this.particles.spawnRetxParticles(s)
    this.particles.spawnIRQParticles(s)
    this.particles.spawnDrift(preset)

    // Physics
    this.particles.update(s, preset, dtS)

    // Sync to GPU with signal colors
    this.particles.syncToGPU(preset.particleSize, SIGNAL_COLORS)
    this.composer.render()

    // Labels
    if (this.options.showLabels && this.labelCtx && this.labelCanvas) {
      const w = parseInt(this.labelCanvas.style.width || '800')
      const h = parseInt(this.labelCanvas.style.height || '600')
      this.drawLabels(this.labelCtx, w, h, s)
    } else if (this.labelCtx && this.labelCanvas) {
      this.labelCtx.clearRect(0, 0, this.labelCanvas.width, this.labelCanvas.height)
    }
  }

  private drawLabels(ctx: CanvasRenderingContext2D, w: number, h: number, s: typeof this.signals.smoothed) {
    ctx.clearRect(0, 0, w * 2, h * 2)
    ctx.font = '11px "Space Grotesk", sans-serif'
    ctx.textAlign = 'left'

    const labels: [string, string, number[]][] = [
      [`exec ${(s.exec_s * 100).toFixed(0)}%`, 'bursts', SIGNAL_COLORS[1]],
      [`csw ${(s.csw_s * 100).toFixed(0)}%`, 'turbulence', [0.6, 0.6, 0.65]],
      [`net ${((s.rx_kbs + s.tx_kbs) * 50).toFixed(0)}%`, 'expansion', SIGNAL_COLORS[2]],
      [`io ${((s.blk_r_kbs + s.blk_w_kbs) * 50).toFixed(0)}%`, 'glow', SIGNAL_COLORS[3]],
      [`retx ${(s.retx_s * 100).toFixed(0)}%`, 'flash', SIGNAL_COLORS[4]],
      [`irq ${(s.irq_s * 100).toFixed(0)}%`, 'jitter', SIGNAL_COLORS[5]],
      [`mem ${(s.mem_pct * 100).toFixed(0)}%`, 'warmth', [0.7, 0.7, 0.75]],
    ]

    const y0 = h - labels.length * 20 - 16
    for (let i = 0; i < labels.length; i++) {
      const [sig, param, c] = labels[i]
      const r = Math.round(c[0] * 255)
      const g = Math.round(c[1] * 255)
      const b = Math.round(c[2] * 255)
      ctx.fillStyle = `rgba(${r},${g},${b},0.5)`
      ctx.fillText('\u25CF', 14, y0 + i * 20)
      ctx.fillStyle = 'rgba(255,255,255,0.35)'
      ctx.fillText(`${sig} \u2192 ${param}`, 28, y0 + i * 20)
    }
  }
}
