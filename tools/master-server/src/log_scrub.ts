// What the request log may hold (PRIVACY.md): the method and the path, with any sign-in token hidden. The games
// sign in to /ws/signal with ?token=..., which the default log line printed in full along with the player's address.
export function logSafeUrl(url: string): string {
  return url.replace(/([?&]token=)[^&#]*/gi, '$1REDACTED');
}

export function logSafeRequest(req: { method: string; url: string }): { method: string; url: string } {
  return { method: req.method, url: logSafeUrl(req.url) };
}
