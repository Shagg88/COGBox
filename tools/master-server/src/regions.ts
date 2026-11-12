// US-W and US-E were merged into one US region; older launchers still send the old names.
export function normalizeRegion<T extends string | undefined>(region: T): T | 'us' {
  return region === 'us-w' || region === 'us-e' ? 'us' : region;
}
