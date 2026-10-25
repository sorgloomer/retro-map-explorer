// Typed access to the elements of index.html.

export function $<T extends HTMLElement = HTMLElement>(id: string): T {
  const el = document.getElementById(id);
  if (!el) throw new Error('missing element #' + id);
  return el as T;
}

export const $input = (id: string) => $<HTMLInputElement>(id);
