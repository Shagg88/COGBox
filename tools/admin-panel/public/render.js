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
