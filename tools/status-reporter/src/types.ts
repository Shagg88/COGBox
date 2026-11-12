// up: fine. warn: reachable but something needs a look (failed pipeline). down: a service is not answering.
// unknown: we could not find out (API error, rate limit). unavailable: an optional source that is off or unreachable.
export type State = 'up' | 'warn' | 'down' | 'unknown' | 'unavailable';

export interface Line {
  name: string;
  state: State;
  text: string;
  core?: boolean;        // a core service going down turns the whole embed red
  optional?: boolean;    // never changes the embed colour
  retryAfterS?: number;  // the source asked us to slow down
  ms?: number;           // response time
}

export interface Embed {
  title: string;
  description: string;
  color: number;
  thumbnail?: { url: string };
  fields: { name: string; value: string; inline?: boolean }[];
  footer: { text: string };
  timestamp: string;
}
