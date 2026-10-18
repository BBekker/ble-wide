// SPDX-License-Identifier: MIT
// Loopback-only HTTP/WebSocket sample transport. All signal processing is in the browser.
import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {dirname, resolve, extname} from 'node:path';
import {spawn} from 'node:child_process';
import {WebSocketServer} from 'ws';

const root = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const option = (name, fallback) => {const i = args.indexOf(name); return i < 0 ? fallback : args[i+1];};
const port = Number(option('--port', '8077'));
const bits = Number(option('--bits', '8'));
const file = option('--file', null);
const replay = ['--file', file, '--repeat', option('--repeat', '1')];
if (args.includes('--raw14')) replay.push('--raw14');
if (args.includes('--pace')) replay.push('--pace');
const relayArgs = file ? replay : ['--stream', option('--stream', '/w1700k-iq4')];
const mime = {'.html':'text/html; charset=utf-8', '.mjs':'text/javascript', '.wasm':'application/wasm'};
const server = createServer(async (req, res) => {
    // Never expose arbitrary local files, captures, or the Emscripten SDK.
    const path = new URL(req.url, 'http://localhost').pathname;
    const allowed = new Set(['/index.html', '/app.mjs', '/coordinator.mjs', '/filter-worker.mjs',
        '/advertisement.mjs', '/build/ble.mjs', '/build/ble.wasm']);
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    res.setHeader('Cache-Control', 'no-store');
    const target = path === '/' ? '/index.html' : path;
    if (!allowed.has(target)) {res.writeHead(404).end(); return;}
    try {
        const data = await readFile(resolve(root, '.'+target));
        res.writeHead(200, {'Content-Type': mime[extname(target)] || 'application/octet-stream'}).end(data);
    } catch {res.writeHead(404).end('Build the browser receiver first.');}
});
const wss = new WebSocketServer({noServer:true, perMessageDeflate:false, maxPayload:4096});
let active = null;
server.on('upgrade', (req, socket, head) => {
    // Restrict browser origins as well as the listening address: a random web
    // page must not be able to subscribe to locally captured radio samples.
    const origin = req.headers.origin;
    if (req.url !== '/samples' || !origin || ![`http://127.0.0.1:${port}`, `http://localhost:${port}`].includes(origin) || active) {
        socket.end('HTTP/1.1 403 Forbidden\r\n\r\n'); return;
    }
    wss.handleUpgrade(req, socket, head, ws => wss.emit('connection', ws));
});
wss.on('connection', ws => {
    active = ws;
    const pending = new Set();
    let child = null, need = 0, skipped = 0, lastFrame = 0, ended = false;
    ws.send(JSON.stringify({type:'hello', bits, rate:160000000, center:2441000000, maxInFlight:8}));
    ws.on('message', data => {
        let message;
        try {message = JSON.parse(data);} catch {ws.close(1008, 'Invalid control message'); return;}
        if (message.type === 'ready' && !child) {
            child = spawn(resolve(root, 'build/sample-relay'), relayArgs, {stdio:['ignore','pipe','pipe']});
            child.stderr.on('data', x => process.stderr.write(x));
            child.on('error', error => {ws.send(JSON.stringify({type:'error', message:error.message})); ws.close();});
            child.on('close', code => {ended = true; if (code) ws.send(JSON.stringify({type:'error', message:`Sample relay exited ${code}`}));});
            child.stdout.on('readable', () => {
                for (;;) {
                    if (!need) {
                        const size = child.stdout.read(4);
                        if (!size) return;
                        need = size.readUInt32LE();
                        if (need < 40 || need > 40 + 163840*4) {ws.close(1011, 'Invalid sample frame'); return;}
                    }
                    const frame = child.stdout.read(need);
                    if (!frame) return;
                    need = 0;
                    if (frame.readUInt32LE(4) & 4) {
                        ws.send(JSON.stringify({type:'eof'})); continue;
                    }
                    const id = frame.readUInt32LE(36);
                    if (pending.size >= 8 || ws.bufferedAmount > 8*1024*1024) {skipped++; continue;}
                    pending.add(id); lastFrame = Date.now();
                    ws.send(frame, {binary:true});
                }
            });
        } else if (message.type === 'ack' && Number.isInteger(message.id)) pending.delete(message.id);
    });
    const timer = setInterval(() => {
        if (ws.readyState === ws.OPEN) ws.send(JSON.stringify({type:'transport', active:!ended && Date.now()-lastFrame < 2000, skipped, inFlight:pending.size}));
    }, 1000);
    ws.on('close', () => {clearInterval(timer); child?.kill('SIGTERM'); active = null;});
});
server.listen(port, '127.0.0.1', () => console.log(`Browser BLE receiver: http://127.0.0.1:${port}/ (${bits}-bit source)`));
for (const signal of ['SIGTERM', 'SIGINT']) process.on(signal, () => {
    active?.close(); wss.close(); server.close(); setTimeout(() => process.exit(), 500).unref();
});
