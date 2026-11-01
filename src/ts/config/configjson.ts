export const prettyJson = (values: unknown): string => {
  const seen = new Set<unknown>();
  const mark = (v: unknown) => {
    if (seen.has(v)) {
      throw new Error('Circular reference');
    }
    seen.add(v);
  };
  const traverse = (value: unknown): unknown => {
    if (Array.isArray(value)) {
      mark(value);
      return value.map((v) => traverse(v));
    }
    if (typeof value === 'object' && value !== null) {
      mark(value);
      return Object.fromEntries(
        Object.keys(value).sort()
          .map((key) => [key, traverse((value as any)[key])])
      );
    }
    return value;
  };

  return JSON.stringify(traverse(values), null, 2) + "\n";
};
