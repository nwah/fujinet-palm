#!/usr/bin/env node
// Bridge the Visor's "USB Library" byte stream to fujinet-pc's bus-over-IP
// TCP port, so a Palm app talking FujiBus over USB reaches FujiNet.
//
// Usage: node tools/visorbridge.js [--echo] [--trace] [--ep=N] [host:port]
//   default target localhost:1985 (run/run-fujinet.sh)
//   --echo   loop bytes back to the Visor instead (link test)
//   --trace  hex-dump traffic in both directions
//   --ep=N   bulk endpoint pair to use (default 1). When an app opens the
//            "USB Library" the Visor reports two GENERIC ports (1: 16-byte
//            packets, 2: 64-byte packets) and no HotSync port.
//
// Uses palm-sync's USB server for device discovery and the Handspring
// connection-info handshake; we take over the raw stream in onConnection().
const net = require('net');
const path = require('path');
const {UsbSyncServer} = require(path.join(__dirname, 'palm-sync'));
const {WebUSBDevice} = require(path.join(__dirname, 'palm-sync', 'node_modules', 'usb'));

// palm-sync reads 64 bytes per bulk IN transfer. The Visor's EP1 uses 16-byte
// packets and sends no zero-length packet when a write is an exact multiple
// of 16, so a 64-byte transfer stays pending and the data (e.g. a 32-byte
// FujiBus frame) is held until the next write. Read one packet at a time so
// every packet completes its transfer.
const USB_IN_PACKET = 16;
const origTransferIn = WebUSBDevice.prototype.transferIn;
WebUSBDevice.prototype.transferIn = function (ep, length) {
  return origTransferIn.call(this, ep, Math.min(length, USB_IN_PACKET));
};

const args = process.argv.slice(2);
const echo = args.includes('--echo');
const trace = args.includes('--trace');
const target = args.find((a) => !a.startsWith('--')) || 'localhost:1985';
const epArg = args.find((a) => a.startsWith('--ep='));
const ep = epArg ? Number(epArg.slice(5)) : 1;
const [host, port] = target.split(':');

function dump(dir, buf) {
  if (!trace) return;
  for (let i = 0; i < buf.length; i += 16) {
    const s = buf.subarray(i, i + 16);
    console.log(`${dir} ${[...s].map((b) => b.toString(16).padStart(2, '0')).join(' ')}`);
  }
}

class BridgeServer extends UsbSyncServer {
  constructor(...a) {
    super(...a);
    // Force the endpoint pair after palm-sync's connection-info handshake.
    for (const k of Object.keys(this.USB_INIT_FNS)) {
      const orig = this.USB_INIT_FNS[k];
      this.USB_INIT_FNS[k] = async (device) => {
        const cfg = await orig(device);
        console.log(`connection info picked ${JSON.stringify(cfg)}, using endpoint ${ep}`);
        return {inEndpoint: ep, outEndpoint: ep};
      };
    }
  }

  async onConnection(stream) {
    console.log('Visor connected');
    await new Promise((resolve) => {
      let sock = null;
      const done = () => {
        if (sock) sock.destroy();
        stream.destroy();
        resolve();
      };
      stream.on('error', (e) => { console.log('USB:', e.message); done(); });
      stream.on('end', done);
      if (echo) {
        stream.on('data', (d) => { dump('P>', d); dump('<P', d); stream.write(d); });
        return;
      }
      sock = net.connect(Number(port), host, () => console.log(`Connected to fujinet at ${target}`));
      sock.setNoDelay(true);
      sock.on('error', (e) => { console.log('TCP:', e.message); done(); });
      sock.on('close', done);
      stream.on('data', (d) => { dump('P>F', d); sock.write(d); });
      sock.on('data', (d) => { dump('F>P', d); stream.write(d); });
    });
    console.log('Visor disconnected');
  }
}

const server = new BridgeServer(async () => {});
server.start();
console.log(`Waiting for Visor (${echo ? 'echo mode' : 'bridge to ' + target})...`);
process.on('SIGINT', async () => { await server.stop(); process.exit(0); });
