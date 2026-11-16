// Pure renderers: data in, HTML string out. Every server string goes through esc().

export function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

const TITLES = { '4D5307D5': 'Gears of War', '4D53082D': 'Gears of War 2', '4D5308AB': 'Gears of War 3', '4D530A26': 'Gears of War: Judgment' };
const title = (id) => TITLES[String(id).toUpperCase()] ?? id;
const OLD_MS = 2 * 3600_000;

export function duration(ms) {
  const m = Math.max(0, Math.floor(ms / 60_000));
  if (m < 60) return `${m} min`;
  const h = Math.floor(m / 60);
  return h < 48 ? `${h} h ${m % 60} min` : `${Math.floor(h / 24)} d ${h % 24} h`;
}

export function when(ms) {
  // a bad or missing time must not take the whole table down
  if (typeof ms !== 'number' || !Number.isFinite(ms) || Math.abs(ms) > 8.64e15) return '—';
  return new Date(ms).toISOString().replace('T', ' ').slice(0, 19) + ' UTC';
}

function card(name, state, cls, lines, restart) {
  return `<div class="card ${cls}"><h3>${esc(name)}</h3><p class="state">${esc(state)}</p>` +
    lines.map((l) => `<p class="dim">${esc(l)}</p>`).join('') +
    (restart ? `<button type="button" data-restart="${esc(restart)}">Restart</button>` : '') + '</div>';
}

function counts(obj) {
  const e = Object.entries(obj ?? {});
  if (!e.length) return '<p class="dim">None</p>';
  return '<ul class="counts">' + e.map(([k, v]) => `<li><span>${esc(title(k))}</span><b>${esc(v)}</b></li>`).join('') + '</ul>';
}

export function overviewHtml({ health, status, last_action }, now = Date.now()) {
  // health null = Docker unreachable: we don't know, so not Down. status null = the game server did not answer.
  const svc = (s, restart) => {
    const h = health?.[s];
    if (!h) return card(s, 'Unknown', 'unknown', ['Docker did not answer'], restart);
    const started = Date.parse(h.started_at);
    const up = h.up && Number.isFinite(started) ? [`Up for ${duration(now - started)}`] : [];
    return card(s, h.up ? 'Up' : 'Down', h.up ? 'up' : 'down', [...up, `Restarts: ${h.restarts}`], restart);
  };
  const game = status
    ? card('Game server', 'Up', 'up', [`Version ${status.version}`, `Up for ${duration(status.uptime_s * 1000)}`], null)
    : card('Game server', 'Down', 'down', ['No answer from master-server'], 'master-server');
  let out = '<section class="cards">' + svc('master-server', 'master-server') + svc('coturn', 'coturn') + svc('caddy', null) + game + '</section>';
  if (status) {
    out += '<section class="cards">' +
      `<div class="card"><h3>Players online</h3><p class="big">${esc(status.players.total)}</p></div>` +
      `<div class="card"><h3>Open lobbies</h3><p class="big">${esc(status.lobbies)}</p></div>` +
      `<div class="card"><h3>Pending invites</h3><p class="big">${esc(status.pending_invites)}</p></div></section>` +
      '<section class="cards">' +
      `<div class="card"><h3>By game</h3>${counts(status.players.by_title)}</div>` +
      `<div class="card"><h3>By region</h3>${counts(status.players.by_region)}</div></section>`;
    const errs = (status.errors ?? []).slice(-20);
    out += '<h2>Last errors</h2>' + (errs.length
      ? '<pre class="log">' + errs.map((e) => `${esc(when(e.time))}  ${esc(e.msg)}`).join('\n') + '</pre>'
      : '<p class="dim">None</p>');
  }
  out += '<h2>Last admin action</h2>' + (last_action
    ? `<p>${esc(last_action.action)} ${esc(last_action.detail)} <span class="dim">by ${esc(last_action.name)}, ${esc(when(last_action.time))}</span></p>`
    : '<p class="dim">None yet</p>');
  return out;
}

export function lobbiesHtml(list, now = Date.now()) {
  if (!list.length) return '<p class="dim">No open lobbies.</p>';
  return '<table><thead><tr><th>Room</th><th>Game</th><th>Host</th><th>Type</th><th>Age</th><th></th></tr></thead><tbody>' +
    list.map((l) => {
      const age = now - l.created_at;
      return `<tr${age > OLD_MS ? ' class="old"' : ''}><td data-l="Room">${esc(l.room_code)}</td><td data-l="Game">${esc(title(l.title_id))}</td>` +
        `<td data-l="Host">${esc(l.host_tag)}</td><td data-l="Type">${l.is_public ? 'Public' : 'Friends only'}</td>` +
        `<td data-l="Age">${esc(duration(age))}</td>` +
        `<td><button type="button" data-close="${esc(l.lobby_id)}" data-room="${esc(l.room_code)}">Close</button></td></tr>`;
    }).join('') + '</tbody></table>';
}

