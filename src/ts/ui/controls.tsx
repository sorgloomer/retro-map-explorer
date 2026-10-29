// Small form controls bound to the Display options in the store.
import type { ComponentChildren } from 'preact';
import { changeOpt } from './controller.ts';
import { useUi } from './store.ts';

interface Common {
  optKey: string;
  disabled?: boolean;
  title?: string;
}

/** A labelled range slider for the option `optKey`. */
export function Slider({ optKey, label, min, max, step, disabled, title, showValue }: Common & { label: string; min: number; max: number; step: number; showValue?: boolean }) {
  const value = useUi((s) => s.opts[optKey]);
  return (
    <label class="slider" title={title} style={disabled ? { opacity: '.5' } : undefined}>
      {showValue ? `${label}: ${value}` : label}
      <input type="range" min={min} max={max} step={step} value={value} disabled={disabled}
        onInput={(e) => changeOpt(optKey, parseFloat(e.currentTarget.value))} />
    </label>
  );
}

/** A labelled checkbox for the option `optKey` (stored as 0 / 1). */
export function Check({ optKey, children, disabled, title }: Common & { children: ComponentChildren }) {
  const value = useUi((s) => s.opts[optKey]);
  return (
    <label class="check" title={title} style={disabled ? { opacity: '.5' } : undefined}>
      <input type="checkbox" checked={!!value} disabled={disabled}
        onChange={(e) => { changeOpt(optKey, e.currentTarget.checked ? 1 : 0); e.currentTarget.blur(); }} />
      {children}
    </label>
  );
}
