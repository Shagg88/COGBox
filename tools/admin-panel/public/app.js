import { auditHtml, backupsHtml, lobbiesHtml, logsHtml, overviewHtml } from './render.js';

const $ = (s) => document.querySelector(s);
const API = '/admin/api';
let csrf = '';
let timer = 0;
let logLines = [];

class HttpError extends Error { constructor(status, msg) { super(msg); this.status = status; } }

async function api(path, body) {
  const opts = body === undefined ? {} : {
    method: 'POST',
    headers: { 'content-type': 'application/json', 'x-csrf-token': csrf },
    body: JSON.stringify(body),
  };
  const res = await fetch(API + path, { credentials: 'same-origin', ...opts });
  const data = await res.json().catch(() => ({}));
  if (res.status === 401 && path !== '/login') { show('login'); throw new HttpError(401, 'Signed out.'); }
  if (!res.ok) throw new HttpError(res.status, data.error || `Request failed (${res.status}).`);
  return data;
}

function show(which) {
  for (const id of ['login', 'pwchange', 'panel']) $('#' + id).hidden = id !== which;
  $('#who').hidden = which === 'login';
  clearInterval(timer);
  if (which === 'panel') { tab('overview'); timer = setInterval(() => { if (current === 'overview') loadOverview(); }, 10_000); }
  if (which === 'login') { csrf = ''; $('#login').reset(); $('#login [name=name]').focus(); }
}

function note(text, bad = false) {
  const m = $('#msg');
  m.textContent = text;   // textContent: server error texts are never parsed as HTML
  m.className = bad ? 'bad' : '';
}
const fail = (e) => { if (e.status !== 401) note(e.message, true); };

let current = 'overview';
const loaders = { overview: loadOverview, lobbies: loadLobbies, logs: loadLogs, actions: loadBackups, audit: loadAudit };
function tab(name) {
  current = name;
  for (const b of document.querySelectorAll('#tabs button')) b.classList.toggle('on', b.dataset.tab === name);
  for (const s of document.querySelectorAll('[data-page]')) s.hidden = s.dataset.page !== name;
  note('');
  loaders[name]();
}

async function loadOverview() { try { $('#overview').innerHTML = overviewHtml(await api('/overview')); } catch (e) { fail(e); } }
async function loadLobbies() { try { $('#lobbies').innerHTML = lobbiesHtml(await api('/lobbies'), Date.now()); } catch (e) { fail(e); } }
async function loadBackups() { try { $('#backups').innerHTML = backupsHtml(await api('/backups')); } catch (e) { fail(e); } }
async function loadAudit() { try { $('#audit').innerHTML = auditHtml(await api('/audit')); } catch (e) { fail(e); } }
async function loadLogs() {
  try {
    logLines = (await api('/logs?service=' + encodeURIComponent($('#log-service').value))).lines;
    drawLogs();
  } catch (e) { logLines = []; drawLogs(); fail(e); }
}
const drawLogs = () => { $('#logs').innerHTML = logsHtml(logLines, $('#log-filter').value); };

async function start() {
  try {
    const me = await api('/me');
    csrf = me.csrf;
    $('#whoname').textContent = me.name;
    show(me.must_change ? 'pwchange' : 'panel');
  } catch (e) {
    if (e.status !== 401) { show('login'); $('#login .err').textContent = e.message; }
  }
}

$('#login').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target);
  const err = $('#login .err');
  err.textContent = '';
  try {
    await api('/login', { name: f.get('name'), password: f.get('password'), code: f.get('code') });
    await start();
  } catch (e) { err.textContent = e.message; }
});

$('#pwchange').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target);
  const err = $('#pwchange .err');
  if (f.get('password') !== f.get('again')) { err.textContent = 'The two passwords differ.'; return; }
  try {
    await api('/password', { password: f.get('password') });
    ev.target.reset();
    err.textContent = '';
    await start();
  } catch (e) { err.textContent = e.message; }
});

$('#logout').addEventListener('click', async () => {
  try { await api('/logout', {}); } catch { /* signed out either way */ }
  show('login');
});

$('#tabs').addEventListener('click', (ev) => { const t = ev.target.closest('button[data-tab]'); if (t) tab(t.dataset.tab); });
$('#lobbies-refresh').addEventListener('click', loadLobbies);
$('#logs-refresh').addEventListener('click', loadLogs);
$('#log-service').addEventListener('change', loadLogs);
$('#log-filter').addEventListener('input', drawLogs);
$('#logs-copy').addEventListener('click', async () => {
  const text = $('#logs pre').textContent;
  try { await navigator.clipboard.writeText(text); note('Copied.'); } catch { note('Copy failed; select the text instead.', true); }
});

$('#backup').addEventListener('click', async (ev) => {
  ev.target.disabled = true;
  note('Backing up...');
  try { const r = await api('/backup', {}); note(`Backup made: ${r.name}`); loadBackups(); } catch (e) { fail(e); }
  ev.target.disabled = false;
});

// Lobby close and restart buttons are rendered into several places, so one listener handles them all.
document.addEventListener('click', async (ev) => {
  const close = ev.target.closest('button[data-close]');
  if (close) {
    if (!confirm(`Close lobby ${close.dataset.room}? Its players are sent back as if the host left.`)) return;
    try { await api(`/lobbies/${encodeURIComponent(close.dataset.close)}/close`, { confirm: true }); note(`Closed lobby ${close.dataset.room}.`); } catch (e) { fail(e); }
    loadLobbies();
    return;
  }
  const r = ev.target.closest('button[data-restart]');
  if (r) openRestart(r.dataset.restart);
});

const dlg = $('#restart-dlg');
let restartSvc = '';
function openRestart(svc) {
  restartSvc = svc;
  dlg.returnValue = '';   // otherwise Esc after an earlier confirm would still read 'ok'
  $('#restart-svc').textContent = svc;
  $('#restart-word').value = '';
  $('#restart-ok').disabled = true;
  dlg.showModal();
  $('#restart-word').focus();
}
$('#restart-word').addEventListener('input', (ev) => { $('#restart-ok').disabled = ev.target.value !== 'RESTART'; });
dlg.addEventListener('close', async () => {
  if (dlg.returnValue !== 'ok') return;
  dlg.returnValue = '';
  const confirmWord = $('#restart-word').value;
  note(`Restarting ${restartSvc}...`);
  try { await api('/restart', { service: restartSvc, confirm: confirmWord }); note(`${restartSvc} restarted.`); } catch (e) { fail(e); }
  if (current === 'overview') loadOverview();
});

start();
