import { useEffect, useMemo, useRef, useState, useCallback } from 'react'
import type { ChangeEvent, ReactNode } from 'react'
import { cn } from './lib/cn'
import { KhorVisualizer } from './components/visualizer/KhorVisualizer'

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
    tcp_retransmit_total: number
    irq_total: number
  }
  rates: {
    exec_s: number
    rx_kbs: number
    tx_kbs: number
    csw_s: number
    blk_r_kbs: number
    blk_w_kbs: number
    retx_s: number
    irq_s: number
    mem_pct: number
  }
  controls: { bpm: number; key_midi: number; density: number; smoothing: number }
}

type RatePoint = { ts_ms: number } & ApiMetrics['rates']

type AudioDevice = { id: string; name: string; is_default: boolean }
type ApiMetricsResponse = ApiMetrics & { history?: RatePoint[] }

type SectionPanelProps = {
  eyebrow?: string
  title?: string
  subtitle?: string
  actions?: ReactNode
  className?: string
  children: ReactNode
}

type HeroStatProps = {
  label: string
  value: ReactNode
  detail: ReactNode
  tintClassName: string
}

type StatusTileProps = {
  label: string
  state: string
  detail: ReactNode
  tone: 'emerald' | 'amber' | 'slate'
}

type RateCardProps = {
  label: string
  value: ReactNode
  detail: ReactNode
  tintClassName: string
  chartClassName: string
  children: ReactNode
}

type TelemetryLaneProps = {
  label: string
  value: ReactNode
  detail: ReactNode
  tintClassName: string
  children: ReactNode
}

type ToggleProps = {
  checked: boolean
  onChange: (event: ChangeEvent<HTMLInputElement>) => void
}

const API_BASE = import.meta.env.VITE_API_BASE ?? ''
const API_HINT = API_BASE || '(same-origin)'
const PRESET_OPTIONS = [
  { id: 'ambient', label: 'Ambient', description: 'Wide pads and slower motion with gentle phrasing.' },
  { id: 'percussive', label: 'Percussive', description: 'Sharper transients and more immediate rhythmic response.' },
  { id: 'arp', label: 'Arp', description: 'Tighter note spacing for patterned, machine-like movement.' },
  { id: 'drone', label: 'Drone', description: 'Sustained tones with restrained rhythm and heavier texture.' },
] as const
const SCALE_LABELS: Record<string, string> = {
  pentatonic_minor: 'Pentatonic minor',
  natural_minor: 'Natural minor',
  dorian: 'Dorian',
}
const compactNumber = new Intl.NumberFormat('en-US', { notation: 'compact', maximumFractionDigits: 1 })
const fieldClassName =
  'h-10 w-full rounded-[16px] border border-slate-700/80 bg-black/90 px-3.5 text-sm text-slate-100 outline-none transition placeholder:text-slate-500 focus:border-cyan-400/45 focus:ring-4 focus:ring-cyan-400/10'
const fieldPanelClassName =
  'grid min-w-0 gap-2 rounded-[20px] border border-white/10 bg-slate-900/72 p-3.5 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]'
const insetMetricCardClassName =
  'rounded-[18px] border border-white/10 bg-black/68 px-3.5 py-3 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]'

const api = (path: string) => `${API_BASE}${path}`

function midiToName(midi: number) {
  const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
  const n = names[((midi % 12) + 12) % 12]
  const oct = Math.floor(midi / 12) - 1
  return `${n}${oct}`
}

function formatRate(value: number | undefined, digits = 1) {
  return typeof value === 'number' && Number.isFinite(value) ? value.toFixed(digits) : '—'
}

function formatCompact(value: number | undefined) {
  return typeof value === 'number' && Number.isFinite(value) ? compactNumber.format(value) : '—'
}

function getWindowStats(points: RatePoint[], key: keyof ApiMetrics['rates']) {
  const values = points.map((point) => (Number.isFinite(point[key]) ? (point[key] as number) : 0))
  if (!values.length) return { peak: 0, average: 0 }
  return {
    peak: Math.max(...values, 0),
    average: values.reduce((sum, value) => sum + value, 0) / values.length,
  }
}

function Sparkline(props: { points: RatePoint[]; k: keyof ApiMetrics['rates']; className?: string }) {
  const { points, k } = props
  const w = 320
  const h = 68
  const pad = 6
  const vals = points.map((p) => (Number.isFinite(p[k]) ? (p[k] as number) : 0))

  if (!vals.length) {
    return (
      <svg viewBox={`0 0 ${w} ${h}`} className={cn('h-16 w-full', props.className)}>
        <path d={`M${pad},${h - pad} L${w - pad},${h - pad}`} stroke="currentColor" opacity="0.12" />
      </svg>
    )
  }

  const min = Math.min(...vals, 0)
  const max = Math.max(...vals, 1e-6)
  const coords = vals.map((v, i) => {
    const x = pad + (i / Math.max(1, vals.length - 1)) * (w - pad * 2)
    const t = (v - min) / (max - min || 1)
    const y = pad + (1 - t) * (h - pad * 2)
    return { x, y }
  })
  const linePath = coords
    .map((point, i) => `${i === 0 ? 'M' : 'L'}${point.x.toFixed(2)},${point.y.toFixed(2)}`)
    .join(' ')
  const last = coords[coords.length - 1]
  const areaPath = `${linePath} L${last.x.toFixed(2)},${(h - pad).toFixed(2)} L${coords[0].x.toFixed(2)},${(h - pad).toFixed(2)} Z`

  return (
    <svg viewBox={`0 0 ${w} ${h}`} className={cn('h-16 w-full', props.className)}>
      <path d={areaPath} fill="currentColor" opacity="0.18" />
      <path d={linePath} fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round" />
      <path d={`M${pad},${h - pad} L${w - pad},${h - pad}`} stroke="currentColor" opacity="0.1" />
      <circle cx={last.x} cy={last.y} r="3" fill="currentColor" opacity="0.92" />
    </svg>
  )
}

async function fetchJson<T>(url: string, init?: RequestInit) {
  const r = await fetch(url, init)
  const text = await r.text()
  let j: unknown = null
  try {
    j = text ? JSON.parse(text) : null
  } catch {
    /* ignore */
  }
  if (!r.ok) {
    const msg = j && typeof j === 'object' && 'error' in j ? String(j.error) : `HTTP ${r.status}`
    throw new Error(msg)
  }
  return j as T
}

