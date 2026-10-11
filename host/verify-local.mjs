/* verify-local.mjs — prove the whole pipeline without Atlas credentials.
 *
 * Starts a real single-node mongod as a replica set (so change streams work),
 * then runs the actual project scripts against it in order:
 *
 *     init-db.mjs  ->  seed.mjs --once  ->  bridge.mjs  ->  GET /cadence, GET /
 *
 * Why this exists: on venue Wi-Fi an Atlas failure has three
 * indistinguishable causes (SRV lookup blocked, wrong password, IP not on the
 * allowlist) and none of them are code. Running the same scripts against a
 * local server first means that when the Atlas URI goes in, anything that
 * breaks is credentials or network -- never the pipeline.
 *
 * It also checks the one thing that cannot be undone: that bump_events is
 * created as a TIME SERIES collection. Any write to a non-existent collection
 * creates it as a regular one, permanently.
 *
 *     npm run verify
 *
 * No real badge is involved: BADGE_PORT is pointed at a port that does not
 * exist, so the bridge logs its serial retry and carries on. That is the
 * intended behaviour, not a failure.
 */
import { spawn } from 'node:child_process';
import { MongoMemoryReplSet } from 'mongodb-memory-server';
import { MongoClient } from 'mongodb';

const DB = 'badge';
const PORT = 8099;              // not 8080, so a real bridge can keep running
const FAKE_SERIAL = 'COM99';    // deliberately absent

