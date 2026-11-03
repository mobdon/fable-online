// Puts a ChatHub on TCP: one JSON object per line. TLS goes in front later (stunnel or the gateway).
import { createServer, type Server, type Socket } from "node:net";
import { ChatHub, type Conn } from "./hub.ts";
import { encode, parseClientMsg, MAX_LINE_BYTES, type ServerMsg } from "./protocol.ts";

const MAX_BAD_LINES = 10;
const IDLE_MS = 90_000;            // clients ping every 30 s
const MAX_QUEUED_BYTES = 256 * 1024; // a client that stops reading is dropped, not buffered forever

export function listen(hub: ChatHub, port: number, host = "0.0.0.0"): Promise<Server> {
  const server = createServer((sock) => attach(hub, sock));
  return new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(port, host, () => resolve(server));
  });
}

function attach(hub: ChatHub, sock: Socket) {
  sock.setNoDelay(true);
  sock.setTimeout(IDLE_MS);
  let buf = "";
  let bad = 0;
  let open = true;
  const conn: Conn = {
    send(m: ServerMsg) {
      if (!open) return;
      if (sock.writableLength > MAX_QUEUED_BYTES) { conn.close("slow"); return; }
      sock.write(encode(m));
    },
    close() {
      if (!open) return;
      open = false;
      sock.end();
      setTimeout(() => sock.destroy(), 1000).unref();
    },
  };
  hub.open(conn);
  sock.setEncoding("utf8");
  sock.on("data", (chunk: string) => {
    buf += chunk;
    let nl: number;
    while ((nl = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (!line) continue;
      const m = parseClientMsg(line);
      if (!m) {
        if (++bad > MAX_BAD_LINES) return conn.close("bad");
        conn.send({ t: "err", code: "bad_message", text: "Could not read that message." });
        continue;
      }
      hub.handle(conn, m);
      if (!open) return;
    }
    if (Buffer.byteLength(buf) > MAX_LINE_BYTES) conn.close("line too long");
  });
  sock.on("timeout", () => conn.close("idle"));
  sock.on("error", () => {});
  sock.on("close", () => { open = false; hub.closed(conn); });
}