function SectionPanel({ eyebrow, title, subtitle, actions, className, children }: SectionPanelProps) {
  return (
    <section
      className={cn(
        'min-w-0 rounded-[28px] border border-white/10 bg-black/65 p-4 shadow-[0_24px_70px_rgba(2,6,23,0.55)] backdrop-blur-xl sm:p-5',
        className,
      )}
    >
      {eyebrow || title || subtitle || actions ? (
        <div className="flex flex-col gap-4 md:flex-row md:items-start md:justify-between">
          <div className="max-w-2xl">
            {eyebrow ? <div className="text-[11px] font-medium uppercase tracking-[0.28em] text-slate-500">{eyebrow}</div> : null}
            {title ? <h2 className="mt-2 text-2xl font-semibold tracking-[-0.04em] text-slate-50">{title}</h2> : null}
            {subtitle ? <p className="mt-2 text-sm leading-6 text-slate-300">{subtitle}</p> : null}
          </div>
          {actions ? <div className="shrink-0">{actions}</div> : null}
        </div>
      ) : null}
      <div className={cn(eyebrow || title || subtitle || actions ? 'mt-6' : '')}>{children}</div>
    </section>
  )
}

function HeroStat({ label, value, detail, tintClassName }: HeroStatProps) {
  return (
    <div className={cn('rounded-[26px] border p-4 shadow-[inset_0_1px_0_rgba(255,255,255,0.55)]', tintClassName)}>
      <div className="text-[11px] font-medium uppercase tracking-[0.24em] text-slate-400">{label}</div>
      <div className="mt-3 text-2xl font-semibold tracking-[-0.04em] text-slate-50">{value}</div>
      <div className="mt-2 text-sm leading-6 text-slate-300">{detail}</div>
    </div>
  )
}

function StatusTile({ label, state, detail, tone }: StatusTileProps) {
  const toneClasses =
    tone === 'emerald'
      ? {
          panel: 'border-emerald-400/20 bg-gradient-to-br from-emerald-500/12 via-black/94 to-black/98',
          badge: 'border border-emerald-400/20 bg-emerald-400/12 text-emerald-200',
          dot: 'bg-emerald-300',
        }
      : tone === 'amber'
        ? {
            panel: 'border-amber-400/20 bg-gradient-to-br from-amber-500/12 via-black/94 to-black/98',
            badge: 'border border-amber-400/20 bg-amber-400/12 text-amber-200',
            dot: 'bg-amber-300',
          }
        : {
            panel: 'border-white/10 bg-gradient-to-br from-slate-800/72 to-black/98',
            badge: 'border border-white/10 bg-white/8 text-slate-200',
            dot: 'bg-slate-400',
          }

  return (
    <div className={cn('rounded-[24px] border p-4 shadow-[inset_0_1px_0_rgba(255,255,255,0.55)]', toneClasses.panel)}>
      <div className="flex items-center justify-between gap-3">
        <div className="text-sm font-medium text-slate-100">{label}</div>
        <div className={cn('inline-flex items-center gap-2 rounded-full px-3 py-1 text-[11px] font-medium uppercase tracking-[0.18em]', toneClasses.badge)}>
          <span className={cn('h-2 w-2 rounded-full', toneClasses.dot)} />
          {state}
        </div>
      </div>
      <div className="mt-4 text-sm leading-6 text-slate-300">{detail}</div>
    </div>
  )
}

function RateCard({ label, value, detail, tintClassName, chartClassName, children }: RateCardProps) {
  return (
    <div className={cn('rounded-[28px] border p-4 shadow-[inset_0_1px_0_rgba(255,255,255,0.55)] sm:p-5', tintClassName)}>
      <div className="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
        <div>
          <div className="text-[11px] font-medium uppercase tracking-[0.24em] text-slate-400">{label}</div>
          <div className="mt-3 text-3xl font-semibold tracking-[-0.05em] text-slate-50 tabular-nums">{value}</div>
        </div>
        <div className="rounded-full border border-white/10 bg-black/72 px-3 py-1 text-xs text-slate-300">{detail}</div>
      </div>
      <div className={cn('mt-5 rounded-[22px] border border-white/10 p-3 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]', chartClassName)}>
        {children}
      </div>
    </div>
  )
}

function TelemetryLane({ label, value, detail, tintClassName, children }: TelemetryLaneProps) {
  return (
    <div className={cn('rounded-[22px] border p-4 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]', tintClassName)}>
      <div className="flex items-start justify-between gap-4">
        <div>
          <div className="text-sm font-medium text-slate-100">{label}</div>
          <div className="mt-2 text-2xl font-semibold tracking-[-0.04em] text-slate-50 tabular-nums">{value}</div>
        </div>
        <div className="max-w-[9rem] text-right text-xs leading-5 text-slate-400">{detail}</div>
      </div>
      <div className="mt-3">{children}</div>
    </div>
  )
}

function Toggle({ checked, onChange }: ToggleProps) {
  return (
    <label className="relative inline-flex cursor-pointer items-center">
      <input type="checkbox" className="peer sr-only" checked={checked} onChange={onChange} />
      <span className="h-7 w-12 rounded-full border border-slate-700/80 bg-slate-900/88 transition peer-checked:border-cyan-400/30 peer-checked:bg-cyan-400/16 peer-focus-visible:ring-4 peer-focus-visible:ring-cyan-400/10" />
      <span className="pointer-events-none absolute left-1 h-5 w-5 rounded-full bg-slate-300 shadow-[0_6px_18px_rgba(2,6,23,0.4)] transition peer-checked:translate-x-5 peer-checked:bg-cyan-200" />
    </label>
  )
}