function run(script, uri, extra = {}) {
  return new Promise((resolve) => {
    const p = spawn(process.execPath, [script, ...(extra.args ?? [])], {
      env: { ...process.env, MONGODB_URI: uri, MONGODB_DB: DB,
             BADGE_PORT: FAKE_SERIAL, HTTP_PORT: String(PORT) },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let out = '';
    p.stdout.on('data', d => { out += d; });
    p.stderr.on('data', d => { out += d; });
    p.on('close', code => resolve({ code, out }));
  });
}

function startBridge(uri) {
  const p = spawn(process.execPath, ['bridge.mjs'], {
    env: { ...process.env, MONGODB_URI: uri, MONGODB_DB: DB,
           BADGE_PORT: FAKE_SERIAL, HTTP_PORT: String(PORT) },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let out = '';
  p.stdout.on('data', d => { out += d; });
  p.stderr.on('data', d => { out += d; });
  return { proc: p, log: () => out };
}

const waitFor = (test, ms = 20000, step = 250) => new Promise((res, rej) => {
  const t0 = Date.now();
  const tick = async () => {
    try { if (await test()) return res(true); } catch { /* keep waiting */ }
    if (Date.now() - t0 > ms) return rej(new Error('timed out'));
    setTimeout(tick, step);
  };
  tick();
});

let repl, bridge;
let failures = 0;
const ok = (label, good, detail = '') => {
  if (!good) failures++;
  console.log(`${good ? '  PASS' : '  FAIL'}  ${label}${detail ? '  — ' + detail : ''}`);
};

try {
  console.log('starting local mongod (replica set, for change streams)...');
  repl = await MongoMemoryReplSet.create({ replSet: { count: 1, storageEngine: 'wiredTiger' } });
  const uri = repl.getUri();
  const client = new MongoClient(uri);
  await client.connect();
  const db = client.db(DB);
  const build = await db.admin().serverInfo();
  console.log(`mongod ${build.version} up\n`);

  // ---- 1. the guard fires before init -------------------------------------
  console.log('1. scripts refuse to run before init-db');
  const seedEarly = await run('seed.mjs', uri, { args: ['--once'] });
  ok('seed.mjs exits non-zero before init', seedEarly.code === 3,
     `exit ${seedEarly.code}`);
  const bridgeEarly = await run('bridge.mjs', uri);
  ok('bridge.mjs exits non-zero before init', bridgeEarly.code === 3,
     `exit ${bridgeEarly.code}`);

  // ---- 2. init creates a TIME SERIES collection ---------------------------
  console.log('\n2. init-db creates the collections');
  const init = await run('init-db.mjs', uri);
  ok('init-db.mjs succeeds', init.code === 0, `exit ${init.code}`);
  const [info] = await db.listCollections({ name: 'bump_events' }).toArray();
  ok('bump_events is a TIME SERIES collection', info?.type === 'timeseries',
     `type=${info?.type}`);
  ok('timeField/metaField/granularity are right',
     info?.options?.timeseries?.timeField === 'ts' &&
     info?.options?.timeseries?.metaField === 'meta' &&
     info?.options?.timeseries?.granularity === 'seconds',
     JSON.stringify(info?.options?.timeseries ?? {}));
  const again = await run('init-db.mjs', uri);
  ok('init-db.mjs is idempotent', again.code === 0, `exit ${again.code}`);

  // ---- 3. seed lands data -------------------------------------------------
  console.log('\n3. seed writes measurements');
  const seed = await run('seed.mjs', uri, { args: ['--once'] });
  ok('seed.mjs succeeds', seed.code === 0, `exit ${seed.code}`);
  const nEvents = await db.collection('bump_events').countDocuments();
  const badge = await db.collection('badges').findOne({});
  ok('bump_events has rows', nEvents > 0, `${nEvents} docs`);
  ok('badges rollup exists', !!badge, badge ? `bumpCount=${badge.bumpCount}` : 'missing');

  // ---- 4. the bridge serves the aggregation and the page ------------------
  console.log('\n4. bridge serves /cadence and /');
  bridge = startBridge(uri);
  await waitFor(async () => {
    const r = await fetch(`http://127.0.0.1:${PORT}/cadence`);
    return r.ok;
  });
  const rows = await (await fetch(`http://127.0.0.1:${PORT}/cadence`)).json();
  ok('/cadence returns rows', Array.isArray(rows) && rows.length > 0,
     `${Array.isArray(rows) ? rows.length : 0} buckets`);

  if (Array.isArray(rows) && rows.length > 1) {
    const gaps = new Set();
    for (let i = 1; i < rows.length; i++) {
      gaps.add(new Date(rows[i].ts) - new Date(rows[i - 1].ts));
    }
    ok('buckets are uniformly 5 s apart', gaps.size === 1 && gaps.has(5000),
       `distinct gaps: ${[...gaps].join(',')}`);
    const aligned = rows.every(r => new Date(r.ts).getTime() % 5000 === 0);
    ok('buckets are grid-aligned', aligned);
    ok('rolling rate is populated',
       rows.some(r => (r.bumpsPerMin ?? 0) > 0),
       `max=${Math.max(...rows.map(r => r.bumpsPerMin ?? 0))}`);
  }

  const page = await (await fetch(`http://127.0.0.1:${PORT}/`)).text();
  ok('page serves and wires SSE', page.includes("EventSource('/events')"),
     `${page.length} bytes`);

  // ---- 5. the change stream actually delivers ----------------------------
  console.log('\n5. change stream -> SSE');
  const sse = await fetch(`http://127.0.0.1:${PORT}/events`);
  const reader = sse.body.getReader();
  const first = await Promise.race([
    reader.read().then(({ value }) => new TextDecoder().decode(value)),
    new Promise(r => setTimeout(() => r(null), 6000)),
  ]);
  ok('SSE connects', typeof first === 'string', first ? 'frame received' : 'no frame');

  await db.collection('badges').updateOne(
    { _id: badge?._id ?? 'badge-01' },
    { $inc: { bumpCount: 1 }, $set: { lastMag: 2600, lastBump: new Date() } },
    { upsert: true });
  const pushed = await Promise.race([
    (async () => {
      for (let i = 0; i < 8; i++) {
        const { value } = await reader.read();
        const txt = new TextDecoder().decode(value);
        if (txt.includes('"type":"badge"')) return txt;
      }
      return null;
    })(),
    new Promise(r => setTimeout(() => r(null), 8000)),
  ]);
  ok('a write is pushed to the page live', !!pushed,
     pushed ? 'change stream delivered' : 'nothing arrived in 8s');
  try { await reader.cancel(); } catch { /* already closing */ }

  await client.close();

  console.log('\n' + '='.repeat(60));
  console.log(failures === 0
    ? 'ALL CHECKS PASSED — the pipeline is sound. Swap in the Atlas URI and\n'
      + 'anything that breaks from here is credentials or network, not code.'
    : `${failures} CHECK(S) FAILED — see above.`);
  console.log('='.repeat(60));
  if (failures) console.log('\nbridge log:\n' + bridge.log());
} catch (err) {
  console.error('\nverify failed to run:', err.message);
  if (bridge) console.error('\nbridge log:\n' + bridge.log());
  failures = failures || 1;
} finally {
  if (bridge?.proc) bridge.proc.kill();
  if (repl) await repl.stop();
  process.exit(failures ? 1 : 0);
}
