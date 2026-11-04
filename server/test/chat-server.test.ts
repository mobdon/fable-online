// End to end over a real socket.
import { test } from "node:test";
import assert from "node:assert/strict";
import { connect } from "node:net";
import { ChatHub } from "../src/chat/hub.js";
import { listen } from "../src/chat/server.js";

function client(port: number) {
  const sock = connect(port, "127.0.0.1");
  sock.setEncoding("utf8");
  const lines: any[] = [];
  let buf = "";
  const waiters: (() => void)[] = [];
  sock.on("data", (d: string) => {
    buf += d;
    let i;
    while ((i = buf.indexOf("\n")) >= 0) { lines.push(JSON.parse(buf.slice(0, i))); buf = buf.slice(i + 1); }
    waiters.splice(0).forEach((w) => w());
  });
  const next = async (t: string) => {
    for (;;) {
      const i = lines.findIndex((l) => l.t === t);
      if (i >= 0) return lines.splice(i, 1)[0];
      await new Promise<void>((r) => waiters.push(r));
    }
  };
  return { sock, next, send: (o: unknown) => sock.write(JSON.stringify(o) + "\n"), raw: (s: string) => sock.write(s) };
}

test("two clients chat over TCP", async () => {
  const hub = new ChatHub();
  const server = await listen(hub, 0, "127.0.0.1");
  const port = (server.address() as any).port;
  const a = client(port), b = client(port);
  a.send({ t: "hello", v: 1, name: "Alpha" });
  b.send({ t: "hello", v: 1, name: "Bravo" });
  await a.next("welcome"); await b.next("welcome");
  a.raw('{"t":"say","ch":"realm",');            // split across packets
  a.raw('"text":"hi there"}\n{"t":"ping","n":7}\n');
  assert.equal((await b.next("msg")).text, "hi there");
  assert.equal((await a.next("pong")).n, 7);
  a.send({ nonsense: true });
  assert.equal((await a.next("err")).code, "bad_message");
  a.sock.destroy(); b.sock.destroy();
  await new Promise((r) => server.close(r));
});