export function logsHtml(lines, filter) {
  const f = String(filter ?? '').toLowerCase();
  const shown = f ? lines.filter((l) => l.toLowerCase().includes(f)) : lines;
  return `<p class="dim">${shown.length} of ${lines.length} lines</p><pre class="log" tabindex="0">${shown.map(esc).join('\n')}</pre>`;
}

export function backupsHtml(list) {
  if (!list.length) return '<p class="dim">No backups yet.</p>';
  return '<table><thead><tr><th>Name</th><th>Size</th><th>Date</th></tr></thead><tbody>' +
    list.map((b) => `<tr><td data-l="Name">${esc(b.name)}</td><td data-l="Size">${esc((b.size / 1048576).toFixed(2))} MB</td>` +
      `<td data-l="Date">${esc(when(b.mtime))}</td></tr>`).join('') + '</tbody></table>';
}

export function auditHtml(rows) {
  if (!rows.length) return '<p class="dim">Nothing recorded yet.</p>';
  return '<table><thead><tr><th>Time</th><th>User</th><th>Action</th><th>Detail</th></tr></thead><tbody>' +
    rows.map((r) => `<tr><td data-l="Time">${esc(when(r.time))}</td><td data-l="User">${esc(r.name)}</td>` +
      `<td data-l="Action">${esc(r.action)}</td><td data-l="Detail">${esc(r.detail)}</td></tr>`).join('') + '</tbody></table>';
}

// ---- Fabricator ----
// Mod names, report texts and tags are written by players: every one goes through esc().
const GB = 1073741824;
const GAMES = { gears1: 'Gears of War', gears2: 'Gears of War 2', gears3: 'Gears of War 3', judgment: 'Gears of War: Judgment' };
const STATES = { live: 'Live', taken_down: 'Taken down', removed: 'Removed' };
const VERSION_STATES = { live: 'Live', files_deleted: 'Files deleted' };
const MB = 1048576;

// The creator's publishing switch ('unknown' = the account is gone: nothing to switch).
function publishingButton(author, publishing) {
  if (author === 'unknown') return '';
  return publishing === false
    ? `<button type="button" data-fab-allow="${esc(author)}">Allow publishing</button>`
    : `<button type="button" class="danger" data-fab-stop="${esc(author)}">Stop publishing</button>`;
}

// Take down / restore and delete files for a mod that is not removed.
function modButtons(mod) {
  if (mod.state === 'removed') return '';
  const id = esc(mod.id);
  const btn = (action, label, cls = '') =>
    `<button type="button"${cls ? ` class="${cls}"` : ''} data-fab-action="${action}" data-mod="${id}" data-name="${esc(mod.name)}">${label}</button>`;
  return (mod.state === 'taken_down' ? btn('restore', 'Restore') : btn('takedown', 'Take down', 'danger')) + btn('delete-files', 'Delete files', 'danger');
}

export function fabStatsHtml(s) {
  const used = (s.disk.total - s.disk.free) / GB;
  const pct = s.disk.total > 0 ? Math.round(((s.disk.total - s.disk.free) / s.disk.total) * 100) : 0;
  return '<section class="cards">' +
    `<div class="card"><h3>Disk</h3><p class="big">${esc(pct)}%</p><p class="dim">${esc(used.toFixed(1))} of ${esc((s.disk.total / GB).toFixed(1))} GB used</p></div>` +
    `<div class="card"><h3>Uploads (30 days)</h3><p class="big">${esc((s.month_bytes / GB).toFixed(2))} GB</p></div>` +
    `<div class="card"><h3>Downloads (30 days)</h3><p class="big">${esc(((s.month_bytes_out ?? 0) / GB).toFixed(2))} GB</p></div>` +
    `<div class="card"><h3>Live mods</h3><p class="big">${esc(s.mods)}</p></div>` +
    `<div class="card"><h3>Creators</h3><p class="big">${esc(s.creators)}</p></div>` +
    `<div class="card"><h3>Open reports</h3><p class="big">${esc(s.open_reports)}</p></div></section>`;
}