export default function App() {
  const [health, setHealth] = useState<ApiHealth | null>(null)
  const [config, setConfig] = useState<ApiConfig | null>(null)
  const [metrics, setMetrics] = useState<ApiMetrics | null>(null)
  const [history, setHistory] = useState<RatePoint[]>([])
  const [devices, setDevices] = useState<AudioDevice[]>([])

  const [err, setErr] = useState<string | null>(null)
  const [restartHint, setRestartHint] = useState<string | null>(null)
  const [showVisualizer, setShowVisualizer] = useState(false)

  const esRef = useRef<EventSource | null>(null)

  const closeVisualizer = useCallback(() => setShowVisualizer(false), [])

  const keyName = useMemo(() => {
    const midi = config?.music?.key_midi
    if (typeof midi !== 'number' || !Number.isFinite(midi)) return 'n/a'
    return midiToName(midi)
  }, [config?.music?.key_midi])

  const scaleLabel = useMemo(() => {
    const scale = config?.music?.scale
    return scale ? SCALE_LABELS[scale] ?? scale : '—'
  }, [config?.music?.scale])

  const currentPreset = useMemo(
    () => PRESET_OPTIONS.find((preset) => preset.id === config?.music?.preset) ?? PRESET_OPTIONS[0],
    [config?.music?.preset],
  )

  const activeOutputs = useMemo(() => {
    const outputs = []
    if (config?.features?.audio) outputs.push('audio')
    if (config?.features?.midi) outputs.push('midi')
    if (config?.features?.osc) outputs.push('osc')
    if (config?.features?.fake) outputs.push('fake')
    return outputs.length ? outputs.join(' + ') : 'none'
  }, [config?.features])

  const updatedAt = metrics?.ts_ms ? new Date(metrics.ts_ms).toLocaleTimeString([], { hour12: false }) : 'waiting for stream'

  const recentHistory = history.slice(-72)
  const execWindow = getWindowStats(recentHistory, 'exec_s')
  const schedulerWindow = getWindowStats(recentHistory, 'csw_s')
  const networkWindow = {
    rx: getWindowStats(recentHistory, 'rx_kbs'),
    tx: getWindowStats(recentHistory, 'tx_kbs'),
  }
  const blockWindow = {
    read: getWindowStats(recentHistory, 'blk_r_kbs'),
    write: getWindowStats(recentHistory, 'blk_w_kbs'),
  }
  const retxWindow = getWindowStats(recentHistory, 'retx_s')
  const irqWindow = getWindowStats(recentHistory, 'irq_s')
  const memWindow = getWindowStats(recentHistory, 'mem_pct')

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

  async function refreshAll() {
    await Promise.all([refreshHealth(), refreshConfig(), refreshDevices()])
  }

  async function putConfig(patch: unknown) {
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

    void boot()

    const id = window.setInterval(() => {
      void refreshHealth().catch(() => {})
    }, 1200)

    return () => {
      alive = false
      window.clearInterval(id)
    }
  }, [])

  useEffect(() => {
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
      void fetchJson<ApiMetricsResponse>(api('/api/metrics'))
        .then((m) => {
          if (!alive) return
          setMetrics(m)
          setErr(null)
          if (Array.isArray(m.history)) {
            setHistory(m.history)
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
    <div className="relative min-h-dvh overflow-hidden text-slate-100">
      <div className="pointer-events-none absolute inset-0 bg-[radial-gradient(circle_at_top_left,rgba(45,212,191,0.14),transparent_34%),radial-gradient(circle_at_top_right,rgba(251,191,36,0.08),transparent_30%),radial-gradient(circle_at_82%_82%,rgba(56,189,248,0.12),transparent_26%)]" />

      <main className="app-shell relative mx-auto flex w-full max-w-none flex-col gap-4 px-3 py-5 sm:px-4 lg:px-5 xl:px-6">
        <header className="reveal grid gap-6 xl:grid-cols-[1.15fr_0.85fr]">
          <section className="rounded-[34px] border border-white/10 bg-black/72 p-6 shadow-[0_24px_70px_rgba(2,6,23,0.58)] backdrop-blur-xl sm:p-8">
            <div className="flex flex-wrap items-center gap-3">
              <div className="inline-flex items-center gap-2 rounded-full border border-white/10 bg-slate-900/84 px-4 py-2 text-xs text-slate-300 shadow-[0_12px_30px_rgba(2,6,23,0.3)]">
                <span className={cn('h-2.5 w-2.5 rounded-full', err ? 'bg-amber-500' : 'bg-emerald-500')} />
                <span className="font-semibold">khor</span>
                <span className="text-slate-500">API</span>
                <span className="font-mono text-slate-200">{API_HINT}</span>
              </div>
              <div className="rounded-full border border-white/10 bg-slate-900/70 px-3 py-2 text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">
                native linux music engine
              </div>
              <button
                onClick={() => setShowVisualizer(true)}
                className="rounded-full border border-cyan-400/20 bg-cyan-400/10 px-4 py-2 text-xs font-semibold text-cyan-200 shadow-[0_12px_30px_rgba(8,47,73,0.3)] transition-colors hover:bg-cyan-400/18"
              >
                Visualizer
              </button>
            </div>

            <div className="mt-10 max-w-3xl">
              <div className="text-[11px] font-medium uppercase tracking-[0.32em] text-slate-500">Realtime kernel orchestra</div>
              <h1 className="mt-4 max-w-3xl text-4xl font-semibold leading-none tracking-[-0.06em] text-slate-50 sm:text-5xl lg:text-[4.5rem]">
                Kernel activity,
                <br />
                performed as music.
              </h1>
              <p className="mt-5 max-w-2xl text-base leading-7 text-slate-300 sm:text-lg">
                Live eBPF rates drive a deterministic sequencer. Audio stays local, while MIDI and OSC remain optional adapters for
                routing the same signal into external tools.
              </p>
            </div>

            <div className="mt-8 grid gap-3 lg:grid-cols-3">
              <HeroStat
                label="Daemon"
                value={err ? 'Attention' : 'Streaming'}
                detail={
                  err ? (
                    <span>{err}</span>
                  ) : (
                    <span>
                      Updated <span className="font-mono text-slate-100">{updatedAt}</span>
                    </span>
                  )
                }
                tintClassName={err ? 'border-amber-400/20 bg-gradient-to-br from-amber-500/12 via-black/92 to-slate-950/96' : 'border-emerald-400/20 bg-gradient-to-br from-emerald-500/12 via-black/92 to-slate-950/96'}
              />
              <HeroStat
                label="Sequence"
                value={currentPreset.label}
                detail={
                  <span>
                    <span className="font-mono text-slate-100">{config ? Math.round(config.music.bpm) : '—'} BPM</span> · {keyName} · {scaleLabel}
                  </span>
                }
                tintClassName="border-white/10 bg-gradient-to-br from-slate-800/70 to-slate-950/96"
              />
              <HeroStat
                label="Outputs"
                value={activeOutputs}
                detail={
                  <span>
                    audio {health?.audio.ok ? 'live' : 'idle'} · midi {health?.midi.ok ? 'live' : 'off'} · osc {health?.osc.ok ? 'live' : 'off'}
                  </span>
                }
                tintClassName="border-sky-400/20 bg-gradient-to-br from-sky-500/12 via-black/92 to-slate-950/96"
              />
            </div>
          </section>

          <section className="reveal reveal-2 rounded-[34px] border border-white/10 bg-slate-950/70 p-5 shadow-[0_24px_70px_rgba(2,6,23,0.58)] backdrop-blur-xl sm:p-6">
            <div className="flex items-start justify-between gap-4">
              <div>
                <div className="text-[11px] font-medium uppercase tracking-[0.28em] text-slate-500">Live telemetry</div>
                <h2 className="mt-2 text-2xl font-semibold tracking-[-0.04em] text-slate-50">Rate monitor</h2>
                <p className="mt-2 max-w-sm text-sm leading-6 text-slate-300">A quick read on what the kernel is doing right now and how much of the musical mix each source owns.</p>
              </div>
              <div className="rounded-[26px] border border-cyan-400/15 bg-cyan-400/10 px-4 py-4 text-white shadow-[0_18px_40px_rgba(8,47,73,0.28)]">
                <div className="text-[11px] font-medium uppercase tracking-[0.24em] text-cyan-100/60">events total</div>
                <div className="mt-2 text-3xl font-semibold tracking-[-0.04em] tabular-nums">{formatCompact(metrics?.totals.events_total)}</div>
              </div>
            </div>

            <div className="mt-6 grid gap-3 sm:grid-cols-3">
              <div className="rounded-[20px] border border-white/10 bg-slate-900/78 px-4 py-3">
                <div className="text-[11px] uppercase tracking-[0.2em] text-slate-500">ringbuf lost</div>
                <div className="mt-2 text-xl font-semibold tabular-nums text-slate-50">{metrics?.totals.events_dropped ?? 0}</div>
              </div>
              <div className="rounded-[20px] border border-white/10 bg-slate-900/78 px-4 py-3">
                <div className="text-[11px] uppercase tracking-[0.2em] text-slate-500">density</div>
                <div className="mt-2 text-xl font-semibold tabular-nums text-slate-50">{config ? config.music.density.toFixed(2) : '—'}</div>
              </div>
              <div className="rounded-[20px] border border-white/10 bg-slate-900/78 px-4 py-3">
                <div className="text-[11px] uppercase tracking-[0.2em] text-slate-500">smoothing</div>
                <div className="mt-2 text-xl font-semibold tabular-nums text-slate-50">{config ? config.music.smoothing.toFixed(2) : '—'}</div>
              </div>
            </div>

            <div className="mt-4 grid gap-3 sm:grid-cols-2">
              <TelemetryLane
                label="Exec cadence"
                value={formatRate(metrics?.rates.exec_s)}
                detail={
                  <span>
                    peak {formatRate(execWindow.peak)}
                    <br />
                    avg {formatRate(execWindow.average)}
                  </span>
                }
                tintClassName="border-emerald-400/18 bg-gradient-to-br from-emerald-500/10 via-black/92 to-black/98"
              >
                <div className="text-emerald-300">
                  <Sparkline points={recentHistory} k="exec_s" />
                </div>
              </TelemetryLane>

              <TelemetryLane
                label="Network"
                value={metrics ? `${formatRate(metrics.rates.rx_kbs)} / ${formatRate(metrics.rates.tx_kbs)}` : '—'}
                detail={
                  <span>
                    rx peak {formatRate(networkWindow.rx.peak)}
                    <br />
                    tx peak {formatRate(networkWindow.tx.peak)}
                  </span>
                }
                tintClassName="border-rose-400/18 bg-gradient-to-br from-rose-500/10 via-black/92 to-black/98"
              >
                <div className="grid grid-cols-2 gap-2.5">
                  <div className="text-rose-300">
                    <Sparkline points={recentHistory} k="rx_kbs" />
                  </div>
                  <div className="text-sky-300">
                    <Sparkline points={recentHistory} k="tx_kbs" />
                  </div>
                </div>
              </TelemetryLane>

              <TelemetryLane
                label="Scheduler"
                value={formatRate(metrics?.rates.csw_s, 0)}
                detail={
                  <span>
                    peak {formatRate(schedulerWindow.peak, 0)}
                    <br />
                    avg {formatRate(schedulerWindow.average, 0)}
                  </span>
                }
                tintClassName="border-white/10 bg-gradient-to-br from-slate-800/72 to-black/98"
              >
                <div className="text-slate-200">
                  <Sparkline points={recentHistory} k="csw_s" />
                </div>
              </TelemetryLane>

              <TelemetryLane
                label="Block io"
                value={metrics ? `${formatRate(metrics.rates.blk_r_kbs)} / ${formatRate(metrics.rates.blk_w_kbs)}` : '—'}
                detail={
                  <span>
                    r peak {formatRate(blockWindow.read.peak)}
                    <br />
                    w peak {formatRate(blockWindow.write.peak)}
                  </span>
                }
                tintClassName="border-sky-400/18 bg-gradient-to-br from-sky-500/10 via-black/92 to-black/98"
              >
                <div className="grid grid-cols-2 gap-2.5">
                  <div className="text-amber-300">
                    <Sparkline points={recentHistory} k="blk_r_kbs" />
                  </div>
                  <div className="text-cyan-300">
                    <Sparkline points={recentHistory} k="blk_w_kbs" />
                  </div>
                </div>
              </TelemetryLane>

              <TelemetryLane
                label="TCP retransmits"
                value={formatRate(metrics?.rates.retx_s)}
                detail={<span>peak {formatRate(retxWindow.peak)}</span>}
                tintClassName="border-red-400/18 bg-gradient-to-br from-red-500/10 via-black/92 to-black/98"
              >
                <div className="text-red-300">
                  <Sparkline points={recentHistory} k="retx_s" />
                </div>
              </TelemetryLane>

              <TelemetryLane
                label="Interrupts"
                value={formatRate(metrics?.rates.irq_s, 0)}
                detail={<span>peak {formatRate(irqWindow.peak, 0)}</span>}
                tintClassName="border-violet-400/18 bg-gradient-to-br from-violet-500/10 via-black/92 to-black/98"
              >
                <div className="text-violet-300">
                  <Sparkline points={recentHistory} k="irq_s" />
                </div>
              </TelemetryLane>

              <TelemetryLane
                label="Memory pressure"
                value={metrics ? `${formatRate(metrics.rates.mem_pct)}%` : '—'}
                detail={<span>peak {formatRate(memWindow.peak)}%</span>}
                tintClassName="border-fuchsia-400/18 bg-gradient-to-br from-fuchsia-500/10 via-black/92 to-black/98"
              >
                <div className="text-fuchsia-300">
                  <Sparkline points={recentHistory} k="mem_pct" />
                </div>
              </TelemetryLane>
            </div>
          </section>
        </header>

        {err ? (
          <div className="reveal reveal-2 rounded-[26px] border border-amber-400/18 bg-gradient-to-r from-amber-500/10 to-slate-950/82 p-4 shadow-[0_18px_45px_rgba(120,53,15,0.28)]">
            <div className="text-sm font-semibold text-amber-200">Daemon not reachable</div>
            <div className="mt-1 text-sm leading-6 text-amber-100/80">
              {err}. Start it with <span className="rounded bg-slate-900/88 px-2 py-1 font-mono text-xs text-slate-100">./scripts/linux-run.sh</span>.
            </div>
          </div>
        ) : null}

        {restartHint ? (
          <div className="reveal reveal-2 rounded-[26px] border border-sky-400/18 bg-gradient-to-r from-sky-500/10 to-slate-950/82 p-4 shadow-[0_18px_45px_rgba(12,74,110,0.26)]">
            <div className="text-sm font-semibold text-sky-200">Restart required</div>
            <div className="mt-1 text-sm leading-6 text-slate-300">{restartHint}</div>
          </div>
        ) : null}

        <section className="reveal reveal-3 grid gap-6 xl:grid-cols-[0.86fr_1.14fr]">
          <SectionPanel
            eyebrow="Runtime"
            title="System health"
            subtitle="Status from the daemon and adapters. Errors stay visible here so you can see whether a failure is in eBPF collection or an output layer."
          >
            <div className="grid gap-3 sm:grid-cols-2">
              <StatusTile
                label="eBPF"
                state={health ? (health.bpf.ok ? 'enabled' : health.bpf.enabled ? 'error' : 'disabled') : 'pending'}
                detail={health?.bpf.error ?? 'Kernel collectors are attached and feeding live event rates.'}
                tone={health?.bpf.ok ? 'emerald' : health?.bpf.enabled ? 'amber' : 'slate'}
              />
              <StatusTile
                label="Audio"
                state={health ? (health.audio.ok ? 'running' : health.audio.enabled ? 'error' : 'disabled') : 'pending'}
                detail={
                  health?.audio.error ? (
                    health.audio.error
                  ) : (
                    <span>
                      <span className="font-mono text-slate-100">{health?.audio.backend || 'backend n/a'}</span>
                      {health?.audio.device ? <span> on {health.audio.device}</span> : null}
                    </span>
                  )
                }
                tone={health?.audio.ok ? 'emerald' : health?.audio.enabled ? 'amber' : 'slate'}
              />
              <StatusTile
                label="MIDI"
                state={health ? (health.midi.ok ? 'enabled' : health.midi.enabled ? 'error' : 'off') : 'pending'}
                detail={health?.midi.error ?? 'Virtual ALSA sequencer port stays ready for external synth routing.'}
                tone={health?.midi.ok ? 'emerald' : health?.midi.enabled ? 'amber' : 'slate'}
              />
              <StatusTile
                label="OSC"
                state={health ? (health.osc.ok ? 'enabled' : health.osc.enabled ? 'error' : 'off') : 'pending'}
                detail={health?.osc.error ?? 'UDP note and signal messages for remote consumers and visualizers.'}
                tone={health?.osc.ok ? 'emerald' : health?.osc.enabled ? 'amber' : 'slate'}
              />
            </div>

            <div className="mt-5 rounded-[24px] border border-white/10 bg-slate-900/90 px-4 py-4 text-white shadow-[0_18px_40px_rgba(2,6,23,0.4)]">
              <div className="text-[11px] font-medium uppercase tracking-[0.24em] text-white/55">Config path</div>
              <div className="mt-3 break-all font-mono text-sm leading-6 text-white/85">{health?.config_path ?? '—'}</div>
            </div>
          </SectionPanel>

          <SectionPanel
            eyebrow="Signals"
            title="Live lanes"
            subtitle="Independent signal groups get their own card so the activity patterns remain readable even when totals spike."
          >
            <div className="grid gap-4 lg:grid-cols-2">
              <RateCard
                label="exec / s"
                value={formatRate(metrics?.rates.exec_s)}
                detail="process launches"
                tintClassName="border-emerald-400/18 bg-gradient-to-br from-emerald-500/10 via-black/92 to-black/98"
                chartClassName="bg-emerald-400/[0.05]"
              >
                <div className="text-emerald-300">
                  <Sparkline points={history} k="exec_s" />
                </div>
              </RateCard>

              <RateCard
                label="network"
                value={metrics ? `${formatRate(metrics.rates.rx_kbs)} / ${formatRate(metrics.rates.tx_kbs)}` : '—'}
                detail="rx / tx kB/s"
                tintClassName="border-rose-400/18 bg-gradient-to-br from-rose-500/10 via-black/92 to-black/98"
                chartClassName="bg-rose-400/[0.05]"
              >
                <div className="grid grid-cols-2 gap-3">
                  <div className="text-rose-300">
                    <Sparkline points={history} k="rx_kbs" />
                  </div>
                  <div className="text-sky-300">
                    <Sparkline points={history} k="tx_kbs" />
                  </div>
                </div>
              </RateCard>

              <RateCard
                label="scheduler"
                value={formatRate(metrics?.rates.csw_s, 0)}
                detail="context switches / s"
                tintClassName="border-white/10 bg-gradient-to-br from-slate-800/72 to-black/98"
                chartClassName="bg-white/[0.03]"
              >
                <div className="text-slate-200">
                  <Sparkline points={history} k="csw_s" />
                </div>
              </RateCard>

              <RateCard
                label="block io"
                value={metrics ? `${formatRate(metrics.rates.blk_r_kbs)} / ${formatRate(metrics.rates.blk_w_kbs)}` : '—'}
                detail="read / write kB/s"
                tintClassName="border-sky-400/18 bg-gradient-to-br from-sky-500/10 via-black/92 to-black/98"
                chartClassName="bg-sky-400/[0.05]"
              >
                <div className="grid grid-cols-2 gap-3">
                  <div className="text-amber-300">
                    <Sparkline points={history} k="blk_r_kbs" />
                  </div>
                  <div className="text-cyan-300">
                    <Sparkline points={history} k="blk_w_kbs" />
                  </div>
                </div>
              </RateCard>

              <RateCard
                label="retransmits"
                value={formatRate(metrics?.rates.retx_s)}
                detail="tcp retransmits / s"
                tintClassName="border-red-400/18 bg-gradient-to-br from-red-500/10 via-black/92 to-black/98"
                chartClassName="bg-red-400/[0.05]"
              >
                <div className="text-red-300">
                  <Sparkline points={history} k="retx_s" />
                </div>
              </RateCard>

              <RateCard
                label="interrupts"
                value={formatRate(metrics?.rates.irq_s, 0)}
                detail="hardware IRQs / s"
                tintClassName="border-violet-400/18 bg-gradient-to-br from-violet-500/10 via-black/92 to-black/98"
                chartClassName="bg-violet-400/[0.05]"
              >
                <div className="text-violet-300">
                  <Sparkline points={history} k="irq_s" />
                </div>
              </RateCard>

              <RateCard
                label="memory pressure"
                value={metrics ? `${formatRate(metrics.rates.mem_pct)}%` : '—'}
                detail="PSI some avg10"
                tintClassName="border-fuchsia-400/18 bg-gradient-to-br from-fuchsia-500/10 via-black/92 to-black/98"
                chartClassName="bg-fuchsia-400/[0.05]"
              >
                <div className="text-fuchsia-300">
                  <Sparkline points={history} k="mem_pct" />
                </div>
              </RateCard>
            </div>
          </SectionPanel>
        </section>

        <section className="reveal reveal-4 grid gap-4 xl:grid-cols-[minmax(0,1.12fr)_minmax(0,0.88fr)]">
          <SectionPanel
            eyebrow="Compose"
            title="Sequencer mapping"
            subtitle="Presets rewrite the mapping server-side. The control deck below lets you steer tempo, harmonic center, scale choice, and response curve."
          >
            <div className="grid gap-3 xl:grid-cols-[minmax(0,1.08fr)_minmax(0,0.92fr)]">
              <div className="grid min-w-0 gap-3 sm:grid-cols-2">
                {PRESET_OPTIONS.map((preset) => (
                  <button
                    key={preset.id}
                    className={cn(
                      'min-w-0 rounded-[20px] border p-3 text-left shadow-[inset_0_1px_0_rgba(255,255,255,0.55)]',
                      config?.music?.preset === preset.id
                        ? 'border-cyan-400/25 bg-slate-950 text-white shadow-[0_16px_30px_rgba(8,47,73,0.24)]'
                        : 'border-white/10 bg-slate-900/72 text-slate-100 hover:border-white/20 hover:bg-slate-900/90',
                    )}
                    onClick={() =>
                      void post(`/api/preset/select?name=${encodeURIComponent(preset.id)}`)
                        .then(() => refreshAll())
                        .catch((e) => setErr(e instanceof Error ? e.message : 'failed'))
                    }
                  >
                    <div className="flex items-center justify-between gap-3">
                      <div className="text-base font-semibold tracking-[-0.03em]">{preset.label}</div>
                      <div
                        className={cn(
                          'shrink-0 rounded-full px-2.5 py-1 text-[10px] font-medium uppercase tracking-[0.16em]',
                          config?.music?.preset === preset.id ? 'bg-cyan-400/14 text-cyan-100' : 'bg-white/8 text-slate-400',
                        )}
                      >
                        preset
                      </div>
                    </div>
                    <div className={cn('mt-2 text-xs leading-5', config?.music?.preset === preset.id ? 'text-white/72' : 'text-slate-300')}>
                      {preset.description}
                    </div>
                  </button>
                ))}
              </div>

              <div className="min-w-0 rounded-[24px] border border-white/10 bg-gradient-to-br from-slate-900/82 to-slate-950/96 p-4 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                <div className="flex items-start justify-between gap-3">
                  <div className="min-w-0">
                    <div className="text-[11px] font-medium uppercase tracking-[0.24em] text-slate-500">Current scene</div>
                    <div className="mt-2 text-xl font-semibold tracking-[-0.04em] text-slate-50">{currentPreset.label}</div>
                    <div className="mt-2 text-sm leading-5 text-slate-300">{currentPreset.description}</div>
                  </div>
                  <div className="shrink-0 rounded-[20px] border border-cyan-400/18 bg-cyan-400/10 px-3 py-2.5 text-white">
                    <div className="text-[11px] uppercase tracking-[0.2em] text-cyan-100/60">tempo</div>
                    <div className="mt-1.5 text-2xl font-semibold tabular-nums">{config ? Math.round(config.music.bpm) : '—'}</div>
                  </div>
                </div>

                <div className="mt-4 grid gap-2.5 sm:grid-cols-2">
                  <div className={insetMetricCardClassName}>
                    <div className="text-[11px] uppercase tracking-[0.18em] text-slate-500">Key</div>
                    <div className="mt-1.5 text-base font-semibold text-slate-100">{keyName}</div>
                  </div>
                  <div className={insetMetricCardClassName}>
                    <div className="text-[11px] uppercase tracking-[0.18em] text-slate-500">Scale</div>
                    <div className="mt-1.5 text-base font-semibold text-slate-100">{scaleLabel}</div>
                  </div>
                  <div className={insetMetricCardClassName}>
                    <div className="text-[11px] uppercase tracking-[0.18em] text-slate-500">Density</div>
                    <div className="mt-1.5 text-base font-semibold tabular-nums text-slate-100">{config ? config.music.density.toFixed(2) : '—'}</div>
                  </div>
                  <div className={insetMetricCardClassName}>
                    <div className="text-[11px] uppercase tracking-[0.18em] text-slate-500">Smoothing</div>
                    <div className="mt-1.5 text-base font-semibold tabular-nums text-slate-100">{config ? config.music.smoothing.toFixed(2) : '—'}</div>
                  </div>
                </div>
              </div>
            </div>

            <div className="mt-4 grid gap-3 md:grid-cols-2 2xl:grid-cols-4">
              <label className={fieldPanelClassName}>
                <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">BPM</span>
                <input
                  className={fieldClassName}
                  inputMode="numeric"
                  value={config ? String(Math.round(config.music.bpm)) : '110'}
                  onChange={(e) => setConfig((c) => (c ? { ...c, music: { ...c.music, bpm: Number(e.target.value) } } : c))}
                  onBlur={() => void putConfig({ music: { bpm: config?.music?.bpm } }).catch((e) => setErr(String(e)))}
                />
              </label>

              <label className={fieldPanelClassName}>
                <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">Key MIDI</span>
                <input
                  className={fieldClassName}
                  inputMode="numeric"
                  value={config ? String(config.music.key_midi) : '62'}
                  onChange={(e) => setConfig((c) => (c ? { ...c, music: { ...c.music, key_midi: Number(e.target.value) } } : c))}
                  onBlur={() => void putConfig({ music: { key_midi: config?.music?.key_midi } }).catch((e) => setErr(String(e)))}
                />
                <span className="text-xs text-slate-500">{keyName}</span>
              </label>

              <label className={fieldPanelClassName}>
                <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">Scale</span>
                <select
                  className={fieldClassName}
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

              <label className={fieldPanelClassName}>
                <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">Density</span>
                <input
                  className={fieldClassName}
                  inputMode="decimal"
                  value={config ? String(config.music.density.toFixed(2)) : '0.35'}
                  onChange={(e) => setConfig((c) => (c ? { ...c, music: { ...c.music, density: Number(e.target.value) } } : c))}
                  onBlur={() => void putConfig({ music: { density: config?.music?.density } }).catch((e) => setErr(String(e)))}
                />
              </label>

              <label className={cn(fieldPanelClassName, 'md:col-span-2 2xl:col-span-3')}>
                <div className="flex items-center justify-between gap-3">
                  <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">Smoothing</span>
                  <span className="text-sm font-semibold tabular-nums text-slate-100">{config ? config.music.smoothing.toFixed(2) : '—'}</span>
                </div>
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
                  onPointerUp={() => void putConfig({ music: { smoothing: config?.music?.smoothing } }).catch((e) => setErr(String(e)))}
                />
              </label>

              <div className="flex items-stretch md:col-span-2 2xl:col-span-1">
                <button
                  className="h-full min-h-12 w-full rounded-[20px] border border-cyan-400/18 bg-cyan-400/12 px-4 py-3 text-sm font-semibold uppercase tracking-[0.18em] text-cyan-50 shadow-[0_20px_40px_rgba(8,47,73,0.28)]"
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
                  Apply mapping
                </button>
              </div>
            </div>
          </SectionPanel>

          <SectionPanel
            eyebrow="Outputs"
            title="Adapters and routing"
            subtitle="Local audio is the primary voice. MIDI and OSC are optional mirrors, while fake mode gives you something to hear when eBPF cannot attach."
          >
            <div className="grid gap-3 lg:grid-cols-[minmax(0,1.04fr)_minmax(0,0.96fr)]">
              <div className="grid min-w-0 gap-3">
                <div className="min-w-0 rounded-[24px] border border-emerald-400/18 bg-gradient-to-br from-emerald-500/10 via-black/92 to-black/98 p-4 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                  <div className="flex items-start justify-between gap-4">
                    <div className="min-w-0">
                      <div className="text-base font-semibold tracking-[-0.03em] text-slate-50">Audio</div>
                      <div className="mt-1 text-sm leading-5 text-slate-300">Local playback through miniaudio using PulseAudio, PipeWire, or ALSA.</div>
                    </div>
                    <Toggle
                      checked={config?.features?.audio ?? true}
                      onChange={(e) => void putConfig({ features: { audio: e.target.checked } }).then(refreshHealth).catch((x) => setErr(String(x)))}
                    />
                  </div>

                  <div className="mt-4 grid gap-3">
                    <label className="grid gap-2.5 rounded-[18px] border border-white/10 bg-black/68 p-3.5 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                      <div className="flex items-center justify-between gap-3">
                        <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">Master gain</span>
                        <span className="text-sm font-semibold tabular-nums text-slate-100">{config ? config.audio.master_gain.toFixed(2) : '—'}</span>
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
                        onPointerUp={() => void putConfig({ audio: { master_gain: config?.audio?.master_gain } }).catch((e) => setErr(String(e)))}
                      />
                    </label>

                    <div className="rounded-[18px] border border-white/10 bg-black/68 p-3.5 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                      <div className="flex items-center justify-between gap-3">
                        <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">Device</span>
                        <button
                          className="rounded-full border border-white/10 bg-slate-900/88 px-3 py-1 text-xs font-medium text-slate-200"
                          onClick={() => void refreshDevices().catch(() => {})}
                        >
                          refresh
                        </button>
                      </div>
                      <select
                        className={cn(fieldClassName, 'mt-2.5')}
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
                  </div>
                </div>

                <div className="min-w-0 rounded-[24px] border border-amber-400/18 bg-gradient-to-br from-amber-500/10 via-black/92 to-black/98 p-4 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                  <div className="flex items-start justify-between gap-3">
                    <div className="min-w-0">
                      <div className="text-base font-semibold tracking-[-0.03em] text-slate-50">Fake mode</div>
                      <div className="mt-1 text-sm leading-5 text-slate-300">Generates signal locally when eBPF cannot attach.</div>
                    </div>
                    <Toggle
                      checked={config?.features?.fake ?? false}
                      onChange={(e) => void putConfig({ features: { fake: e.target.checked } }).then(refreshHealth).catch((x) => setErr(String(x)))}
                    />
                  </div>
                </div>
              </div>

              <div className="grid min-w-0 gap-3">
                <div className="min-w-0 rounded-[24px] border border-white/10 bg-gradient-to-br from-slate-800/72 to-black/98 p-4 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                  <div className="flex items-start justify-between gap-4">
                    <div className="min-w-0">
                      <div className="text-base font-semibold tracking-[-0.03em] text-slate-50">MIDI</div>
                      <div className="mt-1 text-sm leading-5 text-slate-300">Virtual ALSA sequencer port for external synths or DAWs.</div>
                    </div>
                    <Toggle
                      checked={config?.features?.midi ?? false}
                      onChange={(e) => void putConfig({ features: { midi: e.target.checked } }).then(refreshHealth).catch((x) => setErr(String(x)))}
                    />
                  </div>

                  <div className="mt-4 grid gap-2.5">
                    <label className="grid gap-2 rounded-[18px] border border-white/10 bg-black/68 p-3.5 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                      <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">Port name</span>
                      <input
                        className={fieldClassName}
                        value={config?.midi?.port ?? 'khor'}
                        onChange={(e) => setConfig((c) => (c ? { ...c, midi: { ...c.midi, port: e.target.value } } : c))}
                        onBlur={() => void putConfig({ midi: { port: config?.midi?.port } }).catch((x) => setErr(String(x)))}
                      />
                    </label>

                    <label className="grid gap-2 rounded-[18px] border border-white/10 bg-black/68 p-3.5 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                      <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">Channel</span>
                      <input
                        className={fieldClassName}
                        inputMode="numeric"
                        value={config ? String(config.midi.channel) : '1'}
                        onChange={(e) => setConfig((c) => (c ? { ...c, midi: { ...c.midi, channel: Number(e.target.value) } } : c))}
                        onBlur={() => void putConfig({ midi: { channel: config?.midi?.channel } }).catch((x) => setErr(String(x)))}
                      />
                    </label>
                  </div>
                </div>

                <div className="min-w-0 rounded-[24px] border border-sky-400/18 bg-gradient-to-br from-sky-500/10 via-black/92 to-black/98 p-4 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                  <div className="flex items-start justify-between gap-4">
                    <div className="min-w-0">
                      <div className="text-base font-semibold tracking-[-0.03em] text-slate-50">OSC</div>
                      <div className="mt-1 text-sm leading-5 text-slate-300">UDP messages under `/khor/note` and `/khor/signal` for remote consumers.</div>
                    </div>
                    <Toggle
                      checked={config?.features?.osc ?? false}
                      onChange={(e) => void putConfig({ features: { osc: e.target.checked } }).then(refreshHealth).catch((x) => setErr(String(x)))}
                    />
                  </div>

                  <div className="mt-4 grid gap-2.5">
                    <label className="grid gap-2 rounded-[18px] border border-white/10 bg-black/68 p-3.5 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                      <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">Host</span>
                      <input
                        className={fieldClassName}
                        value={config?.osc?.host ?? '127.0.0.1'}
                        onChange={(e) => setConfig((c) => (c ? { ...c, osc: { ...c.osc, host: e.target.value } } : c))}
                        onBlur={() => void putConfig({ osc: { host: config?.osc?.host } }).catch((x) => setErr(String(x)))}
                      />
                    </label>

                    <label className="grid gap-2 rounded-[18px] border border-white/10 bg-black/68 p-3.5 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                      <span className="text-[11px] font-medium uppercase tracking-[0.2em] text-slate-500">Port</span>
                      <input
                        className={fieldClassName}
                        inputMode="numeric"
                        value={config ? String(config.osc.port) : '9000'}
                        onChange={(e) => setConfig((c) => (c ? { ...c, osc: { ...c.osc, port: Number(e.target.value) } } : c))}
                        onBlur={() => void putConfig({ osc: { port: config?.osc?.port } }).catch((x) => setErr(String(x)))}
                      />
                    </label>
                  </div>
                </div>
              </div>
            </div>
          </SectionPanel>
        </section>

        <SectionPanel
          eyebrow="Debug"
          title="Operations"
          subtitle="Quick actions for verification and a reminder for the one-time capability setup required to run eBPF without root at runtime."
          className="reveal reveal-4"
          actions={
            <div className="flex flex-wrap gap-2">
              <button
                className="rounded-full border border-cyan-400/18 bg-cyan-400/12 px-4 py-2 text-sm font-semibold text-cyan-50 shadow-[0_18px_40px_rgba(8,47,73,0.24)]"
                onClick={() =>
                  void post('/api/actions/test_note')
                    .then(() => setErr(null))
                    .catch((e) => setErr(e instanceof Error ? e.message : 'failed'))
                }
              >
                Play test note
              </button>
              <button
                className="rounded-full border border-white/10 bg-slate-900/88 px-4 py-2 text-sm font-semibold text-slate-100"
                onClick={() => {
                  setRestartHint(null)
                  void refreshAll().catch(() => {})
                }}
              >
                Refresh
              </button>
            </div>
          }
        >
          <div className="grid gap-4 lg:grid-cols-[0.9fr_1.1fr]">
            <div className="rounded-[26px] border border-white/10 bg-slate-900/90 px-5 py-5 text-white shadow-[0_20px_45px_rgba(2,6,23,0.38)]">
              <div className="text-[11px] font-medium uppercase tracking-[0.24em] text-white/55">Config file</div>
              <div className="mt-3 break-all font-mono text-sm leading-6 text-white/82">{health?.config_path ?? '—'}</div>
            </div>

            <div className="rounded-[26px] border border-white/10 bg-gradient-to-br from-slate-900/86 to-slate-950/96 p-5 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
              <div className="text-[11px] font-medium uppercase tracking-[0.24em] text-slate-500">Capability tip</div>
              <div className="mt-3 text-sm leading-7 text-slate-300">
                For eBPF without a root runtime, apply the one-time capability command below to the daemon binary.
              </div>
              <div className="mt-4 rounded-[20px] border border-white/10 bg-slate-950/86 px-4 py-4 font-mono text-xs leading-6 text-slate-100 shadow-[inset_0_1px_0_rgba(255,255,255,0.04)]">
                sudo setcap cap_bpf,cap_perfmon,cap_sys_resource,cap_sys_admin,cap_dac_read_search+ep ~/.local/bin/khor-daemon
              </div>
            </div>
          </div>
        </SectionPanel>
      </main>

      {showVisualizer && (
        <KhorVisualizer apiBase={API_BASE} onClose={closeVisualizer} />
      )}
    </div>
  )
}
