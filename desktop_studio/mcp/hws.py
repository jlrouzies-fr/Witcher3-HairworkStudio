"""Shell access to the Hairwork Studio MCP tools for agents without MCP registration.

  <python> hws.py daemon                      # start once (background): keeps ONE live Studio session on 127.0.0.1:47613
  <python> hws.py <tool> '<json arguments>'   # call a tool, e.g.  <python> hws.py studio_render '{"shot":15,"overlay":"scalp"}'
  <python> hws.py list                        # tool names + descriptions
  <python> hws.py stop
  <python> hws.py freeport                    # print a random free port for HWS_PORT (the daemon refuses a port in use)
                                              # one session = one HWS_PORT + one HWS_SESSION folder: the server also refuses
                                              # an output folder another live daemon holds (<out>/.hws_lock.json)

Text results are printed; images are saved as PNG under build/desktop_studio/mcp/img/ and their paths printed (open
them with an image viewer / the Read tool).
"""
import base64
import json
import os
import socket
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = int(os.environ.get("HWS_PORT", "47613"))   # one port per parallel session (agents working at once)
IMG = os.path.join(os.environ.get("HWS_OUT") or os.path.join(os.path.dirname(os.path.dirname(HERE)), "build", "desktop_studio", "mcp"),
                   os.environ.get("HWS_SESSION", ""), "img")


def daemon():
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "hairwork_studio_mcp.py")], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE)
    n = [0]

    def rpc(method, params):
        n[0] += 1
        p.stdin.write((json.dumps({"jsonrpc": "2.0", "id": n[0], "method": method, "params": params}) + "\n").encode())
        p.stdin.flush()
        line = p.stdout.readline()
        if not line.strip():   # the server refused to start (e.g. another daemon holds the output folder: its message is above)
            sys.exit("hairwork_studio_mcp.py ended before answering (see its message above)")
        return json.loads(line)

    # refuse a port another daemon already holds (SO_REUSEADDR let two sessions share one on Windows, 2026-10-16:
    # another agent's calls landed in this Studio); bind exclusively before starting the Studio
    srv = socket.socket()
    if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    try:
        srv.bind(("127.0.0.1", PORT))
    except OSError:
        p.kill()
        sys.exit(f"port {PORT} is in use (another hws daemon?): pick another HWS_PORT, e.g. `hws.py freeport`")
    rpc("initialize", {"protocolVersion": "2025-06-18"})
    srv.listen(1)
    print("hws daemon ready on", PORT, flush=True)
    while True:
        c, _ = srv.accept()
        data = b""
        while not data.endswith(b"\n"):
            chunk = c.recv(1 << 20)
            if not chunk:
                break
            data += chunk
        req = json.loads(data)
        if req.get("stop"):
            c.sendall(b'{"stopped":true}\n')
            c.close()
            p.stdin.close()
            p.wait(30)
            return
        resp = rpc(req["method"], req["params"])
        c.sendall((json.dumps(resp) + "\n").encode())
        c.close()


def send(req):
    c = socket.create_connection(("127.0.0.1", PORT))
    c.sendall((json.dumps(req) + "\n").encode())
    data = b""
    while True:
        chunk = c.recv(1 << 20)
        if not chunk:
            break
        data += chunk
    return json.loads(data)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    if a[0] == "daemon":
        return daemon()
    if a[0] == "freeport":   # a random free port for HWS_PORT
        import random
        while True:
            port = random.randint(49200, 59999)
            t = socket.socket()
            try:
                t.bind(("127.0.0.1", port)); t.close(); print(port); return
            except OSError:
                t.close()
    if a[0] == "stop":
        print(send({"stop": True}))
        return
    if a[0] == "list":
        for t in send({"method": "tools/list", "params": {}})["result"]["tools"]:
            print(f"{t['name']}: {t['description']}\n    args: {json.dumps(t['inputSchema'].get('properties', {}))[:600]}")
        return
    raw = a[1] if len(a) > 1 else "{}"
    try:
        args = json.loads(raw)
    except json.JSONDecodeError:   # Windows paths with single backslashes: accept them
        args = json.loads(raw.replace("\\", "/"))
    r = send({"method": "tools/call", "params": {"name": a[0], "arguments": args}})
    if "error" in r:
        print("ERROR", r["error"])
        return
    os.makedirs(IMG, exist_ok=True)
    k = len(os.listdir(IMG))
    for c in r["result"]["content"]:
        if c["type"] == "image":
            k += 1
            path = os.path.join(IMG, f"{k:04d}_{a[0]}.png")
            open(path, "wb").write(base64.b64decode(c["data"]))
            print("IMAGE:", path)
        else:
            print(c["text"])
    if r["result"].get("isError"):
        sys.exit(1)


if __name__ == "__main__":
    main()
