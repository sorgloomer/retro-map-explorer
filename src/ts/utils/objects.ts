export function isPlainObject(x: unknown): x is object {
  const myProto = Object.getPrototypeOf(x);
  return typeof x === 'object' && x !== null && (myProto === Object.prototype || myProto === null);
}

export function hasOwn<T extends object>(o: T, k: PropertyKey): k is keyof T {
  return Object.hasOwn(o, k);
}

export function getOwn<T extends {}, K extends keyof T>(o: Partial<T>, k: K): T[K] | undefined;
export function getOwn<T extends {}, K extends keyof T>(o: T, k: K): T[K];
export function getOwn<T extends {}, K extends keyof T>(o: T, k: K): T[K] {
  return hasOwn(o, k) ? o[k] : undefined!;
}

export function isSubscriptable<T>(x: T | null | undefined): x is T {
  return x !== null && x !== undefined;
}
