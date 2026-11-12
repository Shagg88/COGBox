import type { Embed } from './types.js';

export interface PublishResult { messageId: string; retryAfterS?: number }

// Errors carry the method and status only: the webhook URL holds the token and must never reach a log
export async function publish(f: typeof fetch, hookUrl: string, messageId: string | null, embed: Embed): Promise<PublishResult> {
  const base = hookUrl.replace(/\/+$/, '').split('?')[0];
  const body = JSON.stringify({ embeds: [embed], allowed_mentions: { parse: [] } });
  const send = (method: string, url: string) =>
    f(url, { method, headers: { 'content-type': 'application/json' }, body, signal: AbortSignal.timeout(10000) });

  const backoff = async (res: Response, id: string): Promise<PublishResult> => {
    const j = (await res.json().catch(() => ({}))) as { retry_after?: number };
    const s = Number(j.retry_after ?? res.headers.get('retry-after'));
    return { messageId: id, retryAfterS: Math.max(1, Math.ceil(Number.isFinite(s) ? s : 60)) };
  };

  const create = async (): Promise<PublishResult> => {
    const res = await send('POST', `${base}?wait=true`);
    if (res.status === 429) return backoff(res, '');
    if (!res.ok) throw new Error(`webhook POST failed: ${res.status}`);
    const j = (await res.json()) as { id: string };
    return { messageId: j.id };
  };

  if (!messageId) return create();
  const res = await send('PATCH', `${base}/messages/${encodeURIComponent(messageId)}`);
  if (res.ok) return { messageId };
  if (res.status === 429) return backoff(res, messageId);
  if (res.status === 404) return create(); // someone deleted the message: post a new one
  throw new Error(`webhook PATCH failed: ${res.status}`);
}
