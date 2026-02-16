import { useEffect, useMemo, useRef, useState } from 'react'
import { cn } from './lib/cn'

type ApiHealth = {
  ts_ms: number
  config_path: string
  audio: { enabled: boolean; ok: boolean; backend: string; device: string; error?: string }
  bpf: { enabled: boolean; ok: boolean; err_code: number; error?: string }
  midi: { enabled: boolean; ok: boolean; port: string; channel: number; error?: string }
  osc: { enabled: boolean; ok: boolean; host: string; port: number; error?: string }
  features: { fake: boolean }
}

type ApiConfig = {
  version: number
  listen: { host: string; port: number }
  ui: { serve: boolean; dir: string }
  features: { bpf: boolean; audio: boolean; midi: boolean; osc: boolean; fake: boolean }
  bpf: { enabled_mask: number; sample_interval_ms: number; tgid_allow?: number; tgid_deny?: number; cgroup_id?: number }
  music: { bpm: number; key_midi: number; scale: string; preset: string; density: number; smoothing: number }
  audio: { backend: string; device: string; sample_rate: number; master_gain: number }
  midi: { port: string; channel: number }
  osc: { host: string; port: number }
  ok?: boolean
  restart_required?: boolean
  error?: string
}

type ApiMetrics = {
  ts_ms: number
  totals: {
    events_total: number
    events_dropped: number
    exec_total: number
    net_rx_bytes_total: number
    net_tx_bytes_total: number
    sched_switch_total: number
    blk_read_bytes_total: number
    blk_write_bytes_total: number
  }
  rates: {
    exec_s: number
    rx_kbs: number
    tx_kbs: number
    csw_s: number
    blk_r_kbs: number
    blk_w_kbs: number
  }
  controls: { bpm: number; key_midi: number; density: number; smoothing: number }
}

type RatePoint = { ts_ms: number } & ApiMetrics['rates']

type AudioDevice = { id: string; name: string; is_default: boolean }

const API_BASE = ((import.meta as any).env?.VITE_API_BASE as string | undefined) ?? ''
const api = (path: string) => `${API_BASE}${path}`

function midiToName(midi: number) {
  const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
  const n = names[((midi % 12) + 12) % 12]
  const oct = Math.floor(midi / 12) - 1
  return `${n}${oct}`
}

function fmtK(v: number) {
  if (!Number.isFinite(v)) return '—'
  if (v >= 1000) return `${(v / 1000).toFixed(1)}M`
  if (v >= 100) return `${v.toFixed(0)}k`
  if (v >= 10) return `${v.toFixed(1)}k`
  return `${v.toFixed(2)}k`
}

function Sparkline(props: { points: RatePoint[]; k: keyof ApiMetrics['rates']; className?: string }) {
  const { points, k } = props
  const w = 240
  const h = 44
  const pad = 3
  const vals = points.map((p) => (Number.isFinite(p[k]) ? (p[k] as number) : 0))
  const min = Math.min(...vals, 0)
  const max = Math.max(...vals, 1e-6)

  const d = vals
    .map((v, i) => {
      const x = pad + (i / Math.max(1, vals.length - 1)) * (w - pad * 2)
      const t = (v - min) / (max - min || 1)
      const y = pad + (1 - t) * (h - pad * 2)
      return `${i === 0 ? 'M' : 'L'}${x.toFixed(2)},${y.toFixed(2)}`
    })
    .join(' ')

  return (
    <svg viewBox={`0 0 ${w} ${h}`} className={cn('h-12 w-full', props.className)}>
      <path d={d} fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
      <path d={`M${pad},${h - pad} L${w - pad},${h - pad}`} stroke="currentColor" opacity="0.12" />
    </svg>
  )
}

async function fetchJson<T>(url: string, init?: RequestInit) {
  const r = await fetch(url, init)
  const text = await r.text()
  let j: any = null
  try {
    j = text ? JSON.parse(text) : null
  } catch {
    /* ignore */
  }
  if (!r.ok) {
    const msg = j?.error ? String(j.error) : `HTTP ${r.status}`
    throw new Error(msg)
  }
  return j as T
}

