// The label next to the marker under the mouse (or the tapped one), kept inside the window.
import { useLayoutEffect, useRef } from 'preact/hooks';
import { useUi } from './store.ts';

export function Tooltip() {
  const tip = useUi((s) => s.tip);
  const box = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const el = box.current;
    if (!el || !tip) return;
    el.style.left = Math.min(tip.x + 14, innerWidth - el.offsetWidth - 8) + 'px';
    el.style.top = Math.min(tip.y + 14, innerHeight - el.offsetHeight - 8) + 'px';
  }, [tip]);
  return (
    <div id="tip" ref={box} hidden={!tip}>
      {tip?.text}
      {tip && <small>{tip.cat}</small>}
    </div>
  );
}
