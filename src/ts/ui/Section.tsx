// A collapsible section of the menu. Which sections are folded is remembered (localStorage).
import type { ComponentChildren } from 'preact';
import { toggleFold } from './controller.ts';
import { useUi } from './store.ts';

export function Section({ id, title, hidden, children }: { id: string; title: string; hidden?: boolean; children: ComponentChildren }) {
  const collapsed = useUi((s) => s.folds[id] === 1);
  return (
    <section class={'sec' + (collapsed ? ' collapsed' : '')} id={id} hidden={hidden}>
      <button class="sechead" type="button" aria-expanded={!collapsed} onClick={(e) => { toggleFold(id); e.currentTarget.blur(); }}>
        <span>{title}</span><i class="chev" />
      </button>
      <div class="secbody">{children}</div>
    </section>
  );
}

/** A titled group of controls inside a section; the title folds it, and the state is remembered like the sections'. */
export function Group({ id, title, children }: { id: string; title: string; children: ComponentChildren }) {
  const collapsed = useUi((s) => s.folds[id] === 1);
  return (
    <div class={'group' + (collapsed ? ' collapsed' : '')} id={id}>
      <button class="grouphead" type="button" aria-expanded={!collapsed} onClick={(e) => { toggleFold(id); e.currentTarget.blur(); }}>
        <span>{title}</span><i class="chev" />
      </button>
      <div class="groupbody">{children}</div>
    </div>
  );
}
