// Copied from tools/master-server/src/log_scrub.ts (the two packages share no code); keep the two in step.
// What the request log may hold (PRIVACY.md): the method and the path, with any sign-in token hidden. The games
// sign in to /ws/signal with ?token=..., which the default log line printed in full along with the player's address.
export function logSafeUrl(url: string): string {
  return url.replace(/([?&]token=)[^&#]*/gi, '$1REDACTED');
}

export function logSafeRequest(req: { method: string; url: string }): { method: string; url: string } {
  return { method: req.method, url: logSafeUrl(req.url) };
}

// A free-text message (an error the server logged) for the admin overview: no uuid, xuid, address or token.
export function logSafeText(text: string): string {
  return logSafeUrl(text)
    .replace(/\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b/gi, '<uuid>')
    .replace(/\b\d{1,3}(?:\.\d{1,3}){3}(?::\d+)?\b/g, '<ip>')
    .replace(/(?<![\w:])(?:[0-9a-f]{1,4}:){3,7}[0-9a-f]{1,4}(?![\w:])|(?<![\w:])(?:[0-9a-f]{1,4}:){0,6}[0-9a-f]{0,4}::(?:[0-9a-f]{1,4}(?::[0-9a-f]{1,4}){0,6})?(?![\w:])/gi, '<ip>')
    .replace(/\b[0-9a-f]{16}\b/gi, '<xuid>');
}