export default function App() {
  const [health, setHealth] = useState<ApiHealth | null>(null)
  const [config, setConfig] = useState<ApiConfig | null>(null)
  const [metrics, setMetrics] = useState<ApiMetrics | null>(null)
  const [history, setHistory] = useState<RatePoint[]>([])
  const [devices, setDevices] = useState<AudioDevice[]>([])

  const [err, setErr] = useState<string | null>(null)
  const [restartHint, setRestartHint] = useState<string | null>(null)

  const esRef = useRef<EventSource | null>(null)

  const keyName = useMemo(() => {
    const midi = config?.music?.key_midi
    if (typeof midi !== 'number' || !Number.isFinite(midi)) return 'n/a'
    return midiToName(midi)
  }, [config?.music?.key_midi])

  async function refreshHealth() {
    const h = await fetchJson<ApiHealth>(api('/api/health'))
    setHealth(h)
  }

  async function refreshConfig() {
    const c = await fetchJson<ApiConfig>(api('/api/config'))
    setConfig(c)
  }

  async function refreshDevices() {
    const r = await fetchJson<{ devices: AudioDevice[] }>(api('/api/audio/devices'))
    setDevices(r.devices ?? [])
  }

  async function putConfig(patch: any) {
    const r = await fetchJson<ApiConfig>(api('/api/config'), {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(patch),
    })
    setConfig(r)
    if (r.restart_required) setRestartHint('Some changes require restarting the daemon to take effect.')
    return r
  }

  async function post(url: string) {
    await fetchJson(api(url), { method: 'POST' })
  }

  useEffect(() => {
    let alive = true

    const boot = async () => {
      try {
        await Promise.all([refreshHealth(), refreshConfig(), refreshDevices()])
        if (!alive) return
        setErr(null)
      } catch (e) {
        if (!alive) return
        setErr(e instanceof Error ? e.message : 'daemon not reachable')
      }
    }

    boot()

    const id = window.setInterval(() => {
      void refreshHealth().catch(() => {})
    }, 1200)

    return () => {
      alive = false
      window.clearInterval(id)
    }
  }, [])

  useEffect(() => {
    // Prefer SSE for live metrics; fall back to polling.
    let alive = true

    const open = () => {
      try {
        const es = new EventSource(api('/api/stream'))
        esRef.current = es
        es.onmessage = (evt) => {
          if (!alive) return
          try {
            const m = JSON.parse(evt.data) as ApiMetrics
            setMetrics(m)
            setErr(null)
            setHistory((prev) => {
              const next = prev.concat([{ ts_ms: m.ts_ms, ...m.rates }])
              return next.length > 600 ? next.slice(next.length - 600) : next
            })
          } catch {
            /* ignore */
          }
        }
        es.onerror = () => {
          es.close()
          esRef.current = null
        }
      } catch {
        /* ignore */
      }
    }

    open()

    const poll = window.setInterval(() => {
      if (esRef.current) return
      void fetchJson<ApiMetrics>(api('/api/metrics'))
        .then((m) => {
          if (!alive) return
          setMetrics(m)
          setErr(null)
          if ((m as any).history && Array.isArray((m as any).history)) {
            setHistory((m as any).history as RatePoint[])
          }
        })
        .catch((e) => {
          if (!alive) return
          setErr(e instanceof Error ? e.message : 'daemon not reachable')
        })
    }, 500)

    return () => {
      alive = false
      window.clearInterval(poll)
      esRef.current?.close()
      esRef.current = null
    }
  }, [])

  return (
    <div className="min-h-dvh bg-[radial-gradient(1400px_650px_at_10%_10%,#a7f3d0_0%,rgba(167,243,208,0)_55%),radial-gradient(1200px_650px_at_95%_20%,#fecaca_0%,rgba(254,202,202,0)_55%),linear-gradient(#fafafa,#f4f4f5)] text-zinc-950">
      <div className="mx-auto w-full max-w-6xl px-5 py-8">
        <header className="flex flex-col gap-3 md:flex-row md:items-end md:justify-between">
          <div>
            <div className="inline-flex items-center gap-2 rounded-full border border-zinc-200 bg-white/70 px-3 py-1 text-xs text-zinc-700 shadow-sm backdrop-blur">
              <span className={cn('h-2 w-2 rounded-full', err ? 'bg-amber-500' : 'bg-emerald-500')} />
              <span className="font-medium">khor</span>
              <span className="text-zinc-500">API</span>
              <span className="font-mono text-zinc-800">{API_BASE || '(same-origin)'}</span>
            </div>
            <h1 className="mt-3 text-balance text-3xl font-semibold tracking-tight">Kernel activity as music</h1>
            <p className="mt-1 text-pretty text-sm text-zinc-700">
              eBPF rates drive a deterministic sequencer. Audio is local; MIDI and OSC are optional adapters.
            </p>
          </div>
          <div className="rounded-xl border border-zinc-200 bg-white/70 p-3 text-sm shadow-sm backdrop-blur">
            <div className="tabular-nums text-zinc-800">
              {metrics ? (
                <span>
                  exec/s {metrics.rates.exec_s.toFixed(1)} · rx {metrics.rates.rx_kbs.toFixed(1)} kB/s · tx{' '}
                  {metrics.rates.tx_kbs.toFixed(1)} kB/s
                </span>
              ) : (
                <span>connecting…</span>
              )}
            </div>
            <div className="mt-1 flex items-center justify-between gap-3 text-xs text-zinc-600">
              <span className="tabular-nums">ringbuf lost {metrics?.totals.events_dropped ?? 0}</span>
              <span className="tabular-nums">events {metrics?.totals.events_total ?? 0}</span>
            </div>
          </div>
        </header>

        {err ? (
          <div className="mt-5 rounded-xl border border-amber-200 bg-amber-50 p-4 text-sm text-amber-900 shadow-sm">
            <div className="font-medium">Daemon not reachable</div>
            <div className="mt-1 text-pretty text-amber-800">
              {err}. Start it with <span className="font-mono">./scripts/linux-run.sh</span>.
            </div>
          </div>
        ) : null}

        {restartHint ? (
          <div className="mt-5 rounded-xl border border-zinc-200 bg-white/70 p-4 text-sm text-zinc-800 shadow-sm backdrop-blur">
            <div className="font-medium">Restart required</div>
            <div className="mt-1 text-zinc-600">{restartHint}</div>
          </div>
        ) : null}

        <div className="mt-7 grid gap-4 md:grid-cols-3">
          <section className="rounded-2xl border border-zinc-200 bg-white/75 p-4 shadow-sm backdrop-blur">
            <h2 className="text-sm font-medium text-zinc-800">Status</h2>
            <dl className="mt-3 grid gap-2 text-sm">
              <div className="flex items-center justify-between gap-3">
                <dt className="text-zinc-600">eBPF</dt>
                <dd className={cn('font-medium', health?.bpf.ok ? 'text-emerald-700' : 'text-amber-700')}>
                  {health ? (health.bpf.ok ? 'enabled' : health.bpf.enabled ? 'error' : 'disabled') : '—'}
                </dd>
              </div>
              {health?.bpf.error ? <div className="text-xs text-zinc-600">{health.bpf.error}</div> : null}

              <div className="mt-2 flex items-center justify-between gap-3">
                <dt className="text-zinc-600">Audio</dt>
                <dd className={cn('font-medium', health?.audio.ok ? 'text-emerald-700' : 'text-amber-700')}>
                  {health ? (health.audio.ok ? 'running' : health.audio.enabled ? 'error' : 'disabled') : '—'}
                </dd>
              </div>
              <div className="text-xs text-zinc-600">
                {health?.audio.backend ? <span className="font-mono">{health.audio.backend}</span> : null}
                {health?.audio.device ? <span className="ml-2 truncate font-mono">{health.audio.device}</span> : null}
              </div>
              {health?.audio.error ? <div className="text-xs text-zinc-600">{health.audio.error}</div> : null}

              <div className="mt-2 flex items-center justify-between gap-3">
                <dt className="text-zinc-600">MIDI</dt>
                <dd className={cn('font-medium', health?.midi.ok ? 'text-emerald-700' : 'text-zinc-700')}>
                  {health ? (health.midi.ok ? 'on' : health.midi.enabled ? 'error' : 'off') : '—'}
                </dd>
              </div>
              {health?.midi.error ? <div className="text-xs text-zinc-600">{health.midi.error}</div> : null}

              <div className="mt-2 flex items-center justify-between gap-3">
                <dt className="text-zinc-600">OSC</dt>
                <dd className={cn('font-medium', health?.osc.ok ? 'text-emerald-700' : 'text-zinc-700')}>
                  {health ? (health.osc.ok ? 'on' : health.osc.enabled ? 'error' : 'off') : '—'}
                </dd>
              </div>
              {health?.osc.error ? <div className="text-xs text-zinc-600">{health.osc.error}</div> : null}
            </dl>
          </section>

          <section className="rounded-2xl border border-zinc-200 bg-white/75 p-4 shadow-sm backdrop-blur md:col-span-2">
            <h2 className="text-sm font-medium text-zinc-800">Live Rates</h2>
            <div className="mt-4 grid gap-3 md:grid-cols-2">
              <div className="rounded-xl border border-zinc-200 bg-white p-3">
                <div className="flex items-baseline justify-between gap-3">
                  <div className="text-xs text-zinc-600">exec/s</div>
                  <div className="tabular-nums text-sm font-medium">{metrics ? metrics.rates.exec_s.toFixed(1) : '—'}</div>
                </div>
                <div className="mt-2 text-emerald-700">
                  <Sparkline points={history} k="exec_s" />
                </div>
              </div>
              <div className="rounded-xl border border-zinc-200 bg-white p-3">
                <div className="flex items-baseline justify-between gap-3">
                  <div className="text-xs text-zinc-600">rx / tx (kB/s)</div>
                  <div className="tabular-nums text-sm font-medium">
                    {metrics ? `${metrics.rates.rx_kbs.toFixed(1)} / ${metrics.rates.tx_kbs.toFixed(1)}` : '—'}
                  </div>
                </div>
                <div className="mt-2 flex gap-2">
                  <div className="w-1/2 text-rose-700">
                    <Sparkline points={history} k="rx_kbs" />
                  </div>
                  <div className="w-1/2 text-indigo-700">
                    <Sparkline points={history} k="tx_kbs" />
                  </div>
                </div>
              </div>
              <div className="rounded-xl border border-zinc-200 bg-white p-3">
                <div className="flex items-baseline justify-between gap-3">
                  <div className="text-xs text-zinc-600">csw/s</div>
                  <div className="tabular-nums text-sm font-medium">{metrics ? metrics.rates.csw_s.toFixed(0) : '—'}</div>
                </div>
                <div className="mt-2 text-zinc-900">
                  <Sparkline points={history} k="csw_s" />
                </div>
              </div>
              <div className="rounded-xl border border-zinc-200 bg-white p-3">
                <div className="flex items-baseline justify-between gap-3">
                  <div className="text-xs text-zinc-600">io read / write (kB/s)</div>
                  <div className="tabular-nums text-sm font-medium">
                    {metrics ? `${metrics.rates.blk_r_kbs.toFixed(1)} / ${metrics.rates.blk_w_kbs.toFixed(1)}` : '—'}
                  </div>
                </div>
                <div className="mt-2 flex gap-2">
                  <div className="w-1/2 text-amber-700">
                    <Sparkline points={history} k="blk_r_kbs" />
                  </div>
                  <div className="w-1/2 text-cyan-700">
                    <Sparkline points={history} k="blk_w_kbs" />
                  </div>
                </div>
              </div>
            </div>
          </section>

          <section className="rounded-2xl border border-zinc-200 bg-white/75 p-4 shadow-sm backdrop-blur md:col-span-2">
            <h2 className="text-sm font-medium text-zinc-800">Presets + Mapping</h2>
            <div className="mt-3 flex flex-wrap gap-2">
              {(['ambient', 'percussive', 'arp', 'drone'] as const).map((p) => (
                <button
                  key={p}
                  className={cn(
                    'rounded-full border px-3 py-1 text-sm shadow-sm',
                    config?.music?.preset === p
                      ? 'border-zinc-900 bg-zinc-900 text-white'
                      : 'border-zinc-200 bg-white text-zinc-800 hover:border-zinc-300',
                  )}
                  onClick={() =>
                    void post(`/api/preset/select?name=${encodeURIComponent(p)}`)
                      .then(() => Promise.all([refreshConfig(), refreshHealth()]))
                      .catch((e) => setErr(e instanceof Error ? e.message : 'failed'))
                  }
                >
                  {p}
                </button>
              ))}
            </div>

            <div className="mt-5 grid gap-3 md:grid-cols-4">
              <label className="grid gap-1">
                <span className="text-xs text-zinc-600">BPM</span>
                <input
                  className="h-10 rounded-xl border border-zinc-200 bg-white px-3 text-sm tabular-nums outline-none focus:ring-2 focus:ring-zinc-900/10"
                  inputMode="numeric"
                  value={config ? String(Math.round(config.music.bpm)) : '110'}
                  onChange={(e) => setConfig((c) => (c ? { ...c, music: { ...c.music, bpm: Number(e.target.value) } } : c))}
                  onBlur={() => void putConfig({ music: { bpm: config?.music?.bpm } }).catch((e) => setErr(String(e)))}
                />
              </label>
              <label className="grid gap-1">
                <span className="text-xs text-zinc-600">Key</span>
                <input
                  className="h-10 rounded-xl border border-zinc-200 bg-white px-3 text-sm tabular-nums outline-none focus:ring-2 focus:ring-zinc-900/10"
                  inputMode="numeric"
                  value={config ? String(config.music.key_midi) : '62'}
                  onChange={(e) =>
                    setConfig((c) => (c ? { ...c, music: { ...c.music, key_midi: Number(e.target.value) } } : c))
                  }
                  onBlur={() => void putConfig({ music: { key_midi: config?.music?.key_midi } }).catch((e) => setErr(String(e)))}
                />
                <span className="text-xs text-zinc-500 tabular-nums">{keyName}</span>
              </label>
              <label className="grid gap-1">
                <span className="text-xs text-zinc-600">Scale</span>
                <select
                  className="h-10 rounded-xl border border-zinc-200 bg-white px-3 text-sm outline-none focus:ring-2 focus:ring-zinc-900/10"
                  value={config?.music?.scale ?? 'pentatonic_minor'}
                  onChange={(e) => {
                    const v = e.target.value
                    setConfig((c) => (c ? { ...c, music: { ...c.music, scale: v } } : c))
                    void putConfig({ music: { scale: v } }).catch((x) => setErr(String(x)))
                  }}
                >
                  <option value="pentatonic_minor">Pentatonic minor</option>
                  <option value="natural_minor">Natural minor</option>
                  <option value="dorian">Dorian</option>
                </select>
              </label>
              <label className="grid gap-1">
                <span className="text-xs text-zinc-600">Density</span>
                <input
                  className="h-10 rounded-xl border border-zinc-200 bg-white px-3 text-sm tabular-nums outline-none focus:ring-2 focus:ring-zinc-900/10"
                  inputMode="decimal"
                  value={config ? String(config.music.density.toFixed(2)) : '0.35'}
                  onChange={(e) =>
                    setConfig((c) => (c ? { ...c, music: { ...c.music, density: Number(e.target.value) } } : c))
                  }
                  onBlur={() => void putConfig({ music: { density: config?.music?.density } }).catch((e) => setErr(String(e)))}
                />
              </label>
              <label className="grid gap-1 md:col-span-2">
                <span className="text-xs text-zinc-600">Smoothing</span>
                <input
                  type="range"
                  min={0}
                  max={1}
                  step={0.01}
                  value={config?.music?.smoothing ?? 0.85}
                  onChange={(e) => {
                    const v = Number(e.target.value)
                    setConfig((c) => (c ? { ...c, music: { ...c.music, smoothing: v } } : c))
                  }}
                  onMouseUp={() => void putConfig({ music: { smoothing: config?.music?.smoothing } }).catch((e) => setErr(String(e)))}
                  onTouchEnd={() => void putConfig({ music: { smoothing: config?.music?.smoothing } }).catch((e) => setErr(String(e)))}
                />
                <div className="text-xs text-zinc-500 tabular-nums">{config ? config.music.smoothing.toFixed(2) : '—'}</div>
              </label>

              <div className="flex items-end md:col-span-2">
                <button
                  className={cn(
                    'h-10 w-full rounded-xl border border-zinc-900 bg-zinc-900 px-3 text-sm font-medium text-white shadow-sm',
                    'active:translate-y-px',
                  )}
                  onClick={() =>
                    void putConfig({
                      music: {
                        bpm: config?.music?.bpm,
                        key_midi: config?.music?.key_midi,
                        scale: config?.music?.scale,
                        preset: config?.music?.preset,
                        density: config?.music?.density,
                        smoothing: config?.music?.smoothing,
                      },
                    })
                      .then(() => Promise.all([refreshHealth(), refreshConfig()]))
                      .catch((e) => setErr(e instanceof Error ? e.message : 'failed'))
                  }
                >
                  Apply Mapping
                </button>
              </div>
            </div>
          </section>

          <section className="rounded-2xl border border-zinc-200 bg-white/75 p-4 shadow-sm backdrop-blur">
            <h2 className="text-sm font-medium text-zinc-800">Outputs</h2>

            <div className="mt-3 grid gap-3">
              <div className="flex items-center justify-between rounded-xl border border-zinc-200 bg-white p-3">
                <div>
                  <div className="text-sm font-medium">Audio</div>
                  <div className="text-xs text-zinc-600">miniaudio via Pulse/ALSA</div>
                </div>
                <input
                  type="checkbox"
                  checked={config?.features?.audio ?? true}
                  onChange={(e) => void putConfig({ features: { audio: e.target.checked } }).then(refreshHealth).catch((x) => setErr(String(x)))}
                />
              </div>

              <label className="grid gap-1 rounded-xl border border-zinc-200 bg-white p-3">
                <div className="flex items-center justify-between gap-3">
                  <span className="text-xs text-zinc-600">Master gain</span>
                  <span className="text-xs tabular-nums text-zinc-700">{config ? config.audio.master_gain.toFixed(2) : '—'}</span>
                </div>
                <input
                  type="range"
                  min={0}
                  max={1}
                  step={0.01}
                  value={config?.audio?.master_gain ?? 0.25}
                  onChange={(e) => {
                    const v = Number(e.target.value)
                    setConfig((c) => (c ? { ...c, audio: { ...c.audio, master_gain: v } } : c))
                  }}
                  onMouseUp={() => void putConfig({ audio: { master_gain: config?.audio?.master_gain } }).catch((e) => setErr(String(e)))}
                  onTouchEnd={() => void putConfig({ audio: { master_gain: config?.audio?.master_gain } }).catch((e) => setErr(String(e)))}
                />
              </label>

              <div className="rounded-xl border border-zinc-200 bg-white p-3">
                <div className="flex items-center justify-between gap-3">
                  <div className="text-xs text-zinc-600">Device</div>
                  <button className="text-xs text-zinc-700 underline underline-offset-2" onClick={() => void refreshDevices().catch(() => {})}>
                    refresh
                  </button>
                </div>
                <select
                  className="mt-2 h-10 w-full rounded-xl border border-zinc-200 bg-white px-3 text-sm outline-none focus:ring-2 focus:ring-zinc-900/10"
                  value={config?.audio?.device?.startsWith('id:') ? config.audio.device.slice(3) : 'default'}
                  onChange={(e) => {
                    const id = e.target.value
                    void fetchJson(api('/api/audio/device'), {
                      method: 'POST',
                      headers: { 'Content-Type': 'application/json' },
                      body: JSON.stringify({ device: id === 'default' ? '' : `id:${id}` }),
                    })
                      .then(() => Promise.all([refreshConfig(), refreshHealth()]))
                      .catch((x) => setErr(String(x)))
                  }}
                >
                  <option value="default">default</option>
                  {devices.map((d) => (
                    <option key={d.id} value={d.id}>
                      {d.is_default ? '[default] ' : ''}
                      {d.name}
                    </option>
                  ))}
                </select>
              </div>

              <div className="flex items-center justify-between rounded-xl border border-zinc-200 bg-white p-3">
                <div>
                  <div className="text-sm font-medium">MIDI</div>
                  <div className="text-xs text-zinc-600">ALSA sequencer virtual port</div>
                </div>
                <input
                  type="checkbox"
                  checked={config?.features?.midi ?? false}
                  onChange={(e) => void putConfig({ features: { midi: e.target.checked } }).then(refreshHealth).catch((x) => setErr(String(x)))}
                />
              </div>
              <div className="grid gap-2 rounded-xl border border-zinc-200 bg-white p-3">
                <label className="grid gap-1">
                  <span className="text-xs text-zinc-600">Port name</span>
                  <input
                    className="h-10 rounded-xl border border-zinc-200 bg-white px-3 text-sm outline-none focus:ring-2 focus:ring-zinc-900/10"
                    value={config?.midi?.port ?? 'khor'}
                    onChange={(e) => setConfig((c) => (c ? { ...c, midi: { ...c.midi, port: e.target.value } } : c))}
                    onBlur={() => void putConfig({ midi: { port: config?.midi?.port } }).catch((x) => setErr(String(x)))}
                  />
                </label>
                <label className="grid gap-1">
                  <span className="text-xs text-zinc-600">Channel</span>
                  <input
                    className="h-10 rounded-xl border border-zinc-200 bg-white px-3 text-sm tabular-nums outline-none focus:ring-2 focus:ring-zinc-900/10"
                    inputMode="numeric"
                    value={config ? String(config.midi.channel) : '1'}
                    onChange={(e) =>
                      setConfig((c) => (c ? { ...c, midi: { ...c.midi, channel: Number(e.target.value) } } : c))
                    }
                    onBlur={() => void putConfig({ midi: { channel: config?.midi?.channel } }).catch((x) => setErr(String(x)))}
                  />
                </label>
              </div>

              <div className="flex items-center justify-between rounded-xl border border-zinc-200 bg-white p-3">
                <div>
                  <div className="text-sm font-medium">OSC</div>
                  <div className="text-xs text-zinc-600">UDP schema: /khor/note, /khor/signal</div>
                </div>
                <input
                  type="checkbox"
                  checked={config?.features?.osc ?? false}
                  onChange={(e) => void putConfig({ features: { osc: e.target.checked } }).then(refreshHealth).catch((x) => setErr(String(x)))}
                />
              </div>
              <div className="grid gap-2 rounded-xl border border-zinc-200 bg-white p-3">
                <label className="grid gap-1">
                  <span className="text-xs text-zinc-600">Host</span>
                  <input
                    className="h-10 rounded-xl border border-zinc-200 bg-white px-3 text-sm outline-none focus:ring-2 focus:ring-zinc-900/10"
                    value={config?.osc?.host ?? '127.0.0.1'}
                    onChange={(e) => setConfig((c) => (c ? { ...c, osc: { ...c.osc, host: e.target.value } } : c))}
                    onBlur={() => void putConfig({ osc: { host: config?.osc?.host } }).catch((x) => setErr(String(x)))}
                  />
                </label>
                <label className="grid gap-1">
                  <span className="text-xs text-zinc-600">Port</span>
                  <input
                    className="h-10 rounded-xl border border-zinc-200 bg-white px-3 text-sm tabular-nums outline-none focus:ring-2 focus:ring-zinc-900/10"
                    inputMode="numeric"
                    value={config ? String(config.osc.port) : '9000'}
                    onChange={(e) => setConfig((c) => (c ? { ...c, osc: { ...c.osc, port: Number(e.target.value) } } : c))}
                    onBlur={() => void putConfig({ osc: { port: config?.osc?.port } }).catch((x) => setErr(String(x)))}
                  />
                </label>
              </div>

              <div className="flex items-center justify-between rounded-xl border border-zinc-200 bg-white p-3">
                <div>
                  <div className="text-sm font-medium">Fake mode</div>
                  <div className="text-xs text-zinc-600">Only when eBPF is unavailable</div>
                </div>
                <input
                  type="checkbox"
                  checked={config?.features?.fake ?? false}
                  onChange={(e) => void putConfig({ features: { fake: e.target.checked } }).then(refreshHealth).catch((x) => setErr(String(x)))}
                />
              </div>
            </div>
          </section>

          <section className="rounded-2xl border border-zinc-200 bg-white/75 p-4 shadow-sm backdrop-blur md:col-span-3">
            <h2 className="text-sm font-medium text-zinc-800">Debug</h2>
            <div className="mt-3 flex flex-col gap-3 md:flex-row md:items-center md:justify-between">
              <div className="text-xs text-zinc-600">
                Config file: <span className="font-mono">{health?.config_path ?? '—'}</span>
              </div>
              <div className="flex gap-2">
                <button
                  className="h-10 rounded-xl border border-zinc-900 bg-zinc-900 px-4 text-sm font-medium text-white shadow-sm active:translate-y-px"
                  onClick={() =>
                    void post('/api/actions/test_note')
                      .then(() => setErr(null))
                      .catch((e) => setErr(e instanceof Error ? e.message : 'failed'))
                  }
                >
                  Play test note
                </button>
                <button
                  className="h-10 rounded-xl border border-zinc-200 bg-white px-4 text-sm font-medium text-zinc-800 shadow-sm active:translate-y-px"
                  onClick={() => {
                    setRestartHint(null)
                    void Promise.all([refreshHealth(), refreshConfig(), refreshDevices()]).catch(() => {})
                  }}
                >
                  Refresh
                </button>
              </div>
            </div>
          </section>
        </div>

        <footer className="mt-10 text-xs text-zinc-600">
          Tip: For eBPF without root runtime, run the one-time capability command:
          <span className="ml-2 rounded bg-white/70 px-2 py-1 font-mono">
            sudo setcap cap_bpf,cap_perfmon,cap_sys_resource+ep ~/.local/bin/khor-daemon
          </span>
        </footer>
      </div>
    </div>
  )
}