export function fabReportsHtml(groups, now = Date.now()) {
  if (!groups.length) return '<p class="dim">No open reports.</p>';
  return groups.map(({ mod, reports }) => {
    const buttons = modButtons(mod) + publishingButton(mod.author, mod.publishing);
    return `<div class="card report"><h3>${esc(mod.name)} <span class="dim">${esc(GAMES[mod.game] ?? mod.game)}, by ${esc(mod.author)}, ${esc(STATES[mod.state] ?? mod.state)}</span></h3>` +
      '<table><thead><tr><th>Reason</th><th>Text</th><th>Reporter</th><th>Age</th></tr></thead><tbody>' +
      reports.map((r) => `<tr><td data-l="Reason">${esc(r.reason)}</td><td data-l="Text">${esc(r.text)}</td>` +
        `<td data-l="Reporter">${esc(r.reporter)}</td><td data-l="Age">${esc(duration(now - r.created_at))}</td></tr>`).join('') +
      `</tbody></table><div class="row">${buttons}</div></div>`;
  }).join('');
}

// The mods view: every state, newest first.
export function fabModsHtml(list) {
  if (!list.length) return '<p class="dim">No mods match.</p>';
  return '<table><thead><tr><th>Name</th><th>Game</th><th>Creator</th><th>State</th><th>Versions</th><th>Open reports</th><th>Added</th><th></th></tr></thead><tbody>' +
    list.map((m) => `<tr><td data-l="Name">${esc(m.name)} <span class="dim">#${esc(m.id)}</span></td>` +
      `<td data-l="Game">${esc(GAMES[m.game] ?? m.game)}</td><td data-l="Creator">${esc(m.author)}</td>` +
      `<td data-l="State">${esc(STATES[m.state] ?? m.state)}</td><td data-l="Versions">${esc(m.versions)}</td>` +
      `<td data-l="Open reports">${esc(m.open_reports)}</td><td data-l="Added">${esc(when(m.created_at))}</td>` +
      `<td><div class="row"><button type="button" data-fab-detail="${esc(m.id)}">Details</button>${modButtons(m)}</div></td></tr>`).join('') +
    '</tbody></table>';
}

// One mod: who made it (and whether they may publish), its versions and files, screenshots, every report.
export function fabModHtml(d) {
  const versions = d.versions.length
    ? '<table><thead><tr><th>Version</th><th>Size</th><th>SHA-256</th><th>File</th><th>Published</th></tr></thead><tbody>' +
      d.versions.map((v) => `<tr><td data-l="Version">${esc(v.version)}</td><td data-l="Size">${esc((v.size / MB).toFixed(2))} MB</td>` +
        `<td data-l="SHA-256"><code>${esc(v.sha256)}</code></td><td data-l="File">${esc(VERSION_STATES[v.state] ?? v.state)}</td>` +
        `<td data-l="Published">${esc(when(v.created_at))}</td></tr>`).join('') + '</tbody></table>'
    : '<p class="dim">No versions.</p>';
  const reports = d.reports.length
    ? '<table><thead><tr><th>Reason</th><th>Text</th><th>Reporter</th><th>Date</th><th>Handled</th></tr></thead><tbody>' +
      d.reports.map((r) => `<tr><td data-l="Reason">${esc(r.reason)}</td><td data-l="Text">${esc(r.text)}</td>` +
        `<td data-l="Reporter">${esc(r.reporter)}</td><td data-l="Date">${esc(when(r.created_at))}</td>` +
        `<td data-l="Handled">${r.handled_by ? `${esc(r.outcome)} <span class="dim">by ${esc(r.handled_by)}</span>` : 'Open'}</td></tr>`).join('') + '</tbody></table>'
    : '<p class="dim">No reports.</p>';
  return `<div class="card report"><h3>${esc(d.name)} <span class="dim">#${esc(d.id)}, ${esc(GAMES[d.game] ?? d.game)}, ${esc(d.type)}, ` +
    `${esc(STATES[d.state] ?? d.state)}</span></h3>` +
    `<p>By ${esc(d.author)}${d.author === 'unknown' ? '' : d.publishing === false ? ' <span class="dim">(publishing stopped)</span>' : ''}` +
    ` <span class="dim">added ${esc(when(d.created_at))}, updated ${esc(when(d.updated_at))}, ${esc(d.screens)} screenshots</span></p>` +
    `<div class="row">${modButtons(d)}${publishingButton(d.author, d.publishing)}</div>` +
    `<h4>Versions</h4>${versions}<h4>Reports</h4>${reports}</div>`;
}
