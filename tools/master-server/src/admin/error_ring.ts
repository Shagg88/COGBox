// The last errors the server logged, for the admin panel's overview (messages only, already scrubbed).
export class ErrorRing {
  private items: { time: number; msg: string }[] = [];
  constructor(private readonly capacity = 200) {}

  push(msg: string): void {
    this.items.push({ time: Date.now(), msg });
    if (this.items.length > this.capacity) this.items.shift();
  }

  last(n: number): { time: number; msg: string }[] {
    return n > 0 ? this.items.slice(-n) : [];
  }
}
