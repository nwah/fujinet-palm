// palm-sync conduit: dump ROM/RAM database list (name/type/creator), ROM
// version and a few features relevant to serial/USB access.
// Usage: node tools/palm-sync/dist/bin/cli.js run --usb tools/probe-device.js
const path = require('path');
const ps = require(path.join(__dirname, 'palm-sync'));

async function feature(conn, creator, num) {
  try {
    const r = await conn.execute(ps.DlpReadFeatureReqType.with({ftrCreator: creator, ftrNum: num}));
    return '0x' + (r.feature >>> 0).toString(16);
  } catch (e) {
    return 'absent';
  }
}

exports.run = async function (conn) {
  const si = await conn.execute(ps.DlpReadSysInfoReqType.with({}));
  console.log('ROM version:', JSON.stringify(si.romSWVersion), 'prodId:', si.prodId.toString('hex'));
  for (const [c, n, label] of [
    ['smgr', 1, 'serial mgr: new-serial present (sysFtrNewSerialPresent)'],
    ['smgr', 2, 'serial mgr: version (sysFtrNewSerialVersion)'],
    ['psys', 1, 'system: ROM version (sysFtrNumROMVersion)'],
    ['psys', 2, 'system: product id'],
    ['hsEx', 0, 'Handspring ext: version'],
    ['hsEx', 1, 'Handspring ext: 1'],
    ['netl', 0, 'NetLib version'],
  ]) {
    console.log(`feature ${c}/${n} ${label}: ${await feature(conn, c, n)}`);
  }
  for (const where of ['rom', 'ram']) {
    const list = await ps.readDbList(conn, {rom: where === 'rom', ram: where === 'ram'});
    console.log(`\n== ${where.toUpperCase()} databases (${list.length})`);
    for (const d of list) console.log(`${d.type} ${d.creator}  ${d.name}`);
  }
};
