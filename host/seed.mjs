/* seed.mjs — replay synthetic knocks into Atlas. YOUR INSURANCE.
 *
 * This makes the entire database half demoable with NO BADGE ATTACHED. If the
 * firmware slips, if the badge browns out, if the cable dies at 18:50 -- the
 * aggregation, the live page and the 90-second video all still work. Run this
 * before you touch the firmware, so the Mongo half is never on the critical
 * path of the hardware.
 *
 *   node --env-file=.env seed.mjs          # 10 min of history + live trickle
 *   node --env-file=.env seed.mjs --once   # just the history, then exit
 */
import { MongoClient } from 'mongodb';

const uri     = process.env.MONGODB_URI;
const dbName  = process.env.MONGODB_DB ?? 'badge';
const badgeId = process.env.BADGE_ID   ?? 'badge-01';
const FW      = process.env.BADGE_FW   ?? 'bump-1.0';
const once    = process.argv.includes('--once');
if (!uri) { console.error('MONGODB_URI is not set. Copy .env.example to .env.'); process.exit(1); }

const client = new MongoClient(uri);
await client.connect();
const db = client.db(dbName);
const events = db.collection('bump_events');
const badges = db.collection('badges');

/* Running this before init-db.mjs is the one unrecoverable mistake available
 * in this project: insertMany would CREATE bump_events, and an implicitly
 * created collection is a regular one. It cannot be converted afterwards, and
 * nothing errors -- the seed lands, the pipeline runs, the page updates, and
 * the time series collection the submission is about quietly does not exist. */
{
  const [info] = await db.listCollections({ name: 'bump_events' }).toArray();
  if (!info || info.type !== 'timeseries') {
    console.error(info
      ? `bump_events is a '${info.type}', not a time series collection -- drop it`
      : 'bump_events does not exist -- seeding it would create it as a REGULAR collection');
    console.error('Run `npm run init` first.');
    await client.close();
    process.exit(3);
  }
}

/* Shaped like real data, not uniform noise: bursts of knocks separated by
 * silence, because the whole point of the $densify pipeline is that the
 * silence is visible. A uniform random trickle would hide the feature. */
const now = Date.now();
const docs = [];
let seq = 0, peakMax = 0;

for (let burst = 0; burst < 12; burst++) {
  const burstStart = now - (10 * 60 * 1000) + burst * 50_000 + Math.random() * 8000;
  const n = 1 + Math.floor(Math.random() * 4);
  for (let i = 0; i < n; i++) {
    const mg = 1500 + Math.floor(Math.random() * 2200);   // 1.5 - 3.7 g
    const axis = Math.random() < 0.6 ? 2 : Math.floor(Math.random() * 2);
    peakMax = Math.max(peakMax, mg);
    docs.push({
      ts: new Date(burstStart + i * (280 + Math.random() * 400)),
      meta: { badgeId, fw: FW },
      mag: mg, axis, seq: ++seq, flags: 0, clipped: false, weak: mg < 2250
    });
  }
}
docs.sort((a, b) => a.ts - b.ts);

await events.insertMany(docs);
const last = docs[docs.length - 1];
await badges.updateOne(
  { _id: badgeId },
  { $set: { lastBump: last.ts, lastMag: last.mag, lastAxis: last.axis, fw: FW },
    $max: { peakMag: peakMax },
    $inc: { bumpCount: docs.length } },
  { upsert: true }
);
console.log(`seeded ${docs.length} bumps across 12 bursts over the last 10 minutes`);

if (once) { await client.close(); process.exit(0); }

/* Live trickle so the page visibly moves while you film it. */
console.log('trickling a bump every 4-9s. Ctrl+C to stop.');
async function tick() {
  const mg = 1500 + Math.floor(Math.random() * 2200);
  const axis = Math.random() < 0.6 ? 2 : Math.floor(Math.random() * 2);
  const ts = new Date();
  await events.insertOne({ ts, meta: { badgeId, fw: FW }, mag: mg, axis,
                           seq: ++seq, flags: 0, clipped: false, weak: mg < 2250 });
  await badges.updateOne({ _id: badgeId },
    { $set: { lastBump: ts, lastMag: mg, lastAxis: axis, fw: FW },
      $max: { peakMag: mg }, $inc: { bumpCount: 1 } }, { upsert: true });
  console.log(`seed bump ${seq}: ${mg} mg`);
  setTimeout(tick, 4000 + Math.random() * 5000);
}
tick();

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, async () => { await client.close(); process.exit(0); });
}
