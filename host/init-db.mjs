/* init-db.mjs — create the collections. RUN THIS FIRST, ONCE, CAREFULLY.
 *
 * WHY THIS IS ITS OWN SCRIPT: a collection cannot be converted into a time
 * series collection after the fact. It is a create-time property. Getting it
 * wrong means dropping and re-creating, and if you only discover that at 18:00
 * you have lost the data you were going to demo with.
 *
 * Two collections, and the split is forced by a hard MongoDB limitation:
 * time series collections support NEITHER change streams NOR triggers NOR
 * $search. So the obvious "bumps -> time series -> change stream -> live UI"
 * is impossible. The time series holds the measurements; a small regular
 * collection holds per-badge rollups and is what the live page watches.
 *
 *   node --env-file=.env init-db.mjs
 */
import { MongoClient } from 'mongodb';

const uri = process.env.MONGODB_URI;
const dbName = process.env.MONGODB_DB ?? 'badge';
if (!uri) { console.error('MONGODB_URI is not set. Copy .env.example to .env.'); process.exit(1); }

const client = new MongoClient(uri);

try {
  await client.connect();
  const db = client.db(dbName);
  const existing = new Set((await db.listCollections({}, { nameOnly: true }).toArray()).map(c => c.name));

  if (existing.has('bump_events')) {
    // Verify it is actually a time series collection, because a regular
    // collection with the right name is the failure this script exists to
    // prevent, and it is silent.
    const [info] = await db.listCollections({ name: 'bump_events' }).toArray();
    if (info.type !== 'timeseries') {
      console.error('\n*** bump_events EXISTS BUT IS NOT A TIME SERIES COLLECTION ***');
      console.error('It cannot be converted. Drop it and re-run:');
      console.error(`   db.bump_events.drop()\n`);
      process.exit(2);
    }
    console.log('bump_events: already a time series collection, OK');
  } else {
    await db.createCollection('bump_events', {
      timeseries: {
        timeField: 'ts',       // the measurement timestamp (a real Date)
        metaField: 'meta',     // LOW-CARDINALITY ONLY: badgeId, fw
        granularity: 'seconds' // bumps arrive seconds apart at most
      },
      expireAfterSeconds: 86400 // a one-day demo needs one day of history
    });
    console.log('bump_events: created as TIME SERIES (timeField=ts, metaField=meta)');
  }

  // The index that makes "this badge, most recent first" cheap. On a time
  // series collection the index goes on the meta subfields + timeField.
  await db.collection('bump_events').createIndex({ 'meta.badgeId': 1, ts: -1 });
  console.log('bump_events: index {meta.badgeId:1, ts:-1} ensured');

  if (!existing.has('badges')) {
    await db.createCollection('badges');
    console.log('badges: created (regular collection -- this is what the change stream watches)');
  } else {
    console.log('badges: already exists, OK');
  }

  console.log('\nReady. Next: npm run seed   (proves the DB half with no hardware)');
} catch (err) {
  console.error('init failed:', err.message);
  process.exitCode = 1;
} finally {
  await client.close();
}
