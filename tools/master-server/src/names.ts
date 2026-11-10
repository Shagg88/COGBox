// Player names: the 360's gamertag rule, the same one the runtime applies to its player 1 name (Player1Gamertag):
// letters A-Z a-z, digits, single spaces, trimmed, 3-15 characters. Anything else is refused rather than stored
// mangled, so a player sees the name they typed or an error.
export function normalizeName(raw: unknown): string | null {
  if (typeof raw !== 'string') return null;
  if (!/^[A-Za-z0-9 ]*$/.test(raw)) return null;
  const name = raw.trim().replace(/ {2,}/g, ' ');
  if (name.length < 3 || name.length > 15) return null;
  return name;
}

export const NAME_RULE = 'display_name: 3-15 letters, digits and single spaces';
