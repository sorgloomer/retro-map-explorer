export function mapSum<T>(items: ReadonlyArray<T>, fn: (item: T) => number): number {
  let result = 0;
  for (const item of items) {
    result += fn(item);
  }
  return result;
}

export function sum(items: ReadonlyArray<number>): number {
  let result = 0;
  for (const item of items) {
    result += item;
  }
  return result;
}
