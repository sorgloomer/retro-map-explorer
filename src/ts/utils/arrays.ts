// Small array helpers.

/** Appends all items to `target`, one by one (unlike `target.push(...items)` this has no limit on the number of items). */
export function pushAll<T>(target: T[], items: Iterable<T>): void {
  for (const item of items) target.push(item);
}
