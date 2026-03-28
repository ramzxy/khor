// Main visualizer component — fullscreen Three.js overlay with SSE hookup.

import { useCallback, useEffect, useRef, useState } from 'react'
import { RenderEngine, type EngineOptions } from './engine'
import { SignalStore } from './signals'
import { VisualizerControls } from './controls'

interface KhorVisualizerProps {
  apiBase: string
  onClose: () => void
}

const DEFAULT_OPTIONS: EngineOptions = {
  particleDensity: 1,
  trailLength: 1,
  accentHue: null,
  showLabels: false,
  paused: false,
}

export function KhorVisualizer({ apiBase, onClose }: KhorVisualizerProps) {
  const containerRef = useRef<HTMLDivElement>(null)
  const engineRef = useRef<RenderEngine | null>(null)
  const signalsRef = useRef<SignalStore | null>(null)
  const [options, setOptions] = useState<EngineOptions>(DEFAULT_OPTIONS)
  const [connected, setConnected] = useState(false)

  // Initialize engine + signals
  useEffect(() => {
    const container = containerRef.current
    if (!container) return

    const signals = new SignalStore()
    signalsRef.current = signals
    signals.start(apiBase)

    const engine = new RenderEngine(container, signals, options)
    engineRef.current = engine

    // Mount the WebGL canvas
    container.insertBefore(engine.domElement, container.firstChild)
    engine.domElement.style.cssText = 'position:absolute;inset:0;width:100%;height:100%;'

    // Initial size
    const rect = container.getBoundingClientRect()
    engine.resize(rect.width, rect.height)
    engine.start()

    // Poll connection status
    const connPoll = setInterval(() => {
      setConnected(signals.connected)
    }, 500)

    return () => {
      clearInterval(connPoll)
      engine.dispose()
      engine.domElement.remove()
      signals.stop()
      engineRef.current = null
      signalsRef.current = null
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [apiBase])

  // Handle resize
  useEffect(() => {
    const container = containerRef.current
    if (!container) return

    const obs = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const { width, height } = entry.contentRect
        if (width > 0 && height > 0) {
          engineRef.current?.resize(width, height)
        }
      }
    })
    obs.observe(container)
    return () => obs.disconnect()
  }, [])

  // Propagate option changes to engine
  useEffect(() => {
    engineRef.current?.updateOptions(options)
  }, [options])

  // Keyboard shortcuts
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        onClose()
      } else if (e.key === 'F11') {
        e.preventDefault()
        toggleFullscreen()
      } else if (e.key === ' ') {
        e.preventDefault()
        setOptions(o => ({ ...o, paused: !o.paused }))
      } else if (e.key === 'l' || e.key === 'L') {
        setOptions(o => ({ ...o, showLabels: !o.showLabels }))
      }
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [onClose])

  const toggleFullscreen = useCallback(() => {
    const el = containerRef.current
    if (!el) return
    if (document.fullscreenElement) {
      void document.exitFullscreen()
    } else {
      void el.requestFullscreen()
    }
  }, [])

  const handleOptionsChange = useCallback((partial: Partial<EngineOptions>) => {
    setOptions(o => ({ ...o, ...partial }))
  }, [])

  return (
    <div
      ref={containerRef}
      className="fixed inset-0 z-[100] bg-black"
      style={{ cursor: 'crosshair' }}
    >
      <VisualizerControls
        options={options}
        onChange={handleOptionsChange}
        onClose={onClose}
        onFullscreen={toggleFullscreen}
        connected={connected}
      />

      {/* Keyboard hint — fades after 3s */}
      <div className="animate-fadeOut pointer-events-none absolute top-4 left-1/2 -translate-x-1/2 rounded-xl border border-white/10 bg-slate-950/70 px-4 py-2 text-[10px] text-slate-500 backdrop-blur">
        ESC close &middot; SPACE pause &middot; L labels &middot; F11 fullscreen
      </div>
    </div>
  )
}
