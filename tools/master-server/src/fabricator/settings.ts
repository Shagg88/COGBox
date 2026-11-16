import fs from 'node:fs';
import path from 'node:path';

const GiB = 2 ** 30;
const MiB = 2 ** 20;

export interface FabSettings {
  maxFileBytes: number;            // one uploaded (packed) file
  maxUnpackedBytes: number;        // everything a mod unpacks to
  maxFiles: number;                // files inside one mod
  creatorQuotaBytes: number;       // stored bytes per creator
  creatorUploadsPerDay: number;
  globalUploadsPerDay: number;
  globalUploadBytesPerDay: number;
  minAccountAgeDays: number;       // account age needed to publish
  keepVersions: number;            // versions whose files are kept
  diskFullFraction: number;        // uploads refused above this disk use
  chunkBytes: number;
  sessionTtlMs: number;            // upload session lifetime
  downloadsPerHour: number;        // download addresses per account
  downloadTtlMs: number;           // download address lifetime
  uploadRequestsPerMinute: number; // chunk and screenshot PUTs per account (their own limit, not the shared one)
  imageRequestsPerMinute: number;  // store picture GETs per address
}

export const FAB_DEFAULTS: FabSettings = {
  maxFileBytes: 4 * GiB,
  maxUnpackedBytes: 8 * GiB,
  maxFiles: 50_000,
  creatorQuotaBytes: 20 * GiB,
  creatorUploadsPerDay: 3,
  globalUploadsPerDay: 50,
  globalUploadBytesPerDay: 100 * GiB,
  minAccountAgeDays: 3,
  keepVersions: 3,
  diskFullFraction: 0.85,
  chunkBytes: 16 * MiB,
  sessionTtlMs: 24 * 3600 * 1000,
  downloadsPerHour: 30,
  downloadTtlMs: 15 * 60 * 1000,
  uploadRequestsPerMinute: 1200,
  imageRequestsPerMinute: 1200,
};

const envName = (key: string): string => 'FAB_' + key.replace(/[A-Z]/g, (c) => '_' + c).toUpperCase();

// Defaults, then <dataDir>/fabricator.json, then FAB_<UPPER_SNAKE> environment variables.
// Unknown keys and values that are not finite numbers are ignored.
export function loadFabSettings(dataDir: string, env: NodeJS.ProcessEnv = process.env): FabSettings {
  const out: FabSettings = { ...FAB_DEFAULTS };
  const keys = Object.keys(FAB_DEFAULTS) as (keyof FabSettings)[];

  let file: Record<string, unknown> = {};
  try {
    const parsed = JSON.parse(fs.readFileSync(path.join(dataDir, 'fabricator.json'), 'utf8'));
    if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) file = parsed;
  } catch { /* missing or malformed: defaults */ }

  for (const k of keys) {
    const v = file[k];
    if (typeof v === 'number' && Number.isFinite(v)) out[k] = v;
    const e = env[envName(k)];
    if (e !== undefined && e.trim() !== '') {
      const n = Number(e);
      if (Number.isFinite(n)) out[k] = n;
    }
  }
  return out;
}
