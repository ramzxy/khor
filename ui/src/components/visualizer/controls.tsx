// Minimal overlay controls for the visualizer — semi-transparent, bottom-right corner.

import type { EngineOptions } from './engine'

interface ControlsProps {
  options: EngineOptions
  onChange: (opts: Partial<EngineOptions>) => void
  onClose: () => void
  onFullscreen: () => void
  connected: boolean
}

const densityLabels = ['Low', 'Med', 'High']
const trailLabels = ['Short', 'Med', 'Long']

export function VisualizerControls({ options, onChange, onClose, onFullscreen, connected }: ControlsProps) {
  return (
    <div className="pointer-events-auto absolute right-4 bottom-4 z-50 flex flex-col gap-2 rounded-2xl border border-white/10 bg-slate-950/80 p-4 backdrop-blur-xl"
      style={{ minWidth: 200 }}
    >
      {/* Header */}
      <div className="flex items-center justify-between">
        <span className="flex items-center gap-2 text-[11px] font-medium uppercase tracking-[0.2em] text-slate-400">
          <span className={`inline-block h-1.5 w-1.5 rounded-full ${connected ? 'bg-emerald-500' : 'bg-amber-500'}`} />
          Visualizer
        </span>
        <button
          onClick={onClose}
          className="rounded-lg p-1 text-slate-400 transition-colors hover:bg-white/5 hover:text-white"
          aria-label="Close visualizer"
        >
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none"><path d="M4 4l8 8M12 4l-8 8" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"/></svg>
        </button>
      </div>

      {/* Particle density */}
      <label className="flex items-center justify-between gap-3">
        <span className="text-xs text-slate-400">Density</span>
        <div className="flex gap-1">
          {densityLabels.map((label, i) => (
            <button
              key={label}
              onClick={() => onChange({ particleDensity: i })}
              className={`rounded-md px-2 py-0.5 text-[10px] font-semibold transition-colors ${
                options.particleDensity === i
                  ? 'bg-white/15 text-white'
                  : 'text-slate-500 hover:text-slate-300'
              }`}
            >
              {label}
            </button>
          ))}
        </div>
      </label>

      {/* Trail length */}
      <label className="flex items-center justify-between gap-3">
        <span className="text-xs text-slate-400">Trails</span>
        <div className="flex gap-1">
          {trailLabels.map((label, i) => (
            <button
              key={label}
              onClick={() => onChange({ trailLength: i })}
              className={`rounded-md px-2 py-0.5 text-[10px] font-semibold transition-colors ${
                options.trailLength === i
                  ? 'bg-white/15 text-white'
                  : 'text-slate-500 hover:text-slate-300'
              }`}
            >
              {label}
            </button>
          ))}
        </div>
      </label>

      {/* Labels toggle */}
      <label className="flex cursor-pointer items-center justify-between gap-3">
        <span className="text-xs text-slate-400">Labels</span>
        <button
          onClick={() => onChange({ showLabels: !options.showLabels })}
          className={`rounded-md px-2 py-0.5 text-[10px] font-semibold transition-colors ${
            options.showLabels
              ? 'bg-white/15 text-white'
              : 'text-slate-500 hover:text-slate-300'
          }`}
        >
          {options.showLabels ? 'On' : 'Off'}
        </button>
      </label>

      {/* Action buttons */}
      <div className="mt-1 flex gap-2 border-t border-white/5 pt-2">
        <button
          onClick={() => onChange({ paused: !options.paused })}
          className="flex-1 rounded-lg border border-white/10 bg-white/5 py-1.5 text-[10px] font-semibold text-slate-300 transition-colors hover:bg-white/10"
        >
          {options.paused ? 'Resume' : 'Pause'}
        </button>
        <button
          onClick={onFullscreen}
          className="flex-1 rounded-lg border border-white/10 bg-white/5 py-1.5 text-[10px] font-semibold text-slate-300 transition-colors hover:bg-white/10"
        >
          Fullscreen
        </button>
      </div>
    </div>
  )
}
