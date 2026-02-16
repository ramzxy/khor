import { useEffect, useMemo, useRef, useState } from 'react'
import { cn } from './lib/cn'

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
  controls: {
    bpm: number
    key_midi: number
    density: number
    smoothing: number
  }
  history?: Array<{
    ts_ms: number
    exec_s: number
    rx_kbs: number
    tx_kbs: number
    csw_s: number
    blk_r_kbs: number
    blk_w_kbs: number
  }>
}

const API_BASE = (import.meta as any).env?.VITE_API_BASE ?? 'http://127.0.0.1:17321'

function midiToName(midi: number) {
  const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
  const n = names[((midi % 12) + 12) % 12]
  const oct = Math.floor(midi / 12) - 1
  return `${n}${oct}`
}

function App() {
  const [metrics, setMetrics] = useState<ApiMetrics | null>(null)
  const [err, setErr] = useState<string | null>(null)
  const [bpmDraft, setBpmDraft] = useState('110')
  const [keyDraft, setKeyDraft] = useState('62')
  const [densityDraft, setDensityDraft] = useState('0.35')
  const [smoothingDraft, setSmoothingDraft] = useState('0.85')

  const prevRef = useRef<{ t: number; m: ApiMetrics } | null>(null)
  const [rates, setRates] = useState<{ exec: number; rxKBs: number; txKBs: number } | null>(null)

  async function fetchMetrics() {
    const r = await fetch(`${API_BASE}/api/metrics`)
    if (!r.ok) throw new Error(`HTTP ${r.status}`)
    return (await r.json()) as ApiMetrics
  }

  useEffect(() => {
    let alive = true
    const tick = async () => {
      try {
        const m = await fetchMetrics()
        if (!alive) return
        setMetrics(m)
        setErr(null)
        setBpmDraft(String(Math.round(m.controls.bpm)))
        setKeyDraft(String(m.controls.key_midi))
        setDensityDraft(String(m.controls.density.toFixed(2)))
        setSmoothingDraft(String(m.controls.smoothing.toFixed(2)))

        const now = Date.now()
        const prev = prevRef.current
        if (prev) {
          const dt = Math.max(1, now - prev.t) / 1000
          const exec = (m.totals.exec_total - prev.m.totals.exec_total) / dt
          const rxKBs = (m.totals.net_rx_bytes_total - prev.m.totals.net_rx_bytes_total) / dt / 1024
          const txKBs = (m.totals.net_tx_bytes_total - prev.m.totals.net_tx_bytes_total) / dt / 1024
          setRates({ exec, rxKBs, txKBs })
        }
        prevRef.current = { t: now, m }
      } catch (e) {
        if (!alive) return
        setErr(e instanceof Error ? e.message : 'failed to fetch')
      }
    }

    const id = window.setInterval(tick, 300)
    tick()
    return () => {
      alive = false
      window.clearInterval(id)
    }
  }, [])

  const keyName = useMemo(() => {
    const midi = Number(keyDraft)
    if (!Number.isFinite(midi)) return 'n/a'
    return midiToName(midi)
  }, [keyDraft])

  async function applyControl() {
    const bpm = Number(bpmDraft)
    const key = Number(keyDraft)
    const density = Number(densityDraft)
    const smoothing = Number(smoothingDraft)

    const body: any = { music: {} }
    if (Number.isFinite(bpm)) body.music.bpm = bpm
    if (Number.isFinite(key)) body.music.key_midi = key
    if (Number.isFinite(density)) body.music.density = density
    if (Number.isFinite(smoothing)) body.music.smoothing = smoothing

    await fetch(`${API_BASE}/api/config`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    })
  }

  return (
    <div className="min-h-dvh bg-zinc-50 text-zinc-950">
      <div className="mx-auto w-full max-w-6xl px-5 py-8">
        <header className="flex items-baseline justify-between gap-4">
          <div>
            <h1 className="text-balance text-2xl font-semibold">khor</h1>
            <p className="text-pretty text-sm text-zinc-600">
              Kernel activity to pitched synth. API base <span className="tabular-nums">{API_BASE}</span>
            </p>
          </div>
          <div className="text-right text-sm text-zinc-600">
            <div className="tabular-nums">
              {rates ? (
                <span>
                  exec/s {rates.exec.toFixed(1)} · rx {rates.rxKBs.toFixed(1)} KB/s · tx {rates.txKBs.toFixed(1)} KB/s
                </span>
              ) : (
                <span>polling…</span>
              )}
            </div>
            <div className="tabular-nums">dropped {metrics?.events_dropped ?? 0}</div>
          </div>
        </header>

        <div className="mt-7 grid gap-4 md:grid-cols-3">
          <section className="rounded-xl border border-zinc-200 bg-white p-4 shadow-sm">
            <h2 className="text-balance text-sm font-medium text-zinc-800">Totals</h2>
            <dl className="mt-3 grid gap-2 text-sm">
              <div className="flex items-center justify-between">
                <dt className="text-zinc-600">events</dt>
                <dd className="tabular-nums">{metrics?.totals.events_total ?? '—'}</dd>
              </div>
              <div className="flex items-center justify-between">
                <dt className="text-zinc-600">exec</dt>
                <dd className="tabular-nums">{metrics?.totals.exec_total ?? '—'}</dd>
              </div>
              <div className="flex items-center justify-between">
                <dt className="text-zinc-600">net rx</dt>
                <dd className="tabular-nums">
                  {metrics ? `${(metrics.totals.net_rx_bytes_total / 1024 / 1024).toFixed(1)} MiB` : '—'}
                </dd>
              </div>
              <div className="flex items-center justify-between">
                <dt className="text-zinc-600">net tx</dt>
                <dd className="tabular-nums">
                  {metrics ? `${(metrics.totals.net_tx_bytes_total / 1024 / 1024).toFixed(1)} MiB` : '—'}
                </dd>
              </div>
              <div className="flex items-center justify-between">
                <dt className="text-zinc-600">sched switch</dt>
                <dd className="tabular-nums">{metrics?.totals.sched_switch_total ?? '—'}</dd>
              </div>
              <div className="flex items-center justify-between">
                <dt className="text-zinc-600">block r/w</dt>
                <dd className="tabular-nums">
                  {metrics
                    ? `${(metrics.totals.blk_read_bytes_total / 1024 / 1024).toFixed(1)} / ${(metrics.totals.blk_write_bytes_total / 1024 / 1024).toFixed(1)} MiB`
                    : '—'}
                </dd>
              </div>
            </dl>
          </section>

          <section className="rounded-xl border border-zinc-200 bg-white p-4 shadow-sm md:col-span-2">
            <h2 className="text-balance text-sm font-medium text-zinc-800">Controls</h2>
            <div className="mt-3 grid gap-3 md:grid-cols-3">
              <label className="grid gap-1">
                <span className="text-xs text-zinc-600">BPM</span>
                <input
                  className="h-10 rounded-lg border border-zinc-200 bg-white px-3 text-sm tabular-nums outline-none focus:ring-2 focus:ring-zinc-900/10"
                  inputMode="numeric"
                  value={bpmDraft}
                  onChange={(e) => setBpmDraft(e.target.value)}
                />
              </label>
              <label className="grid gap-1">
                <span className="text-xs text-zinc-600">Key (MIDI)</span>
                <input
                  className="h-10 rounded-lg border border-zinc-200 bg-white px-3 text-sm tabular-nums outline-none focus:ring-2 focus:ring-zinc-900/10"
                  inputMode="numeric"
                  value={keyDraft}
                  onChange={(e) => setKeyDraft(e.target.value)}
                />
                <span className="text-xs text-zinc-500 tabular-nums">{keyName}</span>
              </label>
              <label className="grid gap-1">
                <span className="text-xs text-zinc-600">Density</span>
                <input
                  className="h-10 rounded-lg border border-zinc-200 bg-white px-3 text-sm tabular-nums outline-none focus:ring-2 focus:ring-zinc-900/10"
                  inputMode="decimal"
                  value={densityDraft}
                  onChange={(e) => setDensityDraft(e.target.value)}
                />
              </label>
              <label className="grid gap-1">
                <span className="text-xs text-zinc-600">Smoothing</span>
                <input
                  className="h-10 rounded-lg border border-zinc-200 bg-white px-3 text-sm tabular-nums outline-none focus:ring-2 focus:ring-zinc-900/10"
                  inputMode="decimal"
                  value={smoothingDraft}
                  onChange={(e) => setSmoothingDraft(e.target.value)}
                />
              </label>
              <div className="flex items-end">
                <button
                  className={cn(
                    'h-10 w-full rounded-lg border border-zinc-900 bg-zinc-900 px-3 text-sm font-medium text-white shadow-sm',
                    'active:translate-y-px',
                  )}
                  onClick={() => void applyControl()}
                >
                  Apply
                </button>
              </div>
            </div>

            {err ? (
              <div className="mt-4 rounded-lg border border-amber-200 bg-amber-50 p-3 text-sm text-amber-900">
                <div className="font-medium">Daemon not reachable</div>
                <div className="mt-1 text-pretty text-amber-800">
                  {err}. Start it with <span className="font-mono">./scripts/linux-run.sh</span>.
                </div>
              </div>
            ) : null}
          </section>
        </div>

        <footer className="mt-8 text-xs text-zinc-500">
          v1: sample metrics via eBPF, config via /api/config, presets via /api/preset/select.
        </footer>
      </div>
    </div>
  )
}

export default App
