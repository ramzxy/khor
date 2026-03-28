// SSE consumer with client-side exponential smoothing and note-event simulation.

export interface KhorSignals {
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

export interface NoteEvent {
  midi: number
  velocity: number
  duration: number
  time: number // performance.now() when received
}

export interface SmoothedSignals extends KhorSignals {
  preset: string
}

const ZERO_SIGNALS: KhorSignals = {
  exec_s: 0, rx_kbs: 0, tx_kbs: 0, csw_s: 0,
  blk_r_kbs: 0, blk_w_kbs: 0, retx_s: 0, irq_s: 0, mem_pct: 0,
}

export class SignalStore {
  raw: KhorSignals = { ...ZERO_SIGNALS }
  smoothed: KhorSignals = { ...ZERO_SIGNALS }
  preset = 'ambient'
  notes: NoteEvent[] = []
  connected = false

  private es: EventSource | null = null
  private smoothingFactor = 0.15 // lower = smoother
  private prevExec = 0
  private lastNoteTime = 0

  start(apiBase: string) {
    this.stop()
    try {
      const es = new EventSource(`${apiBase}/api/stream`)
      this.es = es
      es.onmessage = (evt) => {
        try {
          const d = JSON.parse(evt.data)
          if (d.rates) {
            this.raw = {
              exec_s: d.rates.exec_s ?? 0,
              rx_kbs: d.rates.rx_kbs ?? 0,
              tx_kbs: d.rates.tx_kbs ?? 0,
              csw_s: d.rates.csw_s ?? 0,
              blk_r_kbs: d.rates.blk_r_kbs ?? 0,
              blk_w_kbs: d.rates.blk_w_kbs ?? 0,
              retx_s: d.rates.retx_s ?? 0,
              irq_s: d.rates.irq_s ?? 0,
              mem_pct: d.rates.mem_pct ?? 0,
            }
            // Simulate note events from sharp exec spikes — rate-limited to max 1/sec
            const now = performance.now()
            const execDelta = this.raw.exec_s - this.prevExec
            if (execDelta > 0.3 && now - this.lastNoteTime > 1000) {
              this.notes.push({
                midi: 48 + Math.floor(this.raw.exec_s * 36),
                velocity: Math.min(0.6, execDelta),
                duration: 0.5,
                time: now,
              })
              this.lastNoteTime = now
            }
            this.prevExec = this.raw.exec_s
          }
          if (d.controls?.preset) {
            this.preset = d.controls.preset
          }
          this.connected = true
        } catch { /* ignore parse errors */ }
      }
      es.onerror = () => {
        this.connected = false
        es.close()
        this.es = null
        // Reconnect after 2s
        setTimeout(() => this.start(apiBase), 2000)
      }
    } catch {
      this.connected = false
    }
  }

  stop() {
    this.es?.close()
    this.es = null
    this.connected = false
  }

  /** Call each frame to update smoothed values. */
  tick() {
    const a = this.smoothingFactor
    const keys = Object.keys(ZERO_SIGNALS) as (keyof KhorSignals)[]
    for (const k of keys) {
      this.smoothed[k] += (this.raw[k] - this.smoothed[k]) * a
    }
    // Expire old notes (keep last 2 seconds)
    const now = performance.now()
    this.notes = this.notes.filter(n => now - n.time < 2000)
  }
}
