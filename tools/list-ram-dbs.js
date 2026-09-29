// palm-sync conduit: list RAM databases with their attribute flags, and
// flag any 'appl' database that is not a resource database (such an app
// hangs or crashes the system when it is launched, including at reset).
// Usage: node tools/palm-sync/dist/bin/cli.js run --usb tools/list-ram-dbs.js
const path = require('path');
const ps = require(path.join(__dirname, 'palm-sync'));

exports.run = async function (conn) {
  const list = await ps.readDbList(conn, {rom: false, ram: true});
  console.log(`== RAM databases (${list.length})`);
  for (const d of list) {
    const f = d.dbFlags;
    const flags = Object.keys(f).filter((k) => f[k] === true).join(',');
    const bad = d.type === 'appl' && !f.resDB ? '   <-- appl but NOT a resource DB' : '';
    console.log(`${d.type} ${d.creator} v${d.version}  ${JSON.stringify(d.name)}  [${flags}]${bad}`);
  }
};
