export function extractZeroSource(text: string): string {
  const fenced = text.match(/```(?:zero|0)?\s*([\s\S]*?)```/i);
  const source = fenced ? fenced[1] : text;
  return `${source.trim()}\n`;
}

export function sourcePatternFailures(
  source: string,
  patterns: RegExp[],
): string[] {
  return patterns
    .filter((pattern) => !pattern.test(source))
    .map((pattern) => pattern.toString());
}

export function finalSourceResponseFailures(
  responseText: string,
  source: string,
): string[] {
  const trimmed = responseText.trim();
  if (trimmed !== source.trim()) {
    return ["final response included prose or Markdown around the source"];
  }
  if (trimmed.startsWith("```") || /(^|\n)```/.test(trimmed)) {
    return ["final response included prose or Markdown around the source"];
  }
  if (!/^\s*((pub\s+)?fn\s+|use\s+|(pub\s+)?(const|type|enum|choice)\s+|test\s+)/.test(trimmed)) {
    return ["final response did not start with Zero source"];
  }
  return [];
}
