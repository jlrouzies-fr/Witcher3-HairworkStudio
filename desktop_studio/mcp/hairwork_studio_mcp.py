r"""Hairwork Studio MCP server: a live, hidden Studio session exposed as MCP tools.

The server starts `HairworkStudio.exe --serve` (no window, no input, no focus) and talks to it over stdin / stdout:
one JSON command per line in, one `@@HWS {json}` reply line out (other stdout lines are log output). Every tool returns
JSON text, and render tools also return the image itself, so an agent edits and SEES the result in one loop.

MCP transport: stdio, newline-delimited JSON-RPC 2.0 (no third-party packages; runs on the Studio's bundled Python).
Register in Claude Code:  claude mcp add hairwork-studio -- <root>\python\python.exe <root>\desktop_studio\mcp\hairwork_studio_mcp.py
(<root> = the unzipped Studio release, or a clone of the repo with `py` instead of the bundled python.) HAIRWORK_STUDIO_EXE
overrides the exe, HWS_OUT the output folder (default <root>/build/desktop_studio\mcp, written next to the Studio).
Our own code (Studio License). Never exposes NVIDIA SDK material.
"""
import base64
import io
import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
STUDIO_DIR = os.path.dirname(HERE)
REPO = os.path.dirname(STUDIO_DIR)
PROTOCOL = "2025-06-18"

try:
    from PIL import Image
except ImportError:   # images are then returned as written by the Studio, without crop / resize / sheets
    Image = None


def find_exe():
    env = os.environ.get("HAIRWORK_STUDIO_EXE")
    cands = [env] if env else []
    cands += [os.path.join(REPO, "HairworkStudio.exe"),                 # unzipped release: <root>\desktop_studio\mcp\..
              os.path.join(STUDIO_DIR, "HairworkStudio.exe"),
              os.path.join(os.path.dirname(STUDIO_DIR), "HairworkStudio.exe"),
              os.path.join(STUDIO_DIR, "build", "Release", "HairworkStudio.exe")]   # clone built from source
    for c in cands:
        if c and os.path.isfile(c):
            return c
    raise RuntimeError("HairworkStudio.exe not found (set HAIRWORK_STUDIO_EXE)")


def log(*a):
    print("[hairwork-studio-mcp]", *a, file=sys.stderr, flush=True)


# ============================================================================================ Studio process
class Studio:
    def __init__(self):
        self.p = None
        self.lock = threading.Lock()
        self.out = os.path.join(os.environ.get("HWS_OUT") or os.path.join(REPO, "build", "desktop_studio", "mcp"), os.environ.get("HWS_SESSION", ""))   # per parallel session
        os.makedirs(self.out, exist_ok=True)
        self.n = 0
        self.cfg = None
        self.journal = []   # open + state-changing commands: replayed when the Studio process restarts
        self.pending_snapshot = None   # an opened USER save (UI-saved project) not yet snapshotted to <char>_user_HHMM
        self.snapshots = []
        self._lock_path = None
        self.take_folder_lock()

    # ---- helper-folder lock (2026-10-16): two daemons must never share one output folder (one agent's renders /
    # saves landed in another's, HAIRWORK "Regis Studio pass 3"); the port lock lives in hws.py
    def take_folder_lock(self):
        lp = os.path.join(self.out, ".hws_lock.json")
        try:
            other = json.load(open(lp))
            if other.get("pid") != os.getpid() and pid_alive(other.get("pid")):
                raise SystemExit("output folder %s is in use by another hairwork-studio daemon (pid %s, port %s, since %s): "
                                 "use your own HWS_SESSION / HWS_OUT" % (self.out, other.get("pid"), other.get("port"), other.get("since")))
        except (OSError, ValueError):
            pass
        json.dump({"pid": os.getpid(), "port": os.environ.get("HWS_PORT"), "since": time.strftime("%Y-%m-%d %H:%M:%S")}, open(lp, "w"))
        self._lock_path = lp

    def release_folder_lock(self):
        try:
            if self._lock_path and json.load(open(self._lock_path)).get("pid") == os.getpid():
                os.remove(self._lock_path)
        except (OSError, ValueError):
            pass

    # ---- user-save snapshot (2026-10-16, memory ref-studio-user-saves): the Studio's Save writes over the opened
    # project, so a project saved from the UI ("saved_by": "ui", or an old file newer than its edits.json) is copied to
    # <char>_user_HHMM (HHMM = its save time) before the first MCP edit; agents build on the snapshot
    def note_opened(self, path):
        self.pending_snapshot = None
        if not path or not os.path.isfile(path):
            return
        name = os.path.basename(path)
        if re.search(r"_user_\d{4}(_\d+)?\.groomproj\.json$", name):
            return   # already a snapshot name
        try:
            P = json.load(open(path, encoding="utf-8-sig"))
        except Exception:
            return
        by = P.get("saved_by")
        user = by == "ui"
        if by is None:   # before the stamp: a project newer than the edits.json the MCP save wrote next to it was re-saved by the user
            ej = os.path.join(os.path.dirname(path), "edits.json")
            user = os.path.exists(ej) and os.path.getmtime(path) > os.path.getmtime(ej) + 60
        if user:
            self.pending_snapshot = path

    def snapshot_now(self):
        src = self.pending_snapshot
        self.pending_snapshot = None
        if not src:
            return None
        try:
            P = json.load(open(src, encoding="utf-8-sig"))
            char = (P.get("config") or os.path.basename(src)).split("_")[0]
            hhmm = time.strftime("%H%M", time.localtime(os.path.getmtime(src)))
            parent = os.path.dirname(os.path.dirname(src))
            base = "%s_user_%s" % (char, hhmm)
            data = open(src, "rb").read()
            name = base
            k = 1
            while True:
                dst = os.path.join(parent, name, name + ".groomproj.json")
                if not os.path.exists(dst):
                    break
                if open(dst, "rb").read() == data:
                    return dst   # the same save is already snapshotted
                k += 1
                name = "%s_%d" % (base, k)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(data)
            self.snapshots.append(dst)
            log("user save snapshotted:", src, "->", dst)
            return dst
        except Exception as e:
            log("snapshot failed:", e)
            return None

    def start(self):
        if self.p and self.p.poll() is None:
            return
        exe = find_exe()
        env = dict(os.environ, GROOM_STUDIO_NOACTIVATE="1")
        self.p = subprocess.Popen([exe, "--serve", "--out", self.out], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, cwd=os.path.dirname(exe), env=env,
                                  creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        hello = self._read_reply(120)
        log("studio started:", hello)

    def _read_reply(self, timeout):
        t0 = time.time()
        tail = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("Studio process ended. Last log lines:\n" + "\n".join(tail[-15:]))
            s = line.decode("utf-8", "replace").rstrip("\r\n")
            if s.startswith("@@HWS "):
                return json.loads(s[6:])
            tail.append(s)
            if time.time() - t0 > timeout:
                raise RuntimeError("Studio timed out")

    READ_ONLY = {"status", "shots_list", "render", "guides", "guide", "faces", "pick", "pick_region", "scalp_coverage",
                 "look_get", "materials_list", "save_edits", "save_project", "export", "dump_params", "bench", "select", "dump_geo",
                 "wind_check", "list_colliders", "collider_report", "drape_report", "list_meshes", "handback_gate", "gate_status", "gate_fail",
                 "check_loops", "hairline_check", "eye_check", "band_sim"}

    def _send(self, cmd, timeout):
        self.p.stdin.write((json.dumps(cmd) + "\n").encode())
        self.p.stdin.flush()
        return self._read_reply(timeout)

    def _replay(self):
        for c in self.journal:
            self._send(c, 600)

    def call(self, cmd, timeout=600):
        with self.lock:
            restarted = self.p is not None and self.p.poll() is not None
            self.start()
            if restarted:
                self._replay()
            try:
                r = self._send(cmd, timeout)
            except RuntimeError as e:
                if "ended" not in str(e):
                    raise
                log("Studio crashed, restarting and replaying", len(self.journal), "commands")
                self.start()
                self._replay()
                r = self._send(cmd, timeout)
                r["note"] = "the Studio process crashed and was restarted; your edits were replayed"
            if r.get("error"):
                raise RuntimeError(r["error"])
            d = cmd.get("do")
            if d == "open":
                self.journal = [cmd]
                self.note_opened(cmd.get("project"))
            elif d not in self.READ_ONLY and not cmd.get("baseline") and not cmd.get("dry_run") and self.pending_snapshot:
                snap = self.snapshot_now()
                if snap:
                    r["user_save_snapshot"] = snap
            elif d == "reset_edits":
                self.journal = self.journal[:1]
            elif d not in self.READ_ONLY and not cmd.get("baseline"):
                self.journal.append(cmd)
            return r

    def stop(self):
        if self.p and self.p.poll() is None:
            try:
                self.p.stdin.write(b'{"do":"quit"}\n')
                self.p.stdin.flush()
                self.p.wait(15)
            except Exception:
                self.p.kill()
        self.release_folder_lock()


    def png(self, tag):
        self.n += 1
        return os.path.join(self.out, f"{self.n:04d}_{tag}.png")


def pid_alive(pid):
    if not pid:
        return False
    try:
        import ctypes
        h = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pid))   # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(h)
        return bool(ok) and code.value == 259
    except Exception:
        return False


S = Studio()


# ============================================================================================ image helpers
def crop_img(im, crop):
    """crop = [x, y, w, h] in 0..1 of the image, or a named region of the head shots."""
    named = {"top": [0.2, 0.0, 0.6, 0.4], "forehead": [0.25, 0.2, 0.5, 0.35], "face": [0.2, 0.15, 0.6, 0.6],
             "full": [0, 0, 1, 1]}
    c = named.get(crop, crop) if isinstance(crop, str) else crop
    W, H = im.size
    x, y, w, h = c
    return im.crop((int(x * W), int(y * H), int((x + w) * W), int((y + h) * H)))


def image_content(path, crop=None, max_px=1000):
    if Image is None:
        data = open(path, "rb").read()
        return {"type": "image", "data": base64.b64encode(data).decode(), "mimeType": "image/png"}
    im = Image.open(path).convert("RGB")
    if crop:
        im = crop_img(im, crop)
    if max(im.size) > max_px:
        im.thumbnail((max_px, max_px))
    b = io.BytesIO()
    im.save(b, "PNG")
    return {"type": "image", "data": base64.b64encode(b.getvalue()).decode(), "mimeType": "image/png"}


def sheet(paths, labels, out, crop=None, h=700):
    ims = []
    for p in paths:
        im = Image.open(p).convert("RGB")
        if crop:
            im = crop_img(im, crop)
        ims.append(im.resize((max(1, im.width * h // im.height), h)))
    from PIL import ImageDraw
    W = sum(i.width for i in ims) + 6 * (len(ims) - 1)
    s = Image.new("RGB", (W, h + 28), (24, 24, 28))
    x = 0
    d = ImageDraw.Draw(s)
    for im, lab in zip(ims, labels):
        s.paste(im, (x, 28))
        d.text((x + 8, 8), lab, fill=(230, 220, 200))
        x += im.width + 6
    s.save(out)
    return out


# ============================================================================================ tools
WHERE = {"type": "object", "description":
         "Guide selector: {all}, {indices:[..]}, {roots:[[x,y,z],..]} (4 mm match), {back|front|left|right: true}, "
         "{z_min, z_max} (m, root height relative to the head top), {tip_below: m}, {near: [x,y,z], radius: m}, "
         "{max_length / min_length: m}, {passes_near: [x,y,z], radius: m} (any point of the strand, e.g. hair over an "
         "ear), or the string \"selection\". Keys combine (AND)."}
CAM = {
    "shot": {"type": "integer", "description": "HairLab camera id from studio_shots (3 = 1/4 front right, 15 = 1/4 front left, 4 front, 0 back, 2 / 5 sides)"},
    "view": {"type": "string", "description": "top (top-front camera fitted to this character's head) or orbit preset: front, back, left, right, 34, back34 (used when no shot)"},
    "eye": {"type": "array", "items": {"type": "number"}, "description": "explicit camera [x,y,z] (with target)"},
    "target": {"type": "array", "items": {"type": "number"}},
    "fovy": {"type": "number"},
}
IMG = {
    "size": {"type": "integer", "description": "render size in px (default 1400)"},
    "crop": {"description": "[x,y,w,h] in 0..1, or 'top' | 'forehead' | 'face' | 'full' (applied to the returned image)"},
    "max_px": {"type": "integer", "description": "longest side of the returned image (default 1000)"},
}


def obj(props, req=()):
    return {"type": "object", "properties": props, "required": list(req)}


TOOLS = {}


def tool(name, desc, schema):
    def deco(fn):
        TOOLS[name] = {"name": name, "description": desc, "inputSchema": schema, "fn": fn}
        return fn
    return deco


def cam_args(a):
    return {k: a[k] for k in ("shot", "view", "eye", "target", "fovy") if k in a}


def render(a, tag, extra=None):
    png = S.png(tag)
    cmd = {"do": "render", "path": png, "size": a.get("size", 1400), **cam_args(a), **(extra or {})}
    if a.get("overlay"):
        cmd["overlay"] = a["overlay"]
    r = S.call(cmd)
    return png, r


# --- session
@tool("studio_list_configs", "List the hair builds the Studio can open (configs with a built fur; '*' = approved, frozen).",
      obj({}))
def t_list(a):
    py = sys.executable
    r = subprocess.run([py, os.path.join(STUDIO_DIR, "tools", "desk_prep.py"), "--list"], capture_output=True, text=True,
                       cwd=REPO, timeout=120)
    return {"configs": r.stdout.strip().splitlines()}


@tool("studio_open", "Open a hair build (cfg, e.g. guillaume_l124_locks_hug) or a saved *.groomproj.json project. "
      "Edits are kept in memory only; approved builds are never modified.",
      obj({"cfg": {"type": "string"}, "project": {"type": "string"}, "reprep": {"type": "boolean"},
           "edits": {"type": "string", "description": "optional edits JSON ({ops:[..]}) to apply right after opening"}}))
def t_open(a):
    r = S.call({"do": "open", **{k: a[k] for k in ("cfg", "project", "reprep") if k in a}})
    CALLS.clear()   # undo-by-call markers belong to the previous build
    if a.get("edits"):
        r["edits"] = S.call({"do": "ops", "file": a["edits"]})
    S.cfg = a.get("cfg") or a.get("project")
    return r


@tool("studio_status", "Current build, guide / face / op counts, unmatched roots, hair budget vs the reference (ratio, information only: never a reason to stop or thin).",
      obj({}))
def t_status(a):
    return S.call({"do": "status"})


@tool("studio_fur", "Beard mode (builds with a beard fur, e.g. Zoltan / Lambert beard configs: status has_beard). "
      "fur: 'beard' makes the beard fur the edited groom: every tool, look / material, physics, check, render overlay "
      "and the budget then act on the beard, and the hair is a preview; 'hair' switches back. Each fur keeps its own "
      "edits and undo; studio_save writes both into the project and the export.",
      obj({"fur": {"type": "string", "enum": ["hair", "beard"]}}, ["fur"]))
def t_fur(a):
    r = S.call({"do": "fur", "fur": a["fur"]})
    CALLS.clear()   # undo-by-call markers count the other fur's ops
    return r


@tool("studio_shots","Cameras of the open build (HairLab shots with eye / target / fovy and the vanilla in-game still).",
      obj({}))
def t_shots(a):
    return S.call({"do": "shots_list"})


# --- seeing
@tool("studio_render", "Render the current groom and return the image. overlay: 'scalp' = skin in flat green "
      "(visible scalp = green pixels), 'mask' = hair-only flat red, 'guides' = guide curves drawn, none = normal look. "
      "baseline: true renders the build as opened (no edits) for before / after.",
      obj({**CAM, **IMG, "overlay": {"type": "string", "enum": ["none", "scalp", "mask", "guides"]},
           "baseline": {"type": "boolean"}, "hair": {"type": "boolean", "description": "false = hide the hair (see the scalp mask / skin)"},
           "vanilla": {"type": "boolean", "description": "render the game's own hair cards instead of the groom"},
           "guides_front": {"type": "boolean", "description": "with overlay 'guides': only the front-layer guides (default: studio_guide_view)"},
           "guides_body": {"type": "boolean", "description": "with overlay 'guides': hide guide parts behind the body (default: studio_guide_view)"},
           "colliders": {"type": "boolean", "description": "draw the collision capsules (the Studio's N overlay) in this render"},
           "headgear": {"type": "boolean", "description": "draw the character's helmet / hat / hood in this render (default: studio_headgear)"}}))
def t_render(a):
    extra = {"baseline": a.get("baseline", False), "hair": a.get("hair", True), "vanilla": a.get("vanilla", False)}
    for k in ("guides_front", "guides_body", "colliders", "headgear"):
        if k in a:
            extra[k] = a[k]
    png, r = render(a, "render", extra)
    return [r, image_content(png, a.get("crop"), a.get("max_px", 1000))]


@tool("studio_headgear", "View > 'Show headgear / outfit items': the helmet / hat / hood meshes the character wears in "
      "the game (every appearance of its template), to see hair clipping through them. on: show / hide; app: which "
      "appearance's headgear (see 'apps' in the reply). No arguments = report the state. Default on only when every "
      "appearance wears headgear.",
      obj({"on": {"type": "boolean"}, "app": {"type": "string"}}))
def t_headgear(a):
    return S.call(dict({"do": "headgear"}, **{k: a[k] for k in ("on", "app") if k in a}))


@tool("studio_guide_view", "Guide visibility views (the Studio's View > 'Guides: front layer only' / 'Guides: hide behind "
      "the body'). front: only guides that are the outermost hair from the camera (front_pct % of their points within "
      "tol_mm of the front-most hair / body depth); body: guide parts hidden by the head / body / outfit are not drawn. "
      "The modes stay set for the session (renders with overlay 'guides', the UI). With camera keys: counts the guides "
      "shown from that camera (rect [x,y,w,h] 0..1 = only those with a drawn point inside); select: true selects them "
      "(as the UI's box select with 'Select visible only'); limit n lists their indices.",
      obj({**CAM, "front": {"type": "boolean"}, "body": {"type": "boolean"},
           "front_pct": {"type": "number", "description": "front layer: share of points that must be front-most (50)"},
           "tol_mm": {"type": "number", "description": "front layer: depth tolerance behind the outermost hair (12 mm)"},
           "select_visible": {"type": "boolean", "description": "UI picking takes only shown guides (default true)"},
           "rect": {"type": "array", "items": {"type": "number"}}, "select": {"type": "boolean"},
           "limit": {"type": "integer"}, "size": IMG["size"]}))
def t_guide_view(a):
    cmd = {"do": "guide_view", **cam_args(a)}
    for k in ("front", "body", "front_pct", "tol_mm", "select_visible", "rect", "select", "limit", "size"):
        if k in a:
            cmd[k] = a[k]
    return S.call(cmd)


CLOSEUP_AREAS = {   # (azimuth deg: 0 front, + character's left; elevation deg above the head centre)
    "top": (0, 80), "crown": (180, 60), "parting": (20, 70), "back": (180, 30), "nape": (180, 0),
    "left": (90, 35), "right": (-90, 35), "front": (0, 45), "back_left": (135, 45), "back_right": (-135, 45)}


@tool("studio_closeup", "Very close shot of one scalp area to judge fine detail (bare scalp between strands, "
      "stray hairs, root density). area: " + ", ".join(CLOSEUP_AREAS) + "; or point [x,y,z] (edit frame) to look at. "
      "dist_mm (260; closer goes inside the hair shell) = camera distance from the scalp point, fovy (22) deg, size (1200) px. overlay as studio_render.",
      obj({"area": {"type": "string", "enum": list(CLOSEUP_AREAS)}, "point": {"type": "array", "items": {"type": "number"}},
           "dist_mm": {"type": "number"}, "fovy": {"type": "number"}, "size": IMG["size"], "max_px": IMG["max_px"],
           "overlay": {"type": "string", "enum": ["none", "scalp", "mask", "guides"]},
           "hair": {"type": "boolean"}, "vanilla": {"type": "boolean"}}))
def t_closeup(a):
    import math
    fr = S.call({"do": "guides", "where": {"indices": []}, "limit": 0}).get("frame", {})
    c = fr.get("head_centre", [0.0, 0.06, 1.65])
    top = fr.get("head_top_z", c[2] + 0.14)
    rad = (top - c[2]) * 0.92                       # scalp radius about the head centre
    if a.get("point"):
        p = list(a["point"])
        d = [p[k] - c[k] for k in range(3)]
        n = math.sqrt(sum(x * x for x in d)) or 1.0
        u = [x / n for x in d]
    else:
        az, el = CLOSEUP_AREAS[a.get("area", "crown")]
        az, el = math.radians(az), math.radians(el)
        u = [math.cos(el) * math.sin(az), math.cos(el) * math.cos(az), math.sin(el)]
        p = [c[k] + rad * u[k] for k in range(3)]
    dist = a.get("dist_mm", 260) / 1000.0
    eye = [p[k] + u[k] * dist for k in range(3)]
    b = {"eye": eye, "target": p, "fovy": a.get("fovy", 22), "size": a.get("size", 1200),
         "overlay": a.get("overlay", "none"), "max_px": a.get("max_px", 1200)}
    png, r = render(b, "render", {"baseline": False, "hair": a.get("hair", True), "vanilla": a.get("vanilla", False)})
    r = {**r, "closeup": {"eye": eye, "target": p}} if isinstance(r, dict) else r
    return [r, image_content(png, None, b["max_px"])]


@tool("studio_compare", "Side-by-side sheet: vanilla in-game still | build as opened | current edit (optionally the "
      "scalp overlay as a 4th panel). shot = one camera, or shots = [15, 4, 3] for one sheet with a row per camera. "
      "The way to judge an edit.",
      obj({"shot": {"type": "integer"}, "shots": {"type": "array", "items": {"type": "integer"}}, "crop": IMG["crop"],
           "scalp": {"type": "boolean"}, "max_px": IMG["max_px"],
           "vanilla": {"type": "string", "enum": ["studio", "capture", "both"], "description": "vanilla panel: the game's hair "
                       "cards rendered in the Studio from the same camera (studio, default when available), the in-game "
                       "still (capture), or both"}}))
def t_compare(a):
    if a.get("shots"):
        rows = []
        for sh in a["shots"]:
            r = t_compare({**{k: v for k, v in a.items() if k != "shots"}, "shot": sh})
            rows.append(r[0]["sheet"])
        ims = [Image.open(p).convert("RGB") for p in rows]
        W = max(i.width for i in ims)
        out = Image.new("RGB", (W, sum(i.height for i in ims)), (24, 24, 28))
        y = 0
        for i in ims:
            out.paste(i, (0, y))
            y += i.height
        path = S.png("compare_multi")
        out.save(path)
        return [{"sheet": path, "rows": rows}, image_content(path, None, a.get("max_px", 2000))]
    if Image is None:
        raise RuntimeError("Pillow missing")
    shots = S.call({"do": "shots_list"})["shots"]
    sh = next((s for s in shots if str(s["id"]) == str(a["shot"])), None)
    if not sh:
        raise RuntimeError("no such shot")
    base, _ = render({"shot": a["shot"]}, "base", {"baseline": True})
    cur, _ = render({"shot": a["shot"]}, "cur")
    paths, labels = [], []
    # vanilla: the game's hair cards rendered in the Studio from the same camera (pixel-aligned with the other panels)
    # when the pack has them; the in-game still too when "capture" is asked (or the cards are missing)
    mode = a.get("vanilla", "studio")
    cards = S.call({"do": "shots_list"}).get("vanilla_cards", False)
    if cards and mode in ("studio", "both"):
        van, _ = render({"shot": a["shot"]}, "vanilla", {"vanilla": True})
        paths.append(van)
        labels.append("VANILLA (cards, same camera)")
    if sh.get("ref") and (mode in ("capture", "both") or not cards):
        paths.append(os.path.join(REPO, sh["ref"]))
        labels.append("VANILLA (in game)")
    paths += [base, cur]
    labels += ["BUILD (NO EDITS)", "CURRENT (PROJECT + EDITS)"]
    info = {}
    if a.get("scalp"):
        sc, r = render({"shot": a["shot"]}, "scalp", {"overlay": "scalp"})
        paths.append(sc)
        labels.append("SCALP OVERLAY")
        info = r
    out = sheet(paths, labels, S.png("compare"), a.get("crop"))
    return [{"sheet": out, **info}, image_content(out, None, a.get("max_px", 1800))]


@tool("studio_scalp_coverage", "Count visible scalp pixels (green under the scalp overlay) for a camera, optionally in a "
      "region [x,y,w,h] (0..1; face skin is green too, so use a region on the top of the head). Also counts the build as "
      "opened (baseline) so the change is a number. Returns the green mask image.",
      obj({**CAM, "region": {"type": "array", "items": {"type": "number"}}, "size": IMG["size"]}))
def t_cov(a):
    size = a.get("size", 1400)
    cmd = {"do": "scalp_coverage", "size": size, **cam_args(a)}
    if "region" in a:
        x, y, w, h = a["region"]
        cmd["rect"] = [int(x * size), int(y * size), int((x + w) * size), int((y + h) * size)]
    S.n += 1
    r = S.call({**cmd, "tag": f"{S.n:04d}_cov"})["shots"][0]
    rb = S.call({**cmd, "tag": f"{S.n:04d}_covbase", "baseline": True})["shots"][0]
    key = "scalp_pixels_in_rect" if "region" in a else "scalp_pixels"
    res = {"camera": cam_args(a) or {"shot": 3}, "scalp_px": r.get(key), "baseline_scalp_px": rb.get(key), "whole_image_px": r.get("scalp_pixels"),
           "overlay": r.get("png"), "mask": r.get("mask")}
    out = [res]
    if r.get("png"):
        out.append(image_content(r["png"], a.get("region") and [max(0, a["region"][0] - .05), max(0, a["region"][1] - .05),
                                                                 min(1, a["region"][2] + .1), min(1, a["region"][3] + .1)], 900))
    return out


@tool("studio_pick", "What is under pixel (x, y) of a render (pixels of the given size; or auto='scalp' = the visible "
      "scalp patch, optionally searched in rect [x0,y0,x1,y1] px): hair or scalp, the world position, the growth face, "
      "and the nearest guides by root and on screen. Returns the render with a crosshair.",
      obj({**CAM, "x": {"type": "number"}, "y": {"type": "number"}, "auto": {"type": "string"},
           "rect": {"type": "array", "items": {"type": "number"}}, "size": IMG["size"],
           "nearest": {"type": "integer", "description": "number of guides to list (default 8)"}}))
def t_pick(a):
    S.n += 1
    cmd = {"do": "pick", "size": a.get("size", 1400), "n": a.get("nearest", 8), "tag": f"{S.n:04d}_pick", **cam_args(a)}
    for k in ("x", "y", "auto", "rect"):
        if k in a:
            cmd[k] = a[k]
    r = S.call(cmd)
    out = [r]
    if r.get("png"):
        out.append(image_content(r["png"], None, 1000))
    return out


@tool("studio_pick_region", "Guides whose roots project inside a screen rectangle [x,y,w,h] (0..1) of a camera, "
      "e.g. the green patch seen in a scalp render. Also lists the growth faces there with their area.",
      obj({**CAM, "rect": {"type": "array", "items": {"type": "number"}}, "size": IMG["size"]}, ["rect"]))
def t_pick_region(a):
    return S.call({"do": "pick_region", "rect": a["rect"], "size": a.get("size", 1400), **cam_args(a)})


# --- inspecting guides
@tool("studio_guides", "List guides matching a selector: index, root, tip, length (m), edited flag. limit default 200. Indices shift "
      "after deletes: to target the same guides across edits, select them by {roots: [...]} (4 mm match).",
      obj({"where": WHERE, "limit": {"type": "integer"}}))
def t_guides(a):
    return S.call({"do": "guides", "where": a.get("where", {"all": True}), "limit": a.get("limit", 200)})


@tool("studio_guide", "All control points of one guide (index).", obj({"index": {"type": "integer"}}, ["index"]))
def t_guide(a):
    return S.call({"do": "guide", "index": a["index"]})


@tool("studio_faces", "Growth faces (scalp triangles hair grows from) matching a region: index, guide ids, centroid, "
      "area mm2. Big faces grow sparse hair; densify them with studio_densify.",
      obj({"where": WHERE, "min_area_mm2": {"type": "number"}, "limit": {"type": "integer"}}))
def t_faces(a):
    return S.call({"do": "faces", "where": a.get("where", {"all": True}), "min_area_mm2": a.get("min_area_mm2", 0),
                   "limit": a.get("limit", 200)})


# --- editing (all undoable, recorded as edit ops)
CALLS = []


def edit(a, cmd):
    if a.get("dry_run"):
        # try it, measure, restore: {"dry_run": true, "measure": {camera keys, "rect"?}} -> status + scalp coverage after
        cmd = {**cmd, "dry_run": True}
        m = a.get("measure")
        if isinstance(m, list):   # accept [shot] / shot / "top" / {camera keys}; a list measures its first camera
            m = m[0] if m else None
        if isinstance(m, str) and m.isdigit():
            m = int(m)
        if isinstance(m, int):
            m = {"shot": m}
        if isinstance(m, str):
            m = {"view": m}
        if m:
            cmd["measure"] = m
        return S.call(cmd)
    try:   # remember where this tool call started, so studio_undo {"calls": n} can undo whole calls
        CALLS.append(S.call({"do": "status"})["status"]["ops"])
    except Exception:
        pass
    r = S.call(cmd)
    if a.get("show"):
        png, _ = render(a, "after")
        return [r, image_content(png, a.get("crop"), a.get("max_px", 1000))]
    return r


SHOW = {"show": {"type": "boolean", "description": "also render after the edit (with the camera keys) and return the image"},
        "dry_run": {"type": "boolean", "description": "apply, measure, then restore: nothing is kept (predict the effect)"},
        "measure": {"type": "object", "description": "with dry_run: camera keys (shot / eye+target+fovy, optional rect "
                                                     "[x0,y0,x1,y1] px) for a scalp_coverage after the edit"},
        **CAM, **IMG}


@tool("studio_select", "Set the selection (used by tools that take where='selection').", obj({"where": WHERE}, ["where"]))
def t_select(a):
    return S.call({"do": "select", "where": a["where"]})


@tool("studio_delete", "Delete guides.", obj({"where": WHERE, **SHOW}, ["where"]))
def t_delete(a):
    return edit(a, {"do": "delete", "where": a["where"]})


@tool("studio_duplicate", "Duplicate guides with an offset of step_mm (fills thin areas next to them).",
      obj({"where": WHERE, "step_mm": {"type": "number"}, **SHOW}, ["where"]))
def t_dup(a):
    return edit(a, {"do": "duplicate", "where": a["where"], "step_mm": a.get("step_mm", 6)})


@tool("studio_densify", "Add guides on growth faces: one copy of the nearest guide at each face centroid (or n per face). "
      "Use on bald / sparse scalp areas found with studio_pick_region / studio_faces.",
      obj({"faces": {"type": "array", "items": {"type": "integer"}}, "where": WHERE, "per_face": {"type": "integer"},
           "min_area_mm2": {"type": "number"}, **SHOW}))
def t_densify(a):
    return edit(a, {"do": "densify", **{k: a[k] for k in ("faces", "where", "per_face", "min_area_mm2") if k in a}})


@tool("studio_shorten", "Shorten guides to keep (0..1) of their length.",
      obj({"where": WHERE, "keep": {"type": "number"}, **SHOW}, ["where"]))
def t_short(a):
    return edit(a, {"do": "shorten", "where": a["where"], "keep": a.get("keep", 0.8)})


@tool("studio_move", "Translate guides by offset [dx,dy,dz] m (+X = character's left, +Y = forward, +Z = up). "
      "falloff: carry (whole guide), linear / tip (more at the tip = comb), from = first moving point.",
      obj({"where": WHERE, "offset": {"type": "array", "items": {"type": "number"}},
           "falloff": {"type": "string", "enum": ["carry", "linear", "tip"]}, "from": {"type": "integer"}, **SHOW},
          ["where", "offset"]))
def t_move(a):
    return edit(a, {"do": "apply", "type": "translate", "where": a["where"], "offset": a["offset"],
                    "falloff": a.get("falloff", "carry"), "from": a.get("from", 0)})


@tool("studio_move_root_to_scalp", "Move each guide so its root sits on the scalp surface at a target point or height "
      "(the whole guide is carried). Fixes roots inside the forehead / floating roots.",
      obj({"where": WHERE, "z": {"type": "number", "description": "target root height relative to the head top (m)"},
           "point": {"type": "array", "items": {"type": "number"}}, **SHOW}, ["where"]))
def t_root(a):
    return edit(a, {"do": "root_to_scalp", **{k: a[k] for k in ("where", "z", "point") if k in a}})


@tool("studio_set_point", "Set control point `index` of one guide to pos [x,y,z] (carry: move the rest of the guide too).",
      obj({"guide": {"type": "integer"}, "index": {"type": "integer"}, "pos": {"type": "array", "items": {"type": "number"}},
           "carry": {"type": "boolean"}, **SHOW}, ["guide", "index", "pos"]))
def t_setpt(a):
    return edit(a, {"do": "apply", "type": "set_point", "where": {"indices": [a["guide"]]}, "index": a["index"],
                    "pos": a["pos"], "carry": a.get("carry", True)})


@tool("studio_reshape", "Replace all control points of one guide (points: K x [x,y,z], root first).",
      obj({"guide": {"type": "integer"}, "points": {"type": "array"}, **SHOW}, ["guide", "points"]))
def t_reshape(a):
    return edit(a, {"do": "apply", "type": "reshape", "where": {"indices": [a["guide"]]}, "points": a["points"]})


@tool("studio_curl", "Curl / wave guides. params: mode ringlet | roll | wave, start (0..1 along the guide), radius (m), "
      "turns, ramp, hand, jitter, shrink, side, band, zigzag. Under 6 control points per turn reads jagged.",
      obj({"where": WHERE, "params": {"type": "object"}, **SHOW}, ["where", "params"]))
def t_curl(a):
    return edit(a, {"do": "curl", "where": a["where"], "params": a["params"]})


@tool("studio_mirror", "Mirror one side of the groom onto the other (side: 'left' copies the character's left to the right).",
      obj({"side": {"type": "string", "enum": ["left", "right"]}, **SHOW}, ["side"]))
def t_mirror(a):
    return edit(a, {"do": "ops", "ops": [{"type": "mirror", "from": a["side"]}]})


@tool("studio_ops", "Apply raw edit ops (Studio edits format: duplicate / curl / reshape / translate / set_point / shorten / "
      "delete / mirror, roots matched at 4 mm) inline or from a file.",
      obj({"ops": {"type": "array"}, "file": {"type": "string"}, **SHOW}))
def t_ops(a):
    return edit(a, {"do": "ops", **{k: a[k] for k in ("ops", "file") if k in a}})


@tool("studio_hairline_check", "Hairline / parting / frizz quality. Also lists 'lifted' guides (root part standing "
      "off the scalp = frizz; fix with studio_lay_down, not delete) and guides_over_face (hair in front of the eyes / "
      "face; every status reports it too). Lists roots that sit on skin OUTSIDE the painted scalp "
      "(head texture, e.g. inside the forehead) with their distance to the hairline, and flow outliers: guides whose root "
      "direction deviates more than max_angle_deg (60) from their 8 nearest neighbours (frizz, strands crossing a "
      "parting). With a camera it returns a marked render: magenta = painted hairline, red rings = roots off the scalp, "
      "yellow rings = flow outliers. Goal: both counts at 0 in the area you work on.",
      obj({"where": WHERE, "max_angle_deg": {"type": "number"}, **CAM, **IMG,
           "parting": {"type": "array", "description": "[[x,y,z],[x,y,z]] parting line: guides within parting_band_mm "
                                                       "(8) are not flow outliers (the sides flow apart on purpose)"},
           "parting_band_mm": {"type": "number"},
           "lift_deg": {"type": "number", "description": "root part steeper than this off the scalp = lifted / frizz (35)"},
           "lift_max_length": {"type": "number", "description": "only guides shorter than this count as lifted (0.25 m)"},
           "overlay": {"type": "string", "enum": ["none", "scalp"]}}))
def t_hlcheck(a):
    cmd = {"do": "hairline_check", "where": a.get("where", {"all": True}), "max_angle_deg": a.get("max_angle_deg", 60),
           **{k: a[k] for k in ("parting", "parting_band_mm", "lift_deg", "lift_max_length") if k in a}}
    if any(k in a for k in ("shot", "view", "eye")):
        cmd.update(cam_args(a), path=S.png("hairline"), size=a.get("size", 1400), overlay=a.get("overlay", "none"))
    r = S.call(cmd)
    out = [r]
    if r.get("png"):
        out.append(image_content(r["png"], a.get("crop"), a.get("max_px", 1000)))
    return out


@tool("studio_align_to_hairline", "Move roots that sit on skin outside the painted scalp (farther than tolerance_mm, 8) "
      "onto the painted hairline, inset_mm (2) inside it. Only the root part moves (fade_mm, 40): the strand keeps its "
      "fall; carry: true moves the whole guide. force: true moves every selected root (line up an edge). The painted "
      "hairline can sit higher than the vanilla visible hairline: check the result against vanilla.",
      obj({"where": WHERE, "inset_mm": {"type": "number"}, "tolerance_mm": {"type": "number"}, "fade_mm": {"type": "number"},
           "carry": {"type": "boolean"}, "force": {"type": "boolean"}, **SHOW}))
def t_align(a):
    return edit(a, {"do": "align_to_hairline", "where": a.get("where", {"all": True}),
                    **{k: a[k] for k in ("inset_mm", "tolerance_mm", "fade_mm", "carry", "force") if k in a}})


@tool("studio_flow_comb", "Comb the ROOT PART of guides around the scalp normal toward a flow: away_from "
      "[[x,y,z],[x,y,z]] = a parting line in the edit frame (each side combs away from it: clean split), direction "
      "[x,y,z], or by default the mean direction of the 8 nearest guides (smooths frizz / strays). Only the first "
      "span_mm (30) of each guide turns; the rest keeps its direction, so long hair does not swing. strength 0..1, "
      "max_turn_deg (90), min_angle_deg = only guides off by more than this.",
      obj({"where": WHERE, "away_from": {"type": "array"}, "direction": {"type": "array", "items": {"type": "number"}},
           "strength": {"type": "number"}, "span_mm": {"type": "number"}, "max_turn_deg": {"type": "number"},
           "min_angle_deg": {"type": "number"}, **SHOW}, ["where"]))
def t_flow(a):
    return edit(a, {"do": "flow_comb", **{k: a[k] for k in ("where", "away_from", "direction", "strength", "span_mm",
                                                           "max_turn_deg", "min_angle_deg") if k in a}})


@tool("studio_push_out", "Fix guides that pass INSIDE the head (their hair comes out through the skin as dark lines, "
      "e.g. on the forehead): points under the skin are pushed out to standoff_mm (3) above it. list_only to count. "
      "fill_scalp / plant_parting reject templates that would pass through the head. delete: true removes the "
      "guides instead (recommended for planted copies: pushed out they often hang as stray hair; refill with "
      "studio_fill_scalp). Only points clearly inside (> depth_mm 8 under the skull surface) count. Also reports "
      "spans_under_skin / span_guides (deepest first, span_list 60): guides whose straight span BETWEEN two CVs dips > span_mm (2) under the skin (long root "
      "segments across the forehead; drawn as broken specks). Those are not pushed unless fix_spans: true (opt-in 2026-10-17): "
      "each such guide is sampled every 1 mm, samples under the RENDERED head skin are raised along the head radial to "
      "span_standoff_mm (= standoff_mm) above it, smoothed and refit to its CV count (root exact, length kept), repeated "
      "up to span_iterations (8) until no span dips > span_mm; reports spans_fixed / spans_under_skin_after / span_guides_after.",
      obj({"where": WHERE, "standoff_mm": {"type": "number"}, "depth_mm": {"type": "number"}, "delete": {"type": "boolean"},
           "list_only": {"type": "boolean"}, "span_mm": {"type": "number"}, "span_list": {"type": "integer"},
           "fix_spans": {"type": "boolean"}, "span_standoff_mm": {"type": "number"}, "span_iterations": {"type": "integer"}, **SHOW}))
def t_push(a):
    return edit(a, {"do": "push_out", "where": a.get("where", {"all": True}),
                    **{k: a[k] for k in ("standoff_mm", "depth_mm", "delete", "list_only", "span_mm", "span_list", "fix_spans", "span_standoff_mm", "span_iterations", "debug_index") if k in a}})


@tool("studio_lengthen", "Make guides longer (factor > 1, e.g. 1.3) or shorter (< 1) keeping their shape: CVs re-placed along "
      "the smooth curve of the strand (resample to 16+ CVs first, studio_resample); past the old tip it continues along the last direction, bending down with droop (0.5). Fixes a "
      "too-short cut / bob vs vanilla (Margarita). Guides stay outside the head. Check the budget (longer = more hair "
      "volume) and studio_compare.",
      obj({"where": WHERE, "factor": {"type": "number"}, "droop": {"type": "number"}, **SHOW}, ["where"]))
def t_lengthen(a):
    return edit(a, {"do": "lengthen", **{k: a[k] for k in ("where", "factor", "droop") if k in a}})


@tool("studio_resample", "Give EVERY guide n control points (3..32; 16-24 for waves / curls / long hair): CVs re-placed evenly "
      "along the smooth (centripetal Catmull-Rom) curve through the current CVs, root and tip exact, so the shape does not "
      "change (reports max_dev_mm / mean_dev_mm). One CV count for all guides (no where). Do it BEFORE studio_curl (6+ CVs per "
      "turn) and studio_lengthen. Studio-only op (exports write the guides shown, any CV count; the game has run 25-CV "
      "guides). Render cost grows with the CVs: (n-1) x splineMultiplier + 1 vertices per hair.",
      obj({"n": {"type": "integer"}, **SHOW}, ["n"]))
def t_resample(a):
    return edit(a, {"do": "resample", "n": a["n"]})


@tool("studio_clear_ear", "Tuck hair behind the ears (vanilla shows the ear, e.g. Eredin / Crach): finds the ears on the "
      "head mesh (box), presses guide points within radius_mm (30) of the ear in against the skull on the ear's inner "
      "side (tuck_mm 12), so hair runs between ear and skull; back_mm (0) also pulls points in front of the ear back. "
      "remove_faces: true also removes hair growing ON the ear. side both|left|right. Reports the ear centres. Then studio_compare a "
      "side shot; repeat with a bigger radius / back if hair still covers the ear.",
      obj({"where": WHERE, "side": {"type": "string"}, "radius_mm": {"type": "number"}, "back_mm": {"type": "number"}, "tuck_mm": {"type": "number"},
           "remove_faces": {"type": "boolean", "description": "also remove the growth faces rooted ON the ear (box + margin_mm 4): the growth mesh often covers the ear itself (Eredin) - the strongest fix"}, "margin_mm": {"type": "number"}, "delete_near_mm": {"type": "number", "description": "delete (not move) guides rooted within this distance of the ear whose path crosses it, e.g. 25 - for hair growing right at the ear (Eredin); check the temple after"}, **SHOW}))
def t_clear_ear(a):
    return edit(a, {"do": "clear_ear", **{k: a[k] for k in ("where", "side", "radius_mm", "back_mm", "tuck_mm", "delete_near_mm", "remove_faces", "margin_mm") if k in a}})


@tool("studio_smooth_flow", "Declump / smooth: each guide's shape is blended with the mean shape of guides rooted within "
      "radius_mm (12); strength 0..1 (0.6), passes (2). Neighbours flow together like vanilla's smooth card surface - "
      "use after studio_conform_to_cards when the result looks clumpy / streaky / crossing. Bigger radius = smoother "
      "helmet; too big loses partings (limit with where).",
      obj({"where": WHERE, "radius_mm": {"type": "number"}, "strength": {"type": "number"}, "passes": {"type": "integer"}, **SHOW}))
def t_smooth(a):
    return edit(a, {"do": "smooth_flow", **{k: a[k] for k in ("where", "radius_mm", "strength", "passes") if k in a}})


@tool("studio_conform_to_cards", "DRASTIC reshape toward vanilla: pulls every guide (past root_part, default 0.15 of "
      "its length) onto the surface of the game's own hair cards, offset_mm (2) outward; strength 0..1 (0.8), points "
      "already within tol_mm (4) stay. Segment lengths kept, stays outside the head. Use it when the SHAPE is wrong "
      "(waves / flips behind the ears, crests, puffy volume, wrong parting side) - limit with 'where' or run on all, "
      "then studio_compare. Guides longer than the cards keep their extra length hanging past them: follow with "
      "studio_lengthen factor < 1 if vanilla is shorter.",
      obj({"where": WHERE, "strength": {"type": "number"}, "offset_mm": {"type": "number"}, "tol_mm": {"type": "number"},
           "root_part": {"type": "number"}, "mode": {"type": "string", "description": "surface (default) | centreline: curls / rolls / waves - moves only the smoothed centre line onto the cards and keeps each strand's curl"}, "inner_lift": {"type": "number", "description": "0..1 (0.15): how much points lying INSIDE the card shell are lifted out to it (low keeps crown hair on the scalp)"}, **SHOW}))
def t_conform(a):
    return edit(a, {"do": "conform_to_cards", **{k: a[k] for k in ("where", "strength", "offset_mm", "tol_mm", "root_part", "inner_lift", "mode") if k in a}})


@tool("studio_hug_cards", "DRASTIC flattening toward vanilla's outline: the vanilla card shell seen from the head centre "
      "(outer radius per direction, shell_pct 0.9 of the visible card hair samples in a cone_deg 4 cone) is a hard limit - every "
      "guide point past root_part (0.1) is clamped to at most max_mm (2) outside it along its head radial; the last "
      "tip_part (0.12) may stand tip_mm (6) more out (vanilla's tip flick). Lengths kept (constrained re-walk: spare "
      "length slides down along the shell). Points inside the shell are not moved. Use it when hair billows / puffs "
      "out where vanilla lies flat (behind the ears, down the neck) and conform_to_cards leaves it (its tol_mm skips "
      "points near ANY card layer). Shell = alpha-VISIBLE card hair (alpha_vis 0.3; card quads reach far past the painted "
      "hair). Points in directions with no vanilla hair are pulled to within max_mm + footprint_mm (4) of the nearest "
      "visible card hair (footprint false = off). inside_mm (0 = off, e.g. 10): points below head centre + inside_z_mm "
      "(0) that lie INSIDE the shell but farther than inside_mm from any visible card hair (hair on the neck behind the "
      "ear where vanilla shows skin) are pulled to it too. list_only reports the excess before / after without applying.",
      obj({"where": WHERE, "max_mm": {"type": "number"}, "tip_mm": {"type": "number"}, "tip_part": {"type": "number"},
           "root_part": {"type": "number"}, "shell_pct": {"type": "number"}, "cone_deg": {"type": "number"},
           "strength": {"type": "number"}, "alpha_vis": {"type": "number"}, "footprint": {"type": "boolean"},
           "footprint_mm": {"type": "number"}, "inside_mm": {"type": "number"}, "inside_z_mm": {"type": "number"},
           "list_only": {"type": "boolean"}, **SHOW}))
def t_hug_cards(a):
    return edit(a, {"do": "hug_cards", **{k: a[k] for k in ("where", "max_mm", "tip_mm", "tip_part", "root_part", "shell_pct",
                                                           "cone_deg", "strength", "alpha_vis", "footprint", "footprint_mm",
                                                           "inside_mm", "inside_z_mm", "list_only") if k in a}})


CSHELL_KEYS = ("where", "protect", "surface", "cards_mix", "outer_mm", "layers", "assign", "rank_mm", "shell_pct", "cone_deg",
               "smooth", "lump_mm", "offset_mm", "inner_mm", "thick_mm", "root_mm", "blend_mm", "flow", "flow_source",
               "flow_cards_mix", "flow_smooth", "parting_x", "parting_band_mm", "strength", "tip_free", "exclude_fringe",
               "fringe_back_mm", "fringe_up_mm", "alpha_vis", "node_deg", "max_slope", "capsule_mm", "debug_guide", "fringe_side_mm", "root_lift", "fringe", "fringe_fade_mm", "flow_mode", "flow_radius_mm", "scalp_pct", "trim_hem", "trim_mm", "fringe_carry_mm", "fringe_carry_max_mm", "clamp_mm", "list_only")


@tool("studio_conform_shell", "Even, rounded layering (2026-10-16): turns uneven guides (lumps, strands wandering in and out in "
      "depth) into concentric shells. Outer surface seen from the head centre = surface 'guides' (default: shell_pct 0.9 "
      "envelope of the current guides, lumps standing more than lump_mm (3) above the smoothed envelope cut, then smooth (8) "
      "passes) | 'cards' (visible vanilla card hair) | 'blend' (cards_mix 0.5), or outer_mm = constant height over the scalp; "
      "offset_mm moves it out/in. layers (4) shells from the outer surface down to inner_mm (3) over the scalp, at most thick_mm "
      "(14) deep. assign 'even' (default: each guide ranked by its current depth among the guides rooted within rank_mm 25, so "
      "every shell is evenly populated) | 'depth' (nearest shell to its current depth). After root_mm (10) each guide blends "
      "onto its shell over blend_mm (25) and stays at constant depth; its tangential direction follows a smooth flow field "
      "(flow 0.8; flow_source guides | cards | blend with flow_cards_mix; flow_smooth 6 passes); parting_x (m from head centre) "
      "= a parting plane the flow is never averaged across, flow leads away from it within parting_band_mm (30). tip_free "
      "(0) = last share of the length that may leave the shell. Root + segment lengths kept, pushOut. exclude_fringe (true): "
      "guides with points in front of the forehead below brow + fringe_up_mm (30) are left alone; protect = selector kept "
      "unchanged; strength 0..1 blend. One undo step (plain reshape ops, saved + exported). Reports depth_wander_mm_before/"
      "after (mean per-guide std of the depth below the outer surface), layer_population. list_only = report only.",
      obj({"where": WHERE, "protect": WHERE, "surface": {"type": "string", "enum": ["guides", "cards", "blend"]},
           "cards_mix": {"type": "number"}, "outer_mm": {"type": "number"}, "layers": {"type": "integer"},
           "assign": {"type": "string", "enum": ["even", "depth"]}, "rank_mm": {"type": "number"}, "shell_pct": {"type": "number"},
           "cone_deg": {"type": "number"}, "smooth": {"type": "integer"}, "lump_mm": {"type": "number"}, "offset_mm": {"type": "number"},
           "inner_mm": {"type": "number"}, "thick_mm": {"type": "number"}, "root_mm": {"type": "number"}, "blend_mm": {"type": "number"},
           "flow": {"type": "number"}, "flow_source": {"type": "string", "enum": ["neighbours", "guides", "cards", "blend"]},
           "flow_radius_mm": {"type": "number"},
           "flow_cards_mix": {"type": "number"}, "flow_smooth": {"type": "integer"}, "parting_x": {"type": "number"},
           "parting_band_mm": {"type": "number"}, "strength": {"type": "number"}, "tip_free": {"type": "number"},
           "exclude_fringe": {"type": "boolean"}, "fringe_back_mm": {"type": "number"}, "fringe_up_mm": {"type": "number"},
           "alpha_vis": {"type": "number"}, "node_deg": {"type": "number"}, "max_slope": {"type": "number"},
           "capsule_mm": {"type": "number"}, "fringe_side_mm": {"type": "number"}, "root_lift": {"type": "number"}, "fringe": {"type": "string", "enum": ["upper", "skip"]}, "fringe_fade_mm": {"type": "number"}, "trim_hem": {"type": "boolean", "description": "shorten strands that run past the vanilla outline below the head (collar clipping) to end trim_mm (5) past it"}, "trim_mm": {"type": "number"}, "flow_mode": {"type": "string", "enum": ["align", "follow"]}, "debug_guide": {"type": "integer"}, "list_only": {"type": "boolean"}, **SHOW}))
def t_conform_shell(a):
    return edit(a, {"do": "conform_shell", **{k: a[k] for k in CSHELL_KEYS if k in a}})


@tool("studio_crown_swirl", "Comb the crown into one coherent flow (2026-10-16): guides rooted within radius_mm (50) of the crown "
      "point (crown [x,y,z], default on the skull crown_back_deg 30 behind the top) keep their root and leave it in the swirl "
      "direction (away from the crown, turned spiral_deg 20), lying lift_mm (4) over the skull (max_slope 0.4) for swirl_mm "
      "(35), then rejoin their old path over rejoin_mm (35). Fringe / face-side guides (tip in front of the eye plane minus "
      "face_back_mm 30, or within eye_mm 30 of the eyes) and protect are left alone. strength 0..1. One undo step. Follow "
      "with studio_conform_shell for the layers. list_only reports.",
      obj({"where": WHERE, "protect": WHERE, "crown": {"type": "array", "items": {"type": "number"}}, "crown_back_deg": {"type": "number"},
           "radius_mm": {"type": "number"}, "swirl_mm": {"type": "number"}, "rejoin_mm": {"type": "number"}, "spiral_deg": {"type": "number"},
           "lift_mm": {"type": "number"}, "max_slope": {"type": "number"}, "face_back_mm": {"type": "number"}, "eye_mm": {"type": "number"},
           "strength": {"type": "number"}, "mode": {"type": "string", "enum": ["swirl", "untwist"], "description": "untwist: each crown guide's shape = gaussian mean of the crown guides rooted within nb_mm (18), passes (3): no crossing / twisting ropes"},
           "nb_mm": {"type": "number"}, "passes": {"type": "integer"}, "list_only": {"type": "boolean"}, **SHOW}))
def t_crown_swirl(a):
    return edit(a, {"do": "crown_swirl", **{k: a[k] for k in ("where", "protect", "crown", "crown_back_deg", "radius_mm", "swirl_mm",
                                                             "rejoin_mm", "spiral_deg", "lift_mm", "max_slope", "face_back_mm",
                                                             "eye_mm", "strength", "mode", "nb_mm", "passes", "list_only") if k in a}})


@tool("studio_check_direction", "MANDATORY before any save (2026-10-16 user rule: never strands going the wrong way). Checks every "
      "guide's tip part (tip_part 0.5; curly guides turning > curl_turn_deg 300 there are skipped): 'up' = hanging hair "
      "(below head centre + up_above_mm 20) rising (dz > up_thr 0.35) on >= 1/3 of the tip segments; 'against' = heading "
      "against the guides rooted within nb_mm (20) (dot < against_thr -0.2) on most tip segments. wind (default true): the "
      "simulated guides under wind (wind_yaw_deg = heading, clockwise from +y = face: 15 = wind from behind, 195 from the "
      "front, 105 / 285 from the sides; wind_strength 5 = toolbar max, wind_s 3) rising (dz > wind_up_thr 0.3 and turned up "
      "by > wind_turn 0.5 vs rest) in >= 1/3 of the samples. Returns rest_offenders (rest_up / rest_against) / "
      "wind_offenders + lists. fix: true re-combs the offenders (mean shape of non-offending neighbours within fix_nb_mm 25 "
      "from fix_from 0.25 of the length, gravity floor min_drop 0.2 on hanging tip segments; fix_wind false = rest "
      "offenders only; protect = selector kept) in one undo step. Run it for all four wind_yaw_deg. The serve guard runs "
      "the rest test around every tool that changes guides (studio_direction_guard).",
      obj({"tip_part": {"type": "number"}, "up_thr": {"type": "number"}, "up_above_mm": {"type": "number"}, "against_thr": {"type": "number"},
           "nb_mm": {"type": "number"}, "curl_turn_deg": {"type": "number"}, "wind": {"type": "boolean"}, "wind_yaw_deg": {"type": "number"},
           "wind_pitch_deg": {"type": "number"}, "wind_strength": {"type": "number"}, "wind_s": {"type": "number"},
           "wind_up_thr": {"type": "number"}, "wind_turn": {"type": "number"}, "fix": {"type": "boolean"}, "fix_wind": {"type": "boolean"},
           "fix_nb_mm": {"type": "number"}, "fix_from": {"type": "number"}, "min_drop": {"type": "number"}, "protect": WHERE}))
def t_check_direction(a):
    try:
        CALLS.append(S.call({"do": "status"})["status"]["ops"])
    except Exception:
        pass
    return S.call({"do": "check_direction", **a}, timeout=900)


@tool("studio_regrid", "Opt-in, Studio-only (2026-10-16): RE-GRID a region from a smooth flow field, the fix for crown ropes / "
      "locks / knots (measured on Shani: groups of neighbouring guides bundling and diverging, faces of 0.9-49 mm2 that "
      "connect guides going different ways). Deletes the guides in where (default {z_min:-0.06} = the crown; minus "
      "protect, fringe / face-side guides, guides shorter than min_len 0.03) and rebuilds: Poisson roots on the painted "
      "scalp within reach_mm (10) of the old roots at spacing_mm (default: keeps the region's guide count x count_scale 1), "
      "sunk to the groom's root depth; each new guide = Gaussian mean (flow_mm 30) of the OLD guides' shapes and lengths "
      "(neighbours whose root direction opposes the nearest guide's do not vote: partings keep both combs; length_scale 1); "
      "faces = Delaunay stitched to the kept border, no face over a kept face, no face whose guides go opposite ways "
      "(oppose_dot -0.2). One undoable rebuild op (saved, exported through studio_geo). Reports region_guides, "
      "new_guides / new_faces, spacing_mm, root_depth_mm, rejected counts. list_only plans only. Then run "
      "studio_check_faces + studio_check_direction and look from the top / 3-4 above / sides / wind.",
      obj({"where": WHERE, "protect": WHERE, "spacing_mm": {"type": "number"}, "count_scale": {"type": "number"}, "flow_mm": {"type": "number"},
           "reach_mm": {"type": "number"}, "min_len": {"type": "number"}, "length_scale": {"type": "number"}, "exclude_face": {"type": "boolean"},
           "face_back_mm": {"type": "number"}, "eye_mm": {"type": "number"}, "oppose_dot": {"type": "number"}, "edge_mm": {"type": "number"},
           "any_skin": {"type": "boolean"}, "list_only": {"type": "boolean"},
           "dir_frac": {"type": "number", "description": "arc fraction where guide directions are compared for the vote gate / face opposition (0.3)"},
           "gate_iters": {"type": "integer", "description": "consensus rounds dropping voters heading against the weighted mean direction (2; 0 = every neighbour votes)"},
           "hug": {"type": "number", "description": "0..1: re-project each integrated point to the voters mean height over the skull (1; 0 = free tangent integral, leaves the skull)"},
           "smooth_passes": {"type": "integer", "description": "3-point smoothing passes on each new strand after the projection (2)"},
           "keep_cross": {"type": "boolean", "description": "keep the region's guides that head against the local consensus (the crossing layer of a layered style) instead of regridding them (false)"},
           "cross_dot": {"type": "number", "description": "keep_cross threshold: dot(guide dir, consensus) below this = crossing (0)"},
           "mode": {"type": "string", "enum": ["field", "arc"], "description": "field (default): trace each strand through the spatial flow field of the voters' segments; arc: per-arc-fraction mean tangent"},
           **SHOW}))
def t_regrid(a):
    return edit(a, {"do": "regrid", **{k: a[k] for k in ("where", "protect", "spacing_mm", "count_scale", "flow_mm", "reach_mm", "min_len",
                                                         "length_scale", "exclude_face", "face_back_mm", "eye_mm", "oppose_dot", "edge_mm",
                                                         "any_skin", "list_only", "dir_frac", "gate_iters", "hug", "smooth_passes", "mode", "keep_cross", "cross_dot") if k in a}})


@tool("studio_skin_guard", "Hard geometric floor (2026-10-16): moves every control point of the authored rest shape that lies "
      "closer than margin_mm (3) to the posed skin (head, face, ears, neck: the check_faces skin field) out along the field "
      "gradient to margin_mm; roots and the first root_mm (6) of arc length are kept. passes (3). where = guide selector. "
      "One undo group of plain reshape ops. list_only reports guides_moved / points_moved / deepest_mm / list without moving. "
      "Run it before colliders / stiffness work: the SDK's soft collision cannot lift hair that is authored inside the skin.",
      obj({"margin_mm": {"type": "number"}, "root_mm": {"type": "number"}, "passes": {"type": "integer"}, "where": WHERE,
           "list_only": {"type": "boolean"}, "cell_mm": {"type": "number"}, "reach_mm": {"type": "number"}}))
def t_skin_guard(a):
    return S.call({"do": "skin_guard", **a}, timeout=900)


@tool("studio_check_faces", "Growth-face validator (2026-10-16, run before every save next to studio_check_direction). HairWorks draws "
      "the hair of a face as blends of its 3 guides and collides only the GUIDES, so a face whose guides pass on different "
      "sides of the head / face / neck draws hair THROUGH the skin while every guide is clean. Measures the interpolated "
      "hair itself (samples 4 = 12 blends per face) against a signed-distance field of the posed head mesh, on the "
      "SIMULATED guides: settled rest, then wind (fresh simulation per heading in wind_yaw_deg, default [15,105,195,285] = behind, "
      "side, front, side; wind_strength 1 = the toolbar default the user sees, times the look windScaler; 5 = toolbar max, a stress test that overpowers the SDK collision; wind_s 2.5 s). penetration: rest / wind counts, interp_only (only the "
      "in-between hair is inside, deeper than depth_mm 3), guides_too, list (face id, guides, centroid, depths, heading). "
      "guides_inside: guides themselves inside the skin (collider problem). sliver: faces under sliver_mm2 (3; HairWorks "
      "grows the same hair count on every face, so slivers render as dense bundles). big: faces over big_x (4) x the median "
      "area (sparse hair, scalp speckle). divergent: mid spread > spread (4) x root spread or guide directions > div_deg (60) "
      "apart; twisted = winding flips. 2026-10-16: every penetration row carries 'at' (where the deepest sample is) and 'skin' "
      "(true = nearest to a NON-scalp vertex: face / ears / neck, visible; false = under the painted scalp: invisible hair, the "
      "bald-patch mechanism), counts skin_rest / skin_wind / skin_interp_only; fix_what ['skin'] deletes only the interp-only "
      "faces through the skin (studio_fix_faces_skin wraps it with a refill). fix: true deletes the faces in fix_what (default ['penetration'] = interp-only "
      "penetrations; also 'guides', 'sliver', 'big', 'divergent') with delete_face ops in one undo group (protect = "
      "selector whose faces are kept); check scalp coverage afterwards. where limits the faces to those touching the "
      "selected guides. ~20-60 s with wind on.",
      obj({"where": WHERE, "protect": WHERE, "depth_mm": {"type": "number"}, "samples": {"type": "integer"}, "sliver_mm2": {"type": "number"},
           "big_x": {"type": "number"}, "spread": {"type": "number"}, "div_deg": {"type": "number"}, "wind": {"type": "boolean"},
           "wind_yaw_deg": {"type": "array", "items": {"type": "number"}}, "wind_pitch_deg": {"type": "number"}, "wind_strength": {"type": "number"},
           "wind_s": {"type": "number"}, "cell_mm": {"type": "number"}, "reach_mm": {"type": "number"}, "fix": {"type": "boolean"},
           "fix_what": {"type": "array", "items": {"type": "string"}}, "list_only": {"type": "boolean"}}))
def t_check_faces(a):
    try:
        CALLS.append(S.call({"do": "status"})["status"]["ops"])
    except Exception:
        pass
    return S.call({"do": "check_faces", **a}, timeout=1800)


@tool("studio_direction_guard", "Direction guard around every guide-changing tool (not bun / braid / curl / twist / fur): "
      "guides the command made point the wrong way at rest (pre-existing ones are only counted) are, per mode, re-combed in "
      "the same undo step (fix, default) | the command is undone with an error (refuse) | reported (report) | off. The mode "
      "holds for every later command of the session; replies carry a direction_guard summary.",
      obj({"mode": {"type": "string", "enum": ["fix", "refuse", "report", "off"]}}))
def t_direction_guard(a):
    return S.call({"do": "direction_guard", **a})


@tool("studio_rebuild_from_cards", "DRASTIC, opt-in: delete ALL guides in 'where' (default all) and trace new ones from "
      "the game's own hair cards (growth.py idea: lanes along each card's root->tip texture direction, trimmed where the "
      "card alpha has no hair; roots = Poisson points on the scalp under visible card hair, spacing_mm apart, a "
      "target count, or by default spaced so the hair budget matches the build as opened (budget_target 1.0); each root follows a card lane passing near it; stacked duplicate cards dropped; strands through "
      "the head / over the eyes rejected; new growth faces = Delaunay of the roots). For builds that look nothing like "
      "vanilla (too few / wrong guides). One undoable op ('rebuild', Studio-only: the HairLab pipeline ignores it). "
      "The budget is information only (never raises the spacing). replace false = add "
      "guides between the existing ones. hug 0..1 (0.35 as growth.py, 1 = keep the card layer's distance off the skull); "
      "offset_keep (0.3) = how much of the root's offset from the lane stays at the tip (0 = neighbours converge on "
      "one card tip). Check studio_compare after; follow with conform / smooth / look tools as needed.",
      obj({"where": WHERE, "spacing_mm": {"type": "number"}, "count": {"type": "integer"}, "replace": {"type": "boolean"},
           "hug": {"type": "number"}, "offset_keep": {"type": "number"}, "pick": {"type": "string", "description": "balanced (default: random lane among those near the root, weighted by length = layered ends) | longest"},
           "lane_w_mm": {"type": "number"}, "reach_mm": {"type": "number"}, "near_mm": {"type": "number"},
           "min_length_mm": {"type": "number"}, "alpha_tip": {"type": "number"}, "alpha_vis": {"type": "number"},
           "dedupe_mm": {"type": "number"}, "zmin_rel": {"type": "number"}, "edge_mm": {"type": "number"}, "budget_target": {"type": "number"},
           "root_bias_mm": {"type": "number", "description": "prefer card lanes that START near the root (inner layers) over lanes passing over it: weight exp(-length before the root / root_bias_mm); 0 = off"},
           "copy_mm": {"type": "number", "description": "roots with no card lane nearby copy the nearest traced guide within copy_mm (25)"},
           "scalp_only": {"type": "boolean", "description": "roots only on the painted scalp (default true)"},
           "uv_mode": {"type": "string", "description": "uniform (default: builds with a density map get one flat uv at the mean density + median colour for all new guides, no patchy density) | nearest (uv of the closest old growth face)"},
           "density": {"type": "number", "description": "uniform uv_mode: density value to pick (default the build's mean face density)"},
           "eye_mm": {"type": "number", "description": "strands passing within eye_mm (10) of an eyeball are rejected"},
           "allow_face": {"type": "boolean", "description": "keep strands near the eyes"}, "ignore_budget": {"type": "boolean"},
           "layer": {"type": "string", "enum": ["near", "inner"], "description": "inner: raised / stacked cards (1-4 cm off the scalp, Fringilla) - card hair counts up to raise_mm (45) above the skin, a root is under card hair when it lies straight above it, lanes are searched above the root and the INNERMOST layer wins (exp(-height / inner_bias_mm 8)), hug band = hug_band_mm (raise_mm)"},
           "raise_mm": {"type": "number"}, "inner_bias_mm": {"type": "number"}, "hug_band_mm": {"type": "number"},
           # rebase2 (2026-10-12, opt-in)
           "trace": {"type": "string", "enum": ["lane", "flow"], "description": "flow: each strand is a streamline of the card flow field (mean root->tip texture direction of the lanes around it, opposite flows at a parting kept apart), holding a per-strand share (flow_layer_min..flow_layer_max) of the local card layer height = volume of raised / stacked cards kept, layered; neighbouring roots give parallel, non-crossing strands. The lane copy + hug are skipped."},
           "flow_radius_mm": {"type": "number"}, "flow_step_mm": {"type": "number"}, "flow_inertia": {"type": "number"},
           "flow_height": {"type": "number", "description": "flow: scale of the card layer height held (1 = card layer, 0 = on the skin)"},
           "flow_layer_min": {"type": "number"}, "flow_layer_max": {"type": "number"}, "flow_rise_mm": {"type": "number"},
           "flow_length_scale": {"type": "number"}, "flow_hold": {"type": "number"}, "flow_face_full": {"type": "boolean"}, "flow_clear_mm": {"type": "number"}, "flow_whorl": {"type": "number"}, "flow_whorl_agree": {"type": "number"}, "flow_tip_stop": {"type": "boolean"}, "flow_start": {"type": "string", "enum": ["mean", "lane"], "description": "flow start direction: mean (clustered mean of the lanes at the root: sleek, one flow per area) | lane (the balanced-picked lane: minority flows such as a fringe over the top kept)"}, "flow_start_mix": {"type": "number"}, "orient": {"type": "string", "enum": ["near", "vote"], "description": "lane root side: near (end nearer the skin) | vote (islands whose ends are within orient_margin_mm of the same skin distance - fringes on the forehead, short stacked cards - follow the majority of the confident islands of the same atlas column)"}, "orient_margin_mm": {"type": "number"},
           "density_mode": {"type": "string", "enum": ["budget", "coverage"], "description": "coverage: root spacing from the visible card coverage (alpha-weighted card area per scalp area above the root): coverage_spacing_mm (7) at coverage >= coverage_ref (1), sparser where cards are thin (down to coverage_floor), no root under coverage_min; budget_target is then only a maximum"},
           "coverage_spacing_mm": {"type": "number"}, "coverage_ref": {"type": "number"}, "coverage_min": {"type": "number"},
           "coverage_radius_mm": {"type": "number"}, "coverage_floor": {"type": "number"},
           "scalp_grow_mm": {"type": "number", "description": "roots also on skin within this distance of the painted scalp where card hair lies on the skin (grow_card_mm 8), never on the front forehead (temples, sideburns, nape)"},
           "grow_card_mm": {"type": "number"},
           "eye_mode": {"type": "string", "enum": ["reject", "cut"], "description": "cut: strands reaching an eye are cut before it (fringes kept)"},
           "exclude_ears": {"type": "boolean", "description": "no roots on the ear boxes (+ ear_pad_mm 10); strands crossing an ear are routed behind it (ear_route behind | drop | none)"},
           "ear_route": {"type": "string"}, "ear_pad_mm": {"type": "number"}, "behind_mm": {"type": "number"},
           "ear_lat_mm": {"type": "number"}, "ear_top_mm": {"type": "number"}, "ear_box": {"type": "array"},
           "ear_faces": {"type": "boolean", "description": "exclude_ears: drop growth faces whose interpolated hair crosses an ear (default true)"}, "exclude_temples_mm": {"type": "number", "description": "no roots in front of the ears below eye level + this (outside the eye span): bare temples / M hairline"},
           # rebase3 (2026-10-13, opt-in)
           "part_detect": {"type": "boolean", "description": "find the part line where the sideways card flow on the top diverges; roots up to both edges (part_band_mm 18), combed away from it (part_comb 0.7), no whorl crossing there"},
           "part_band_mm": {"type": "number"}, "part_comb": {"type": "number"}, "part_depth_mm": {"type": "number"}, "part_min_flow": {"type": "number"},
           "fringe": {"type": "boolean", "description": "front roots within fringe_reach_mm (20) of a fringe lane (tip over the forehead, fringe_drop_mm below its root, off the scalp) copy it from the hairline (held fringe_clear_mm 3 off the skin)"},
           "fringe_reach_mm": {"type": "number"}, "fringe_clear_mm": {"type": "number"}, "fringe_drop_mm": {"type": "number"},
           "alpha_density": {"type": "boolean", "description": "a root survives with probability smoothstep(alpha_lo 0.15, alpha_hi 0.6, mean card alpha above it within alpha_radius_mm 6): roots only where the card texture is opaque, ragged hairline"},
           "alpha_lo": {"type": "number"}, "alpha_hi": {"type": "number"}, "alpha_radius_mm": {"type": "number"},
           "clump_var": {"type": "number", "description": "0..1: varied clumps (clump_min_mm 4 .. clump_max_mm 14, tips pulled to the clump centre strand by clump_pull 0.6), length variation len_var 0.25, flyaways at card edges (fly_frac 0.3 of edge strands drift fly_mm 12)"},
           "clump_min_mm": {"type": "number"}, "clump_max_mm": {"type": "number"}, "clump_pull": {"type": "number"}, "len_var": {"type": "number"},
           "fly_frac": {"type": "number"}, "fly_mm": {"type": "number"},
           "nape_cut": {"type": "boolean", "description": "no roots on the back below the lowest vanilla card roots (per azimuth bin, nape_pct 0.05 percentile) minus nape_cut_mm (0)"},
           "nape_cut_mm": {"type": "number"}, "nape_pct": {"type": "number"},
           **SHOW}))
def t_rebuild(a):
    keys = ("where", "spacing_mm", "count", "replace", "hug", "offset_keep", "pick", "lane_w_mm", "reach_mm", "near_mm",
            "min_length_mm", "alpha_tip", "alpha_vis", "dedupe_mm", "zmin_rel", "edge_mm", "budget_target", "root_bias_mm", "copy_mm", "scalp_only", "uv_mode", "density", "eye_mm", "debug_lanes", "allow_face", "ignore_budget",
            "layer", "raise_mm", "inner_bias_mm", "hug_band_mm",
            "trace", "flow_radius_mm", "flow_step_mm", "flow_inertia", "flow_height", "flow_layer_min", "flow_layer_max",
            "flow_rise_mm", "flow_length_scale", "flow_hold", "flow_face_full", "flow_clear_mm", "flow_whorl", "flow_whorl_agree","flow_tip_stop", "flow_start", "flow_start_mix", "orient", "orient_margin_mm", "density_mode", "coverage_spacing_mm", "coverage_ref",
            "coverage_min", "coverage_radius_mm", "coverage_floor", "scalp_grow_mm", "grow_card_mm", "eye_mode",
            "exclude_ears", "ear_route", "ear_pad_mm", "behind_mm", "ear_lat_mm", "ear_top_mm", "ear_box", "exclude_temples_mm", "ear_faces",
            "skin_exact", "part_detect", "part_debug", "part_cross_mm", "part_extend_mm", "part_layer", "part_clear_mm", "part_gap_mm", "part_split", "part_band_mm", "part_comb", "part_depth_mm", "part_min_flow", "fringe", "fringe_reach_mm",
            "fringe_clear_mm", "fringe_drop_mm", "fringe_front_mm", "fringe_root_back_mm", "alpha_density", "alpha_lo", "alpha_hi", "alpha_radius_mm", "clump_var",
            "clump_min_mm", "clump_max_mm", "clump_pull", "len_var", "fly_frac", "fly_mm", "nape_cut", "nape_cut_mm", "nape_pct")
    return edit(a, {"do": "rebuild_from_cards", **{k: a[k] for k in keys if k in a}})


@tool("studio_parting_comb", "Opt-in (2026-10-15): fix a bare split / fan of short straight stubs at a parting. Parting roots = "
      "roots whose neighbours within detect_mm (10) start off in diverging directions (min_diverging 5). Roots within "
      "band_mm (8) of them get the first root_part (0.3) of the strand re-aimed toward ONE side - side 'auto' (default: the "
      "flow most neighbours within side_mm 18 share) or a direction [x,y,z] - lifted by 'lift' (0.15), blended smoothly into "
      "the rest (segment lengths kept). Roots further away untouched. exclude [x,y,z] + exclude_mm (30) protects a crown "
      "swirl. dry true = only count; list_core true = list detected parting roots. One undoable group.",
      obj({"where": WHERE, "detect_mm": {"type": "number"}, "band_mm": {"type": "number"}, "root_part": {"type": "number"},
           "lift": {"type": "number"}, "min_diverging": {"type": "integer"}, "side_mm": {"type": "number"},
           "strength": {"type": "number"}, "side": {"type": "array"}, "exclude": {"type": "array"},
           "exclude_mm": {"type": "number"}, "dry": {"type": "boolean"}, "list_core": {"type": "boolean"}, "plant": {"type": "boolean", "description": "v3: plant new roots across the parting band (plant_mm 3 grid within plant_band_mm 5 of each parting root, none within plant_min_mm 2.5 of a root, only between existing roots), each a copy of the nearest guide flowing the chosen way, flat on the scalp near the root (plant_flat_mm 1.5): covers bare scalp + stubs"}, "plant_mm": {"type": "number"}, "plant_band_mm": {"type": "number"}, "plant_min_mm": {"type": "number"}, "plant_flat_mm": {"type": "number"}, "plant_max": {"type": "integer", "description": "hard cap on planted guides (default no cap)"}, "faces": {"type": "string", "enum": ["drop", "keep"], "description": "drop (default, v2): remove growth faces spanning the parting (guides in the band starting off in opposite directions) so no interpolated hair spreads apart there"}, "tail": {"type": "number", "description": "share of the side direction kept past root_part (0.5) so strands do not loop back over the parting"}, **SHOW}))
def t_parting_comb(a):
    keys = ("where", "detect_mm", "band_mm", "root_part", "lift", "min_diverging", "side_mm", "strength", "side",
            "exclude", "exclude_mm", "dry", "list_core", "tail", "faces", "plant", "plant_mm", "plant_band_mm", "plant_min_mm", "plant_flat_mm", "plant_max")
    return edit(a, {"do": "parting_comb", **{k: a[k] for k in keys if k in a}})


@tool("studio_volume_lift", "Opt-in (2026-10-15): uniform volume over a region without starbursts or bumps. Guides in 'where' "
      "(weight 1) and those within feather_mm (25) outside it (smooth falloff) lift their outer part by offset_mm (6) along "
      "the head-centre radial direction; the lift ramps in from the root over root_part (0.35) so roots / partings blend; "
      "weights smoothed over smooth_mm (12), smooth_passes (2). One undoable group.",
      obj({"where": WHERE, "offset_mm": {"type": "number"}, "feather_mm": {"type": "number"}, "root_part": {"type": "number"},
           "smooth_mm": {"type": "number"}, "smooth_passes": {"type": "integer"}, "profile": {"type": "string", "enum": ["bump", "ramp"], "description": "bump (default, v2): lift only the near-scalp part, full at peak_at (0.3) of the length, back on the original path by return_at (0.6), segment lengths kept - long hair, no flyaways; ramp = v1"}, "peak_at": {"type": "number"}, "return_at": {"type": "number"}, **SHOW}))
def t_volume_lift(a):
    keys = ("where", "offset_mm", "feather_mm", "root_part", "smooth_mm", "smooth_passes", "profile", "peak_at", "return_at")
    return edit(a, {"do": "volume_lift", **{k: a[k] for k in keys if k in a}})


@tool("studio_thicken_lock", "Opt-in (2026-10-15): a lock that reads as a thin flat sheet gets layers (2) copies of each guide in "
      "'where', stacked over / under it along the head radial at layer_mm (4) x index, root jitter_mm (1.5), offset ramping in over "
      "root_part (0.2); under copies shortened by under_shrink (0.9). Copies join the growth mesh. One undoable group.",
      obj({"where": WHERE, "layers": {"type": "integer"}, "layer_mm": {"type": "number"}, "jitter_mm": {"type": "number"},
           "root_part": {"type": "number"}, "under_shrink": {"type": "number"}, **SHOW}))
def t_thicken_lock(a):
    keys = ("where", "layers", "layer_mm", "jitter_mm", "root_part", "under_shrink")
    return edit(a, {"do": "thicken_lock", **{k: a[k] for k in keys if k in a}})


@tool("studio_rebuild_rounded", "DRASTIC, opt-in (2026-10-15): rounded short haircut generator (pixie / crop, Ves). "
      "Deletes the guides in 'where' (default all) and grows new ones: even Poisson roots (spacing_mm, default 3.5) on the "
      "painted scalp wherever vanilla card hair covers it (swirl included, no roots past the vanilla hairline); flow = the "
      "card root->tip texture direction projected on the scalp and smoothed (shell_flow_mm 18); around the crown swirl "
      "(crown [x,y,z], default 'auto' = where the card flow diverges most on the back top; null = none) the flow "
      "radiates out (swirl_radius_mm 45, swirl_spiral 0.5; nothing flows back toward it within swirl_outward_mm 80). Each strand lifts off the scalp (rise_mm 18, steeper near "
      "the crown: swirl_rise_mm 3) to the shell and follows it. shell_mode uniform (default): scalp + ONE thickness (shell_thick_mm, default the shell_thick_pct 0.75 percentile of the card heights + offset_mm 5): round, even, no sags; cards = local card height + offset; hull = convex envelope (experimental); the tip ends shell_tip_in_mm "
      "(5) inside it where the card outline ends or at the card hair length (shell_len_pct 0.9 of the card hair over the root) + offset (length_scale 1). New growth faces = "
      "Delaunay of the roots. Ignores the hair budget. One undoable op ('rebuild', Studio-only), exports via studio_to_config.",
      obj({"where": WHERE, "spacing_mm": {"type": "number"}, "offset_mm": {"type": "number"},
           "crown": {"description": "[x,y,z] edit frame, 'auto' (default) or 'none'"},
           "swirl_radius_mm": {"type": "number"}, "swirl_spiral": {"type": "number"}, "swirl_rise_mm": {"type": "number"},
           "rise_mm": {"type": "number"}, "shell_tip_in_mm": {"type": "number"}, "length_scale": {"type": "number"},
           "shell_flow_mm": {"type": "number"}, "shell_col_mm": {"type": "number", "description": "column radius for the card shell height (28)"},
           "shell_pct": {"type": "number", "description": "percentile of the card hair heights in the column = shell (0.45)"},
           "shell_smooth": {"type": "number", "description": "0..0.95 low-pass of the shell height along the strand (0.9)"},
           "shell_step_mm": {"type": "number"}, "shell_inertia": {"type": "number"}, "shell_min_mm": {"type": "number"},
           "shell_min_len_mm": {"type": "number"}, "reach_mm": {"type": "number"}, "zmin_rel": {"type": "number"},
           "edge_mm": {"type": "number"}, "copy_mm": {"type": "number"}, "eye_mm": {"type": "number"},
           "eye_mode": {"type": "string", "enum": ["reject", "cut"]}, "alpha_vis": {"type": "number"},
           "uv_mode": {"type": "string"}, "density": {"type": "number"}, "skin_exact": {"type": "boolean"},
           **SHOW}))
def t_rebuild_rounded(a):
    keys = ("where", "spacing_mm", "offset_mm", "crown", "swirl_radius_mm", "swirl_spiral", "swirl_rise_mm", "rise_mm",
            "shell_tip_in_mm", "length_scale", "shell_flow_mm", "shell_col_mm", "shell_pct", "shell_smooth", "shell_step_mm",
            "shell_inertia", "shell_min_mm", "shell_min_len_mm", "reach_mm", "zmin_rel", "edge_mm", "copy_mm", "eye_mm",
            "eye_mode", "alpha_vis", "uv_mode", "density", "skin_exact", "cand_samples", "scalp_only", "layer", "raise_mm", "shell_len_pct", "swirl_outward_mm", "swirl_min_out", "shell_mode", "shell_cone_deg", "shell_hull_mix", "shell_thick_mm", "shell_thick_pct", "shell_taper_mm", "shell_root", "shell_gap_mm", "shell_fill_mm", "shell_lap_mm", "shell_lap_iter", "shell_lap_from", "shell_dir_mm", "shell_dir_iter")
    cmd = {"do": "rebuild_from_cards", "trace": "shell", "spacing_mm": 3.5, "ignore_budget": True, "cand_samples": 400000, "layer": "inner"}
    cmd.update({k: a[k] for k in keys if k in a})
    return edit(a, cmd)


@tool("studio_make_bun", "Opt-in, Studio-only: recreate a vanilla BUN from strands that HOLDS in place. Finds the bun in the "
      "game's hair cards (most protruding card mass off the skull; or centre [x,y,z] + radius_mm), then reshapes the "
      "gathered guides (all in 'where' except those rooted more than keep_front_mm (35) forward of the head centre): "
      "root -> along the scalp (standoff_mm 4) to the bun base -> wound `turns` (2) times around the bun axis, radius "
      "shrinking, end tucked (tuck 0..1, 0.25). radius_scale / depth_scale / depth_mm resize the coil; direction +1/-1. "
      "hold (default true): stiffness map s = r = 1 on the bun guides, keep_s (0.6) on the kept guides, global "
      "stiffness / strength / root / bend 1 + flat curves, tip_stiffness (0.8). detect_only: report the bun only. "
      "Check with studio_simulate walk / run / turn / nod.",
      obj({"where": WHERE, "centre": {"type": "array", "items": {"type": "number"}}, "axis": {"type": "array", "items": {"type": "number"}},
           "radius_mm": {"type": "number"}, "radius_scale": {"type": "number"}, "depth_mm": {"type": "number"},
           "depth_scale": {"type": "number"}, "turns": {"type": "number"}, "tuck": {"type": "number"},
           "keep_front_mm": {"type": "number"}, "standoff_mm": {"type": "number"}, "direction": {"type": "number"},
           "z_below_mm": {"type": "number"}, "hold": {"type": "boolean"}, "keep_s": {"type": "number"},
           "tip_stiffness": {"type": "number"}, "detect_only": {"type": "boolean"},
           "arc_up": {"type": "number", "description": "0.6: the scalp path bends over the top so side hair passes above the ears"},
           "messy": {"type": "number", "description": "0..1 (0): random radius / depth jitter of the coil = looser, messier bun"},
           "keep_side_z_mm": {"type": "number", "description": "also keep guides rooted lower than head centre + this (mm) and more than keep_side_fwd_mm (0) forward: temple / sideburn locks stay loose instead of being dragged over the ears"},
           "keep_side_fwd_mm": {"type": "number"},
           "fill": {"type": "boolean", "description": "after gathering, densify a thin crown / sides: the widest gap between strands lying on the skin (card footprint, fill_reach_mm 20, above head centre - fill_z_below_mm 60, not ears / under the bun) gets a new guide with its own bun path, repeated while a gap is wider than cover_mm (7), fill_spacing_mm (8) apart, up to fill_max_new (60) (budget: information only)"},
           "densify_only": {"type": "boolean", "description": "the guides already form the bun (an earlier make_bun with the SAME bun parameters): skip the reshape, only fill"},
           "cover_mm": {"type": "number"}, "fill_spacing_mm": {"type": "number"}, "fill_max_new": {"type": "integer"},
           "fill_reach_mm": {"type": "number"}, "fill_z_below_mm": {"type": "number"}, "max_ratio": {"type": "number"},
           "ignore_budget": {"type": "boolean"},
           "core": {"type": "number", "description": "0..1 (0 = old ring coil): FULL domed bun - each strand spirals in to its own end radius (biased to the axis, some end inside the core), height follows a dome highest at the axis, ends dip into the core; wraps tilted by up to cross_deg (30) and counter_frac (0.25) wound the other way = crossing messy wraps"},
           "cross_deg": {"type": "number"}, "counter_frac": {"type": "number"},
           "cross_frac": {"type": "number", "description": "share of strands (0.35 x core) that first cross straight over the dome top to the far rim, then wind: their full-width middle covers the centre (strand tips alone leave it dark)"},
           "lobes": {"type": "array", "items": {"type": "object"}, "description": "MULTI-LOBE bun: [{centre [x,y,z], radius_mm?, depth_mm?, lift_mm?, axis?, weight (1), turns?, search_mm (50)}] - each gathered root goes to the nearest lobe base (distance / weight); missing radius / depth / lift are measured from the card vertices around the centre (lift = how far the lobe base stands off the skull, max_lift_mm 120)"},
           "auto_lobes": {"type": "integer", "description": "2..4: split the detected bun cards (vertices > 12 mm off the skull within lobe_search_mm 90 of the bun) into this many lobes by k-means"},
           "lobe_search_mm": {"type": "number"}, "max_lift_mm": {"type": "number"},
           "wrap_frac": {"type": "number", "description": "0..1 (0): share of gathered strands that wind round the BASE of the whole bun like a braid band (wrap_turns 1.2, wrap_braid_mm 4 zig-zag, wrap_width_mm 10, wrap_height_mm 8 over the lowest lobe base, wrap_mm / wrap_scale radius), then dive into their lobe core"},
           "wrap_turns": {"type": "number"}, "wrap_braid_mm": {"type": "number"}, "wrap_width_mm": {"type": "number"}, "wrap_height_mm": {"type": "number"},
           "wrap_mm": {"type": "number"}, "wrap_scale": {"type": "number"},
           "loose_frac": {"type": "number", "description": "0..1 (0): share of bun strands whose last loose_len (0.35) leaves the bun as a wisp / flyaway (outward, loose_droop 0.6, length x loose_length_scale 0.6); hold gives them stiffness loose_s (0.45)"},
           "loose_len": {"type": "number"}, "loose_droop": {"type": "number"}, "loose_length_scale": {"type": "number"}, "loose_s": {"type": "number"},
           "fill_any_template": {"type": "boolean", "description": "allow fill when no guide is gathered (bare crown: plant a whole new bun from the nearest guide of any kind)"},
           "smooth": {"type": "number", "description": "0..1 (0): SMOOTH sculpted twist (core 0 only): parallel helices at a near-constant lobe radius (smooth_layer 0.15 spread), same hand / pitch, rising to smooth_top (0.85) x depth, end easing in to tuck_r (0.45) x radius; plus smooth x 8 Laplacian passes on every path (no kinks)"},
           "smooth_layer": {"type": "number"}, "smooth_top": {"type": "number"}, "tuck_r": {"type": "number"},
           "smooth_sweep": {"type": "number", "description": "smooth only, rad (0 = off): every strand enters its lobe from the neck side, spread +-smooth_sweep by its left / right root position = parallel wraps (French twist) instead of a vortex of helices from all sides (HairWorks blends those into tangled ribbons)"}, **SHOW}))
def t_make_bun(a):
    keys = ("where", "centre", "axis", "radius_mm", "radius_scale", "depth_mm", "depth_scale", "turns", "tuck",
            "keep_front_mm", "standoff_mm", "direction", "z_below_mm", "hold", "keep_s", "tip_stiffness", "detect_only", "arc_up", "messy", "keep_side_z_mm", "keep_side_fwd_mm",
            "fill", "densify_only", "cover_mm", "fill_spacing_mm", "fill_max_new", "fill_reach_mm", "fill_z_below_mm", "max_ratio", "ignore_budget",
            "core", "cross_deg", "counter_frac", "cross_frac", "ear_pad_mm", "chord_mm",
            "lobes", "auto_lobes", "lobe_search_mm", "max_lift_mm", "wrap_frac", "wrap_turns", "wrap_braid_mm", "wrap_width_mm", "wrap_height_mm",
            "wrap_mm", "wrap_scale", "loose_frac", "loose_len", "loose_droop", "loose_length_scale", "loose_s", "fill_any_template", "smooth", "smooth_layer", "smooth_top", "tuck_r", "smooth_sweep")
    return edit(a, {"do": "make_bun", **{k: a[k] for k in keys if k in a}})


@tool("studio_outside_vanilla", "Guides rooted where vanilla has NO hair: farther than tol_mm (15) from every vertex of the "
      "game's own hair cards (temples, cheeks, forehead, ears). Lists them (to_cards_mm); delete: true removes them. "
      "Use a 'where' to limit it (e.g. front / near a temple); long cards hanging past the face can hide a few. Check "
      "studio_compare after (it can open scalp).",
      obj({"where": WHERE, "tol_mm": {"type": "number"}, "delete": {"type": "boolean"}, **SHOW}))
def t_outside(a):
    return edit(a, {"do": "outside_vanilla", **{k: a[k] for k in ("where", "tol_mm", "delete") if k in a}})


@tool("studio_braid", "Opt-in, Studio-only: small HairWorks 3-strand PLAIT (beard / hair braids) replacing vanilla braid "
      "cards. Source from: 'kept' (default: the studio_keep_cards / project keep_cards triangles, grouped into braids; "
      "only: index picks one), 'card' (near [x,y,z] + radius_mm: card strips touching the sphere) or 'points' (explicit "
      "top->bottom control points, width_mm, width_axis, tassel_mm). The centreline, length and width taper come from the "
      "card strips (PCA + slices; the biggest strip = braid body, strips below it = tie / tassel). Three lobes interleave as a "
      "real plait (period_mm 9, tightness 0.8 = over/under depth x lobe radius, lobe 0.3 = lobe radius / width, twist_deg, "
      "spin_per_mm, width_scale, taper 0.35, top_taper_mm 6, width_min_mm / width_max_mm), tie (tie_mm 3, tie_width 0.55), "
      "tassel (tassel_scale 1, tassel_spread 1.6, tassel_guides 6). Hair: each lobe = overlapping short PIECES (K CVs over "
      "piece_mm, default ~cvs_per_period 6 CVs per period; overlap 2) of guides_per_lobe (3) guides on a ring (ring 0.6 x "
      "lobe radius) with growth faces ONLY inside each lobe piece (no faces to other lobes / the beard = no strays). Roots "
      "are skinned like base_root's guide (default: the live guide nearest the braid top), so the braid holds like the "
      "cards; piece_mm >= braid length = whole-length guides that swing. stiffness (1): per-guide map value. uv [u,v]: "
      "growth uv (default the base guide's). drop_cards (true for kept / card) removes the braid from the kept cards. "
      "list_only: report the detected braids. One undoable 'rebuild' op (saved in projects, exported as shown). Run it on "
      "the live fur (studio_fur beard for beard braids).",
      obj({"from": {"type": "string", "enum": ["kept", "card", "points"]}, "only": {"type": "integer"},
           "near": {"type": "array", "items": {"type": "number"}}, "radius_mm": {"type": "number"},
           "points": {"type": "array"}, "width_mm": {"type": "number"}, "width_axis": {"type": "array", "items": {"type": "number"}},
           "period_mm": {"type": "number"}, "tightness": {"type": "number"}, "lobe": {"type": "number"}, "twist_deg": {"type": "number"},
           "spin_per_mm": {"type": "number"}, "width_scale": {"type": "number"}, "taper": {"type": "number"},
           "top_taper_mm": {"type": "number"}, "width_min_mm": {"type": "number"}, "width_max_mm": {"type": "number"},
           "tie_mm": {"type": "number"}, "tie_width": {"type": "number"}, "tassel_mm": {"type": "number"},
           "tassel_scale": {"type": "number"}, "tassel_spread": {"type": "number"}, "tassel_guides": {"type": "integer"},
           "piece_mm": {"type": "number"}, "cvs_per_period": {"type": "number"}, "overlap": {"type": "number"},
           "guides_per_lobe": {"type": "integer"}, "ring": {"type": "number"}, "base_root": {"type": "array", "items": {"type": "number"}},
           "stiffness": {"type": "number"}, "uv": {"type": "array", "items": {"type": "number"}}, "drop_cards": {"type": "boolean"},
           "top_points": {"type": "array", "description": "[[x,y,z],..] edit frame: each braid starts at the nearest one (mouth corners = out of the moustache sides); card points within top_blend_mm (8) dropped"}, "top_blend_mm": {"type": "number"}, "join_mm": {"type": "number"}, "slices": {"type": "integer"}, "list_only": {"type": "boolean"}, **SHOW}))
def t_braid(a):
    keys = ("from", "only", "near", "radius_mm", "points", "width_mm", "width_axis", "period_mm", "tightness", "lobe", "twist_deg",
            "spin_per_mm", "width_scale", "taper", "top_taper_mm", "width_min_mm", "width_max_mm", "tie_mm", "tie_width", "tassel_mm",
            "tassel_scale", "tassel_spread", "tassel_guides", "piece_mm", "cvs_per_period", "overlap", "guides_per_lobe", "ring",
            "base_root", "stiffness", "uv", "drop_cards", "top_points", "top_blend_mm", "join_mm", "slices", "list_only")
    return edit(a, {"do": "braid", **{k: a[k] for k in keys if k in a}})


@tool("studio_keep_cards", "Opt-in PREVIEW of cards + strands mixed: the vanilla hair cards picked by near [x,y,z] (edit "
      "frame, e.g. studio_pick scalp_edit) + radius_mm (25) stay drawn WITH the groom in every render / compare. by: "
      "'component' (default, whole card strips with a vertex in the sphere) or 'triangle'; z_min / z_max (m, absolute) "
      "limit it; uv_min / uv_max [u, v] keep only triangles whose centroid uv is in the box (a card element with its own "
      "texture tile, e.g. a braid; near is then optional); add: true accumulates picks; clear: true switches it off. Not "
      "saved in edits / projects: the game build uses the pipeline key split_cards (mode boxes, reported as pipeline_hint).",
      obj({"near": {"type": "array", "items": {"type": "number"}}, "radius_mm": {"type": "number"},
           "by": {"type": "string"}, "z_min": {"type": "number"}, "z_max": {"type": "number"},
           "uv_min": {"type": "array", "items": {"type": "number"}}, "uv_max": {"type": "array", "items": {"type": "number"}},
           "add": {"type": "boolean"}, "clear": {"type": "boolean"}}))
def t_keep_cards(a):
    return S.call({"do": "keep_cards", **{k: a[k] for k in ("near", "radius_mm", "by", "z_min", "z_max", "uv_min", "uv_max",
                                                              "add", "clear") if k in a}})


@tool("studio_card_offset", "MOVE kept vanilla card pieces (e.g. Junior's wooden tie ring, uv_min [0.95,0.94] uv_max "
      "[1,1]): the hair-card vertices picked by uv_min/uv_max (triangle centroid uv in the box) and/or near [x,y,z] + "
      "radius_mm are rotated by rotate_deg [x,y,z] about their centroid, then moved by offset [dx,dy,dz] (m, z up). SAVED "
      "in the project (card_offsets) and exported by studio_to_config.py as the config key card_offset, which the game "
      "build applies to the split_cards kept mesh. add: true appends a move (default replaces all); clear: true removes "
      "every move. Pair with studio_keep_cards so the piece is drawn.",
      obj({"uv_min": {"type": "array", "items": {"type": "number"}}, "uv_max": {"type": "array", "items": {"type": "number"}},
           "near": {"type": "array", "items": {"type": "number"}}, "radius_mm": {"type": "number"},
           "offset": {"type": "array", "items": {"type": "number"}}, "rotate_deg": {"type": "array", "items": {"type": "number"}},
           "add": {"type": "boolean"}, "clear": {"type": "boolean"}}))
def t_card_offset(a):
    return S.call({"do": "card_offset", **{k: a[k] for k in ("uv_min", "uv_max", "near", "radius_mm", "offset", "rotate_deg",
                                                               "add", "clear") if k in a}})


@tool("studio_feather_hairline", "Soften a HARD hairline (dense wall of roots, harsh under root alpha mode 0): removes "
      "the growth faces on the open edge of the growth mesh inside 'where' (default front: true) and part of the next "
      "row (keep 0.5), so hair density fades in at the line. Guides stay; rows 1 or 2. list_only first; compare after "
      "(it can open scalp just behind the line -> studio_fill_scalp).",
      obj({"where": WHERE, "rows": {"type": "integer"}, "keep": {"type": "number"}, "list_only": {"type": "boolean"}, **SHOW}))
def t_feather(a):
    return edit(a, {"do": "feather_hairline", **{k: a[k] for k in ("where", "rows", "keep", "list_only") if k in a}})


@tool("studio_trim_growth", "Clear stray IN-BETWEEN hair (strands HairWorks interpolates across growth faces, e.g. "
      "thin hair lying on the forehead at a parting or hairline). Default: divergent faces = their 3 guides go different "
      "ways (direction angle > div_deg 50 AND tips spread > tip_spread 4 x the root spacing): HairWorks fans hair across "
      "that gap. Opt-in: off_scalp (face on skin outside the painted scalp, > tol_mm) and stretched (longest edge > "
      "max_edge_mm or edge_factor x median) - both remove a lot, use a tight 'where'. Guides stay; only the hair between "
      "them goes. list_only first; check scalp coverage and studio_compare after (a removed face can open scalp).",
      obj({"where": WHERE, "divergent": {"type": "boolean"}, "div_deg": {"type": "number"}, "tip_spread": {"type": "number"},
           "off_scalp": {"type": "boolean"}, "stretched": {"type": "boolean"},
           "tol_mm": {"type": "number"}, "max_edge_mm": {"type": "number"}, "edge_factor": {"type": "number"},
           "list_only": {"type": "boolean"}, **SHOW}))
def t_trim(a):
    return edit(a, {"do": "trim_growth", **{k: a[k] for k in ("where", "divergent", "div_deg", "tip_spread", "off_scalp",
                                                            "stretched", "tol_mm", "max_edge_mm",
                                                            "edge_factor", "list_only") if k in a}})


@tool("studio_check_loops", "Read-only: list guides that form an unwanted loop - the strand comes back to within return_mm (15) of "
      "a point it passed (or its own root) after more than min_arc_mm (60) of hair, e.g. over the top and back, or back "
      "to the crown; or the strand circles >= circling_deg (270) in total. Tight repeated turns (curls: loops up to curl_radius_mm 45) are not flagged. Returns "
      "[{guide, reason}]. Fix with studio_delete or the Check > Unwanted loops panel.",
      obj({"min_arc_mm": {"type": "number"}, "return_mm": {"type": "number"}, "curl_radius_mm": {"type": "number"}, "circling_deg": {"type": "number"}}))
def t_check_loops(a):
    return S.call({"do": "check_loops", **{k: a[k] for k in ("min_arc_mm", "return_mm", "curl_radius_mm", "circling_deg") if k in a}})


@tool("studio_wind_check", "Read-only, measured MAX-WIND stray check (numbers, not screenshots; ~100 s with defaults). Rest = exact hair-only "
      "mask with the simulation off, per camera; then fresh simulations at wind `amount` (5 = toolbar maximum) for `seconds` (6), "
      "fixed 1/60 s steps, `frames` (30) mask renders, and every step the SIMULATED guide CVs read back from the GPU. The SDK wind is "
      "NOT deterministic (two identical runs differ by up to ~250 mm per CV from step 2), so the baseline is the envelope (per-pixel / "
      "per-guide max) of `repeats` (6) runs, split in two half-envelopes whose difference is the noise floor; `repeatability` reports "
      "the run1/run2 CV differences and mask IoUs. Per camera / pixel: farthest distance (px) hair gets from the rest silhouette, "
      "> threshold_px (12 at size 600) is out of place. Candidates = guides / growth faces whose own simulated hair strays more than "
      "stray_px (24) beyond the median of their 16 nearest-root neighbours (max_candidates 120). Each is CONFIRMED by elimination: "
      "hide it (independent batches, then overlapping groups halved by azimuth) and re-run the same wind (an envelope of repeats/2 "
      "runs); its footprint must empty beyond the noise. Verdicts: clears (>= 50 %), reduces, clump (only goes away with its group), "
      "replaced (hiding it lets neighbours fly farther: hair interaction couples guides), not cleared, untested. Returns cameras "
      "(flagged_px, max_px, mm_per_px_at_head, calm / half-envelope px), strays, flyers, culprits [{kind guide|face, id, guides, "
      "stray_px, max_px, max_mm, cameras, footprint, root, length_mm, cause: loop / over-long / divergent face fan / soft stiffness / "
      "other, verdict, confirmed, removal, removal_needed, others_fly_farther, group}], groups, tests log, debug PNGs per camera. "
      "cameras: 6 (default: top, front_top, back, left, right, front), a count, names, or [{eye, target, fovy, name}]. confirm (true), "
      "max_runs (64) elimination tests, min_removal (0.15), batch_spacing_mm (40), wind_noise (override; default keeps the fur's), "
      "calm (true), inspect [guide ids] for per-guide detail. Restores the state, adds no ops.",
      obj({"seconds": {"type": "number"}, "frames": {"type": "integer"}, "cameras": {}, "threshold_px": {"type": "number"},
           "size": {"type": "integer"}, "amount": {"type": "number"}, "confirm": {"type": "boolean"}, "max_runs": {"type": "integer"},
           "min_removal": {"type": "number"}, "min_cluster_px": {"type": "number"}, "repeat": {"type": "boolean"}, "calm": {"type": "boolean"},
           "repeats": {"type": "integer"}, "wind_noise": {"type": "number"}, "ramp_s": {"type": "number"}, "inspect": {"type": "array", "items": {"type": "integer"}},
           "png": {"type": "boolean"}, "batch_spacing_mm": {"type": "number"}, "stray_px": {"type": "number"}, "max_candidates": {"type": "integer"}}))
def t_wind_check(a):
    keys = ("seconds", "frames", "cameras", "threshold_px", "size", "amount", "confirm", "max_runs", "min_removal", "min_cluster_px", "repeat",
            "calm", "repeats", "wind_noise", "ramp_s", "inspect", "png", "batch_spacing_mm", "stray_px", "max_candidates")
    return S.call({"do": "wind_check", **{k: a[k] for k in keys if k in a}}, timeout=14400)


@tool("studio_band_sim", "Read-only, measured: do SIMULATED strands stay under a HEADBAND? Detects the band like studio_headband (mesh, "
      "z_range, max_height_mm, min_coverage) and, per wind amount in `amounts` ([0, 1.5, 5]; 5 = toolbar max), runs fresh simulations "
      "(`seconds` 6 at 1/60 s, `repeats` 2: the SDK wind is random) reading back the simulated guide CVs every step; a guide is OUTSIDE "
      "when a point of its simulated strand in the band's height range lies beyond the band's inner surface minus margin_mm (0.75). "
      "Returns rest_crossing / rest_over_outer (sim off), and per amount max_crossing (max over steps), mean_crossing, guides_ever_outside, "
      "the same for over_outer (beyond the band's OUTER radius: visibly on top of it), worst [{guide, steps_outside, steps_over_outer, "
      "max_excess_mm (beyond the inner surface), root}] (worst 15), and the sway envelope of the hair below sway_below_z (head centre - 0.2 m): per guide "
      "max displacement from rest, p50 / p90 / max mm over those guides. wind_noise overrides the fur's. Restores the state, adds no ops.",
      obj({"amounts": {"type": "array", "items": {"type": "number"}}, "seconds": {"type": "number"}, "repeats": {"type": "integer"},
           "wind_noise": {"type": "number"}, "margin_mm": {"type": "number"}, "worst": {"type": "integer"}, "sway_below_z": {"type": "number"},
           "mesh": {"type": "string"}, "z_range": {"type": "array", "items": {"type": "number"}}, "max_height_mm": {"type": "number"},
           "min_coverage": {"type": "number"},
           "wind_yaw_deg": {"type": "number"}, "wind_pitch_deg": {"type": "number"}}))
def t_band_sim(a):
    keys = ("amounts", "seconds", "repeats", "wind_noise", "margin_mm", "worst", "sway_below_z", "mesh", "z_range", "max_height_mm", "min_coverage", "wind_yaw_deg", "wind_pitch_deg")
    return S.call({"do": "band_sim", **{k: a[k] for k in keys if k in a}}, timeout=7200)


@tool("studio_collision_band", "Opt-in fix for hair pushed OVER a headband by the collision bodies (measure first with studio_band_sim: "
      "compare useCollision 0 / 1). Detects the band like studio_headband and shrinks every collision sphere whose surface, inside the "
      "band's height range, comes closer than clearance_mm (2) to the band's inner surface (binary search on the radius); capsules "
      "(sphere pairs) are sampled between their ends and both ends shrink until they fit; a sphere left under min_r_mm (3) is turned "
      "off (0.1 mm). Radii are project state (saved as collision_r, exported in the geo json and written into the fur by the Studio "
      "export / studio_geo build). list_only: report only. set {sphere index (string): radius m, or -1 = the pack's}. reset: "
      "clear all overrides. Returns changed [{sphere, bone, c, r_old_mm, r_new_mm, excess_mm, off}], capsules_shrunk, collision_r.",
      obj({"clearance_mm": {"type": "number"}, "min_r_mm": {"type": "number"}, "list_only": {"type": "boolean"}, "set": {"type": "object"},
           "reset": {"type": "boolean"}, "mesh": {"type": "string"}, "z_range": {"type": "array", "items": {"type": "number"}},
           "max_height_mm": {"type": "number"}, "min_coverage": {"type": "number"}}))
def t_collision_band(a):
    keys = ("clearance_mm", "min_r_mm", "list_only", "set", "reset", "mesh", "z_range", "max_height_mm", "min_coverage")
    return S.call({"do": "collision_band", **{k: a[k] for k in keys if k in a}})


@tool("studio_list_colliders", "List the collision spheres / capsules of the open fur (the fur's own, or the project's fitted / edited set): "
      "per sphere {i, bone, grp (0 base = cranium / neck / ears, kept by a fit; 1 fitted or outfit / back item, replaced by a fit; 2 manual; -1 as built, not "
      "classified yet), tag (fit target), local_mm (bone-local), c_mm (rest, model space), r_mm, capsules_with}, the capsule pairs, counts "
      "(limits 85 spheres / 75 capsules) and the last fit report. targets: true also lists the meshes studio_fit_colliders can fit "
      "(outfit meshes incl. outfit-mod ones, hood, scabbard, sword; ids to pass as `meshes`).",
      obj({"targets": {"type": "boolean"}, "scan": {"type": "boolean", "description": "re-scan the fit targets"}}))
def t_list_colliders(a):
    return S.call({"do": "list_colliders", **{k: a[k] for k in ("targets", "scan") if k in a}})


@tool("studio_fit_colliders", "Fit collision spheres + capsules to the geometry shown on the character (the pipeline's own fitters, run on this "
      "Studio's hair): meshes = target ids / labels from studio_list_colliders targets (or 'default' / 'all'; none = the defaults). Each sphere is "
      "bound to the skin bone that moves the mesh part and a capsule joins neighbours; spheres that would be alone get a twin + capsule (only capsules "
      "collide). The previous outfit / back-item colliders are replaced; head / neck / ear colliders and manual ones (keep_manual, default true) stay. "
      "max_spheres / max_capsules (85 / 75, the vanilla maxima, twins included): per-mesh sphere counts shrink until the set fits. pad_mm (2) is "
      "added to every radius; reach_mm only fits mesh parts within that distance of a guide (default: as the config says; 0 = whole mesh). "
      "The first fit of a build also runs the pipeline once (~1 min) to learn which spheres are outfit ones. Project state (undo with studio_edit_collider "
      "op undo, saved in the project, exported into the fur). Returns report {targets {id: spheres, vertices, coverage_pct (share of the mesh vertices within 8 mm "
      "of a capsule, whole set), coverage_before_pct}, total_spheres, total_capsules, ...}. Does not support dry_run.",
      obj({"meshes": {"type": "array", "items": {"type": "string"}}, "max_spheres": {"type": "integer"}, "max_capsules": {"type": "integer"},
           "pad_mm": {"type": "number"}, "reach_mm": {"type": "number"}, "keep_manual": {"type": "boolean"}}))
def t_fit_colliders(a):
    return S.call({"do": "fit_colliders", **{k: a[k] for k in ("meshes", "max_spheres", "max_capsules", "pad_mm", "reach_mm", "keep_manual") if k in a}}, timeout=1800)


@tool("studio_edit_collider", "Edit one collider by hand (all undoable): op add {bone (name; default = the bone of the region the sphere sits in: head above the neck, neck, else torso3), world_mm [x,y,z] or local_mm, r_mm (20), "
      "twin (true: + a twin sphere and capsule)}, move / set {index or indices, local_mm (absolute, bone-local), offset_mm (relative, bone-local), "
      "world_mm (absolute) or world_offset_mm; set also r_mm / scale}, resize {index, r_mm or scale}, bone {index, bone: rebind keeping the world "
      "position}, delete {index / indices; their twins go too}, mirror {index / indices: mirror image about the head's mid-plane on the l_/r_ "
      "bone, with capsules between mirrored spheres}, link / unlink {a, b: capsule between two spheres}, select {index, index2}, reset (back to the build's "
      "own set), undo, redo. A hand-edited fitted sphere becomes manual (a refit keeps it). "
      "FLATS (HairWorks collides with spheres / capsules only, so a flat is a panel of N thin parallel capsules side by side across its "
      "width, each running along its height, each a pair of spheres tagged \"flat#k\", grp 2 manual; one undo step per op): add_flat {bone (default "
      "torso3), world_mm or centre_mm [x,y,z] (centre, rest pose, default the head), width_mm, height_mm, thickness_mm (= capsule radius), "
      "yaw_deg / pitch_deg / roll_deg or angle [yaw, pitch, roll] or normal [x,y,z] (0/0/0 = facing +y, width along x, height along z), count "
      "(default width / (2 * thickness), capped to the free budget of 85 spheres / 75 capsules)} -> {tag, flat, sphere_indices, params}. "
      "The flat ops take tag (\"flat#k\"), flat (k) or index (one of its spheres): flat_move {world_mm / centre_mm absolute or world_offset_mm, "
      "bone}, flat_resize {width_mm, height_mm, thickness_mm, count, scale}, flat_rotate {yaw_deg, pitch_deg, roll_deg, angle, normal, or "
      "d_yaw_deg / d_pitch_deg / d_roll_deg relative}, flat_set (any of these), flat_mirror (mirror copy on the l_/r_ bone), flat_delete. "
      "studio_list_colliders lists them under 'flats'.",
      obj({"op": {"type": "string", "enum": ["add", "move", "set", "resize", "bone", "rebind", "delete", "mirror", "link", "unlink", "select", "reset", "undo", "redo",
                                             "add_flat", "flat_move", "flat_resize", "flat_rotate", "flat_set", "flat_mirror", "flat_delete"]},
           "tag": {"type": "string"}, "flat": {"type": "integer"}, "centre_mm": {"type": "array", "items": {"type": "number"}},
           "width_mm": {"type": "number"}, "height_mm": {"type": "number"}, "thickness_mm": {"type": "number"}, "count": {"type": "integer"},
           "yaw_deg": {"type": "number"}, "pitch_deg": {"type": "number"}, "roll_deg": {"type": "number"},
           "d_yaw_deg": {"type": "number"}, "d_pitch_deg": {"type": "number"}, "d_roll_deg": {"type": "number"},
           "angle": {"type": "array", "items": {"type": "number"}}, "normal": {"type": "array", "items": {"type": "number"}},
           "index": {"type": "integer"}, "indices": {"type": "array", "items": {"type": "integer"}}, "index2": {"type": "integer"},
           "a": {"type": "integer"}, "b": {"type": "integer"}, "bone": {"type": "string"},
           "local_mm": {"type": "array", "items": {"type": "number"}}, "offset_mm": {"type": "array", "items": {"type": "number"}},
           "world_mm": {"type": "array", "items": {"type": "number"}}, "world_offset_mm": {"type": "array", "items": {"type": "number"}},
           "r_mm": {"type": "number"}, "scale": {"type": "number"}, "twin": {"type": "boolean"}}, ["op"]))
def t_edit_collider(a):
    return S.call({"do": "edit_collider", **a})


@tool("studio_rebind_colliders", "2026-10-16: rebind collision spheres to the bone of the REGION they sit in (head above the neck bone + 20 mm, "
      "neck / neck2 within 40 mm below it, else torso3), or to `bone`, keeping their rest-pose world position (local recomputed through "
      "the target bone's bind pose). Why: a sphere placed around the head but bound to torso3 (the old default of new spheres / flats; "
      "geralt_user_1949: 18 headband-ring spheres) does not turn with the head in game and eats the hair when the character moves. "
      "Default every sphere; indices limits it; capsule partners are rebound together. list_only reports the plan. One undo step. "
      "studio_list_colliders flags such spheres under misbound (bone_suggested per sphere).",
      obj({"indices": {"type": "array", "items": {"type": "integer"}}, "bone": {"type": "string"}, "list_only": {"type": "boolean"}}))
def t_rebind_colliders(a):
    return S.call({"do": "edit_collider", "op": "rebind", **{k: a[k] for k in ("indices", "bone", "list_only") if k in a}})


@tool("studio_collider_report", "Clearance report of the colliders (read-only). (1) guides whose strand ends inside a collider: mode 'rest' = the authored hair "
      "at rest; mode 'sim' = the SDK simulation read back after `steps` (180) frames with a motion `preset` (Idle, TurnHead, Nod, Walk, Run, SpinBody, "
      "Wind; default the current one), CVs from from_cv on, deeper than tol_mm (1) inside a capsule; per guide the deepest CV, the capsule and its "
      "group. (2) capsules poking out of the skin: surface samples of every capsule outside the nearest shown mesh surface (head, body, outfit, scabbard, sword) by more than poke_mm (3), "
      "indicative. Also spheres_without_capsule (inactive in the game).",
      obj({"mode": {"type": "string", "enum": ["rest", "sim"]}, "preset": {"type": "string"}, "steps": {"type": "integer"}, "wind": {"type": "number"},
           "tol_mm": {"type": "number"}, "poke_mm": {"type": "number"}, "from_cv": {"type": "integer"}, "worst": {"type": "integer"}}))
def t_collider_report(a):
    return S.call({"do": "collider_report", **a}, timeout=1800)


DRAPE_MESHES = {"type": "array", "items": {"type": ["string", "integer"]},
                "description": "scene meshes: indices from studio_list_meshes, substrings of name / texture / path (e.g. 'item_10', 'scabbard', 'sword'), "
                               "'hood' (the modCiri hood roll chunks), 'outfit' (all outfit pieces), 'all'"}


@tool("studio_list_meshes", "List the scene meshes a drape / report can use (outfit pieces, outfit-mod meshes, hood, scabbard, sword): index, name, "
      "texture, triangles, height range, hood flag.", obj({}))
def t_list_meshes(a):
    return S.call({"do": "list_meshes", "meshes": ["all"]})


@tool("studio_drape_outfit", "Opt-in: DRAPE hair over scene meshes (hood, scabbard, sword, outfit pieces incl. outfit-mod meshes). At rest, guide points "
      "that are inside or within pad_mm (3) of the selected meshes' OUTER surface (outermost radius around the body axis per azimuth / height) are "
      "moved to the outside and the rest of the strand is carried so it keeps its segment lengths and shape and drapes over the piece. The first "
      "root_mm (20) of every strand never moves; points more than max_push_mm (150) under the surface are left alone. One reshape op per changed "
      "guide (undoable with studio_undo, saved by studio_save, replayed by the export). Reports guides / points moved, max shift, max length "
      "change (fallback = a segment had to stretch). box [xmin,xmax,ymin,ymax,zmin,zmax] limits the triangles used; axis [x,y] = body axis "
      "(default the head centre); where = guide selector; list_only = measure only.",
      obj({"meshes": DRAPE_MESHES, "pad_mm": {"type": "number"}, "root_mm": {"type": "number"}, "max_push_mm": {"type": "number"},
           "box": {"type": "array", "items": {"type": "number"}}, "axis": {"type": "array", "items": {"type": "number"}},
           "where": WHERE, "list_only": {"type": "boolean"},
           "shell": {"type": "boolean", "description": "only points between the pieces' inner and outer surface count as inside (default on when "
                     "only sword / scabbard meshes are selected: hair between the back and the sword stays put)"},
           "tube": {"type": "boolean", "description": "fit the pieces as one long thin piece (sword hilt) along its own axis and move hair SIDEWAYS off that "
                    "axis around it (the default radial push throws the back hair over a hilt into a wing)"},
           "split_faces": {"type": "boolean", "description": "with tube: also remove the growth faces whose in-between hair runs through the piece "
                           "(guides on both sides of the hilt) -> hair parts around it"}, "face_pad_mm": {"type": "number"}, "face_zmin": {"type": "number"}, "face_zmax": {"type": "number"}, "tube_side": {"type": "string", "enum": ["body", "outer", "axis"], "description": "tube: which side every strand passes (axis = straight off the axis, default; body = between the back/head and the piece: tested worse on Ciri, cut a hole)"}, **SHOW}, ["meshes"]))
def t_drape(a):
    return edit(a, {"do": "drape_outfit", **{k: a[k] for k in ("meshes", "pad_mm", "root_mm", "max_push_mm", "box", "axis", "where", "list_only", "shell", "tube", "split_faces", "face_pad_mm", "face_zmin", "face_zmax", "tube_side") if k in a}})


@tool("studio_drape_report", "Read-only: how many guides / points are inside (or within pad_mm) the selected meshes. mode 'rest' = the authored strands "
      "(CVs after the root part + 2 samples per span); mode 'sim' = the SDK simulation read back after `steps` (240) frames with a motion `preset` "
      "(Idle, Walk, ...; the meshes are posed like the Studio draws them), lower 60 % of the refined strand, frames of the second half. "
      "Returns guides_inside, points_inside, ..._or_within_pad, deepest_mm; ids: true adds ids_deeper_5mm / 10mm / 20mm (guide ids).",
      obj({"meshes": DRAPE_MESHES, "mode": {"type": "string", "enum": ["rest", "sim"]}, "preset": {"type": "string"}, "steps": {"type": "integer"},
           "wind": {"type": "number"}, "pad_mm": {"type": "number"}, "root_mm": {"type": "number"}, "max_push_mm": {"type": "number"},
           "box": {"type": "array", "items": {"type": "number"}}, "axis": {"type": "array", "items": {"type": "number"}}, "ids": {"type": "boolean"}}, ["meshes"]))
def t_drape_report(a):
    return S.call({"do": "drape_report", **a}, timeout=1800)


@tool("studio_grow_colliders", "Opt-in: FILL the shell of a piece with collision spheres. The chosen spheres (spheres [indices], or tag = the fit target they "
      "came from, e.g. 'hood' after studio_fit_colliders, or sphere_box [xmin,xmax,ymin,ymax,zmin,zmax] on their centres) are re-centred between the "
      "inner and the outer surface of the selected meshes (same azimuth and height, the outer surface + pad_mm (2) stays touched) and get the radius that "
      "spans both (min_r_mm 8 .. max_r_mm 60). The pipeline's back-item fit puts small spheres on the back surface of a thick rolled hood; hair that falls "
      "in over its top edge ends up inside the cloth between those spheres and the neck. Undoable (studio_edit_collider undo), saved in the project, "
      "written into the fur by the export. list_only: report the changes.",
      obj({"meshes": DRAPE_MESHES, "pad_mm": {"type": "number"}, "min_r_mm": {"type": "number"}, "max_r_mm": {"type": "number"}, "spheres": {"type": "array", "items": {"type": "integer"}},
           "tag": {"type": "string"}, "sphere_box": {"type": "array", "items": {"type": "number"}},
           "box": {"type": "array", "items": {"type": "number"}}, "axis": {"type": "array", "items": {"type": "number"}}, "list_only": {"type": "boolean"}}, ["meshes"]))
def t_grow_colliders(a):
    return S.call({"do": "grow_colliders", **a})


@tool("studio_shell_colliders", "Opt-in: collision chains that FILL the shell of a piece: for each height in rows_z (m) spheres every step_deg (30) around the body axis from "
      "az_from to az_to (degrees, 0 = +x, -90 = straight back; default -165 .. -20), each re-centred between the inner and the outer surface of the selected meshes "
      "(radius up to max_r_mm 60, outer surface + pad_mm 2 touched), chained with capsules. replace {tag, z_tol_mm}: removes the spheres with that fit tag "
      "(e.g. 'hood') at those heights first. For a thick rolled hood the pipeline's back sheet leaves the sides of the collar open, hair falls in there and ends "
      "up inside the cloth. Budget 85 spheres is checked. Undoable (studio_edit_collider undo), saved in the project, exported into the fur. list_only: report the spheres.",
      obj({"meshes": DRAPE_MESHES, "rows_z": {"type": "array", "items": {"type": "number"}}, "az_from": {"type": "number"}, "az_to": {"type": "number"},
           "step_deg": {"type": "number"}, "pad_mm": {"type": "number"}, "min_r_mm": {"type": "number"}, "max_r_mm": {"type": "number"}, "bone": {"type": "string"},
           "replace": {"type": "object"}, "box": {"type": "array", "items": {"type": "number"}}, "axis": {"type": "array", "items": {"type": "number"}}, "list_only": {"type": "boolean"}}, ["meshes", "rows_z"]))
def t_shell_colliders(a):
    return S.call({"do": "shell_colliders", **a})


@tool("studio_dedupe", "Remove duplicate guides (root within tol_mm 0.6 and tip within 4x that of another guide): "
      "copies left by repeated plant / fill / duplicate calls cost budget without adding hair. list_only: just list.",
      obj({"tol_mm": {"type": "number"}, "list_only": {"type": "boolean"}}))
def t_dedupe(a):
    return S.call({"do": "dedupe", **{k: a[k] for k in ("tol_mm", "list_only") if k in a}})


@tool("studio_clear_eye", "Clear hair from in front of the eyes automatically: runs studio_eye_check, takes the culprit "
      "guides of the worst eye, combs their tips away from the face (sideways toward their own side, back and up, tip "
      "falloff), back and slightly up, each guide at most max_total_mm 50; re-checks and repeats (max_rounds 4, step_mm 20) "
      "until worst_increase < target (0.05) or no progress. Each round is "
      "one undoable call. Returns the eye_check after every round. Check studio_compare after: combing can thin a "
      "fringe that vanilla has.",
      obj({"max_rounds": {"type": "integer"}, "step_mm": {"type": "number"}, "target": {"type": "number"},
           "max_total_mm": {"type": "number"},
           "shots": {"type": "array", "items": {"type": "integer"}}}))
def t_clear_eye(a):
    target = a.get("target", 0.05)
    step = a.get("step_mm", 20) / 1000.0
    hist, moved, last = [], {}, None
    for rnd in range(a.get("max_rounds", 4)):
        chk = t_eye({k: a[k] for k in ("shots",) if k in a})
        hist.append({"round": rnd, "worst_increase": chk["worst_increase"]})
        if chk["worst_increase"] < target:
            break
        if last is not None and chk["worst_increase"] > last - 0.005:
            hist[-1]["note"] = ("no progress: stopped. Culprits rooted where vanilla has no hair -> studio_outside_vanilla "
                                "(delete) is the real fix; otherwise vanilla may frame the eye here (compare)")
            break
        last = chk["worst_increase"]
        culprits = []
        for row in chk["shots"]:
            for e in ("eye_char_left", "eye_char_right"):
                v = row.get(e)
                if isinstance(v, dict) and v.get("change", 0) >= target:
                    culprits += v.get("culprits", [])
        if not culprits:
            hist[-1]["note"] = "no culprits listed (coverage may come from interpolated hair: try trim_growth near the eye)"
            break
        frame = S.call({"do": "guides", "where": {"indices": [culprits[0]["i"]]}, "limit": 1})["frame"]
        cx = frame["head_centre"][0]
        seen = set()
        for cu in culprits:
            if cu["i"] in seen:
                continue
            seen.add(cu["i"])
            # back and slightly up only (a sideways push compounds into locks sticking out of the silhouette);
            # every guide moves at most max_total_mm over all rounds
            moved[cu["i"]] = moved.get(cu["i"], 0.0) + step
            if moved[cu["i"]] > a.get("max_total_mm", 50) / 1000.0:
                continue
            S.call({"do": "apply", "type": "translate", "where": {"roots": [cu["root"]]},
                    "offset": [0.0, -step, 0.35 * step], "falloff": "tip", "from": 0})
        hist[-1]["combed"] = len(seen)
    final = t_eye({k: a[k] for k in ("shots",) if k in a})
    return {"rounds": hist, "final_worst_increase": final["worst_increase"], "final": final}


@tool("studio_color_check", "Hair colour vs vanilla: for each camera (shots, default [4, 3]) renders the groom, the "
      "game's hair cards and the head without hair from the same camera, takes the pixels each hair covers, and compares "
      "their mean colour (sRGB, brightness, saturation). Suggests a multiplier for graphicalMaterials.color root/tip "
      "colours. The cards' tint comes from the groom's own colour (no vanilla tint data), so the check is about the "
      "LIGHTING / shading result; compare with the in-game still too (studio_compare vanilla: both).",
      obj({"shots": {"type": "array", "items": {"type": "integer"}}}))
def t_color(a):
    import numpy as np
    res = []
    for sh in a.get("shots", [4, 3]):
        cur, _ = render({"shot": sh}, "col_cur")
        van, _ = render({"shot": sh}, "col_van", {"vanilla": True})
        bare, _ = render({"shot": sh}, "col_bare", {"hair": False})
        C, V, B = (np.asarray(Image.open(p).convert("RGB"), np.float32) for p in (cur, van, bare))
        mc = np.abs(C - B).sum(2) > 45
        mv = np.abs(V - B).sum(2) > 45
        if mc.sum() < 50 or mv.sum() < 50:
            continue
        c, v = C[mc].mean(0), V[mv].mean(0)
        def sat(x): return float((x.max() - x.min()) / max(x.max(), 1))
        row = {"shot": sh, "groom_rgb": [round(float(x)) for x in c], "cards_rgb": [round(float(x)) for x in v],
               "saturation": {"groom": round(sat(c), 3), "cards": round(sat(v), 3)}}
        # the real reference: the in-game still, sampled inside the cards' hair area shrunk by 4 % of the image (the
        # still is framed close to, not exactly like, the Studio camera)
        shots = S.call({"do": "shots_list"})["shots"]
        ref = next((x.get("ref") for x in shots if str(x["id"]) == str(sh)), None)
        if ref and os.path.isfile(os.path.join(REPO, ref)):
            from scipy.ndimage import binary_erosion
            G = np.asarray(Image.open(os.path.join(REPO, ref)).convert("RGB").resize((C.shape[1], C.shape[0])), np.float32)
            core = binary_erosion(mv, iterations=max(3, C.shape[0] // 25))
            if core.sum() > 200:
                g = G[core].mean(0)
                row["ingame_rgb"] = [round(float(x)) for x in g]
                row["saturation"]["ingame"] = round(sat(g), 3)
                row["brightness_ratio_vs_ingame"] = round(float(c.mean() / max(g.mean(), 1)), 3)
                row["suggested_rgb_multiplier"] = [round(float(g[i] / max(c[i], 1)), 3) for i in range(3)]
                row["reference"] = "in-game still (game lighting; the Studio renders darker / redder: apply about half)"
        if "suggested_rgb_multiplier" not in row:
            row["reference"] = "cards only: their tint comes from the groom, so only shading differences show; no suggestion"
        res.append(row)
    # also the in-game still where it exists (real game lighting)
    return {"shots": res, "note": "apply with studio_look_set, e.g. {\"set\": {\"graphicalMaterials.color.rootColor\": "
            "{\"Red\": 54, \"Green\": 42, \"Blue\": 24, \"Alpha\": 255}}} (and tipColor); check studio_compare "
            "vanilla: both after"}


@tool("studio_eye_check", "Hair over the eyes, measured on the RENDERED strands (not guides): per camera (shots, "
      "default [4, 3, 15]) the fraction of each padded eye box hidden by hair. worst_covered should stay < 0.05 "
      "(vanilla ~0). Run it after any edit that moves front hair.",
      obj({"shots": {"type": "array", "items": {"type": "integer"}}, "pad_mm": {"type": "number"},
           "baseline": {"type": "boolean"}}))
def t_eye(a):
    cmd = {"do": "eye_check", **{k: a[k] for k in ("shots", "pad_mm") if k in a}}
    cur = S.call(cmd)
    # reference = the game's own hair cards from the same cameras (vanilla) when the pack has them, else the build as
    # opened; the padded eye box also catches hair framing the temple, so judge the CHANGE against the reference
    cards = S.call({"do": "shots_list"}).get("vanilla_cards", False)
    base = S.call({**cmd, "vanilla": True} if cards else {**cmd, "baseline": True})
    rows, worst_inc = [], 0.0
    for c, b in zip(cur.get("shots", []), base.get("shots", [])):
        row = {"shot": c["shot"], "view": c.get("view")}
        for e in ("eye_char_left", "eye_char_right"):
            if isinstance(c.get(e), dict) and isinstance(b.get(e), dict):
                inc = round(c[e]["covered"] - b[e]["covered"], 3)
                row[e] = {"covered": c[e]["covered"], "reference": b[e]["covered"], "change": inc}
                if inc > 0.02 and c[e].get("culprits"):
                    row[e]["culprits"] = c[e]["culprits"]   # guides putting hair in front of this eye (most first)
                worst_inc = max(worst_inc, inc)
        rows.append(row)
    return {"shots": rows, "worst_increase": round(worst_inc, 3),
            "reference": "vanilla cards" if cards else "build as opened", "verdict": "OK" if worst_inc < 0.05 else "MORE HAIR OVER AN EYE than the reference: check the images",
            "note": "covered = fraction of the padded eye box hidden by rendered strands (includes temple framing)"}


@tool("studio_lay_down", "Frizz fix WITHOUT deleting (deleting short guides usually opens the scalp): guides whose "
      "root part lifts off the scalp more than min_lift_deg (35) are bent down over span_mm (25) to lie target_lift_deg "
      "(8) above it, along the local flow (8 neighbours) or 'direction'; the rest of each strand keeps its shape. "
      "max_length limits it to short guides (e.g. 0.25). Check guides_over_face in the status afterwards.",
      obj({"where": WHERE, "min_lift_deg": {"type": "number"}, "target_lift_deg": {"type": "number"},
           "span_mm": {"type": "number"}, "max_length": {"type": "number"},
           "direction": {"type": "array", "items": {"type": "number"}}, **SHOW}))
def t_lay(a):
    return edit(a, {"do": "lay_down", "where": a.get("where", {"all": True}),
                    **{k: a[k] for k in ("min_lift_deg", "target_lift_deg", "span_mm", "max_length", "direction") if k in a}})


@tool("studio_fill_scalp", "Closed-loop bald-spot filler: renders the scalp overlay from the camera, ray-casts the "
      "visible green pixels to the scalp, and plants a guide at each spot (spacing_mm 4, max_new 60): a copy of a "
      "nearby long guide (min_length 0.15) whose root flows like its 8 neighbours, root part turned to that local flow "
      "(or 'direction') and lying on the scalp (span_mm 25, lift_mm 3). Re-measure with studio_scalp_coverage and repeat "
      "from other cameras; watch the budget.",
      obj({**CAM, "size": {"type": "integer"}, "spacing_mm": {"type": "number"}, "max_new": {"type": "integer"},
           "min_length": {"type": "number"}, "direction": {"type": "array", "items": {"type": "number"}},
           "template_mm": {"type": "number", "description": "max distance of the template guide (12): far copies pass through the head"},
           "max_length": {"type": "number", "description": "cap the planted guides' length (m), e.g. on a fringe so copies stop above the eyes"},
           "strength": {"type": "number"}, "span_mm": {"type": "number"}, "lift_mm": {"type": "number"},
           "sample_px": {"type": "integer"}, **{k: v for k, v in IMG.items() if k != "size"},
           "show": {"type": "boolean"}}))
def t_fill(a):
    r = edit(a, {"do": "fill_scalp", **{k: a[k] for k in ("shot", "view", "eye", "target", "fovy", "size", "spacing_mm",
                                                           "max_new", "min_length", "direction", "strength", "span_mm", "template_mm", "max_length",
                                                           "lift_mm", "sample_px") if k in a}})
    if isinstance(r, dict) and r.get("no_template_outside_head", 0) > max(5, r.get("planted", 0)):
        r["hint"] = ("most spots found no template: short hair -> raise template_mm (20-30) and lower min_length "
                     "(0.04-0.08)")
    return r


@tool("studio_plant_parting", "Cover a parting cleanly: plant new long guides along both edges of the parting line "
      "[[x,y,z],[x,y,z]] (edit frame; get points with studio_pick -> scalp_edit), every spacing_mm (4), at offsets_mm "
      "(4) from the line, sides both|plus|minus. Each is a copy of the nearest long guide (min_length 0.12 m) on its "
      "side, its root part turned (strength 0.7, span_mm 25) to flow away from the line. Then delete the short crossing "
      "tufts (studio_hairline_check flow outliers) — the planted guides now cover the scalp.",
      obj({"line": {"type": "array"}, "spacing_mm": {"type": "number"}, "offsets_mm": {"type": "array"},
           "sides": {"type": "string", "enum": ["both", "plus", "minus"]}, "min_length": {"type": "number"},
           "strength": {"type": "number"}, "span_mm": {"type": "number"}, **SHOW}, ["line"]))
def t_plant(a):
    return edit(a, {"do": "plant_parting", **{k: a[k] for k in ("line", "spacing_mm", "offsets_mm", "sides", "min_length",
                                                              "strength", "span_mm") if k in a}})


@tool("studio_hairline_fill", "Fuller, cleaner hairline from GUIDES only (no cards): along the painted hairline (or the vanilla "
      "card footprint edge when the head has no painted scalp) plant rows (2, max 4) of new guides at offsets_mm "
      "([2,5,8,11] mm inside the line, first `rows` used) every spacing_mm (5), in part front|left_temple|right_temple|"
      "temples|ears|sides|all. Each copies the shape / flow of the nearest guide (min_length 0.06 m, within template_mm 25, "
      "same side of the head, preferring guides rooted farther inside), shortened toward the edge (taper 0.6 = outer row "
      "length factor), hugging the scalp (lift_mm 2, span_mm 25). Copies over the eyes / face (below brow + brow_clear_mm "
      "12), on the ears (box + ear_pad_mm 5), through the head or sticking up are rejected; roots already within "
      "covered_mm of a guide are skipped. New roots past the growth-mesh edge get growth faces fanned onto it, so their "
      "hair renders. straighten: true plants along a smoothed line (local mean over smooth_mm 12). Budget: information only, "
      "never refuses or drops guides. list_only: "
      "report the planned roots without planting. Then studio_compare front + both 3/4 sides.",
      obj({"part": {"type": "string", "enum": ["all", "front", "left_temple", "right_temple", "temples", "ears", "sides"]},
           "rows": {"type": "integer"}, "offsets_mm": {"type": "array"}, "spacing_mm": {"type": "number"},
           "min_length": {"type": "number"}, "taper": {"type": "number"}, "lift_mm": {"type": "number"},
           "span_mm": {"type": "number"}, "template_mm": {"type": "number"}, "straighten": {"type": "boolean"},
           "smooth_mm": {"type": "number"}, "max_new": {"type": "integer"}, "max_ratio": {"type": "number"},
           "covered_mm": {"type": "number"}, "brow_clear_mm": {"type": "number"}, "ear_pad_mm": {"type": "number"},
           "front_deg": {"type": "number"}, "temple_deg": {"type": "number"}, "ear_deg": {"type": "number"},
           "edge_faces": {"type": "boolean", "description": "default true: new roots past the growth-mesh edge get growth faces (false = split the nearest face only)"},
           "ignore_budget": {"type": "boolean", "description": "no-op since 2026-10-15 (the budget never blocks)"},
           "list_only": {"type": "boolean"}, **SHOW}))
def t_hairline_fill(a):
    return edit(a, {"do": "hairline_fill", **{k: a[k] for k in (
        "part", "rows", "offsets_mm", "spacing_mm", "min_length", "taper", "lift_mm", "span_mm", "template_mm", "straighten",
        "smooth_mm", "max_new", "max_ratio", "covered_mm", "brow_clear_mm", "ear_pad_mm", "front_deg", "temple_deg", "ear_deg",
        "edge_faces", "ignore_budget", "list_only") if k in a}})


@tool("studio_free_budget", "Explicit tool only (never called automatically; the budget no longer blocks anything since 2026-10-15): thins / merges guides to lower the budget ratio. "
      "The budget counts 64 x density hairs per GROWTH FACE whatever its size, so deleting an interior guide (its hole is "
      "re-fanned: 2 faces fewer) frees what one planted guide costs. (1) merge: guides whose root is within merge_mm (2.5) "
      "of an earlier guide's root and whose shape is within shape_mm (6, mean point distance) go (copies left by fills); "
      "(2) thin: interior guides (closed ring of faces - never the growth-mesh edge / hairline) in the densest areas "
      "(mean incident face area < dense_factor 0.8 x median, smallest first, never two neighbours per pass) go until "
      "target_ratio (1.10) or max_delete (150). protect (selector) / keep_line [[p0],[p1]] + keep_line_mm (15) keep "
      "guides (a parting you are about to plant). dry_run first; compare after (thinning a fringe shows).",
      obj({"where": WHERE, "target_ratio": {"type": "number"}, "merge": {"type": "boolean"}, "thin": {"type": "boolean"},
           "merge_mm": {"type": "number"}, "shape_mm": {"type": "number"}, "dense_factor": {"type": "number"},
           "max_delete": {"type": "integer"}, "protect": WHERE, "keep_line": {"type": "array"},
           "keep_line_mm": {"type": "number"}, **SHOW}))
def t_free_budget(a):
    return edit(a, {"do": "free_budget", **{k: a[k] for k in ("where", "target_ratio", "merge", "thin", "merge_mm", "shape_mm",
                                                             "dense_factor", "max_delete", "protect", "keep_line",
                                                             "keep_line_mm") if k in a}})


@tool("studio_fill_below_hairline", "Plant guides on bald skin BELOW the painted hairline where the vanilla cards put hair "
      "(receding temples - fill_scalp only sees the painted scalp). Candidate skin = head skin off the painted scalp, "
      "within below_mm (30) of it, under visible card hair (reach_mm 10, alpha_vis 0.3), with no strand lying on it "
      "(cover_mm 3 lateral, cover_height_mm 12; interp: interpolated strands count too); planted every spacing_mm (5), "
      "nearest the scalp first, as copies of a guide within template_mm (35, length >= min_length 0.04, same side) "
      "rotated about the skull centre, root on the skin, length_scale / max_length, root part hugging the skin (span_mm "
      "20, lift_mm 1.5). Rejected: through the head, sticking up (tuft_mm 12), over the face / eyes, on the ears. New "
      "roots get growth faces fanned onto the mesh edge. side left|right|both, where {near, radius} / {z_min, z_max}. "
      "Budget: information only, never refuses or drops. list_only: plan only. allow_temple (opt-in 2026-10-17): the over-face test lets the temple skin pass (lateral of the outer eye corners + temple_inset_mm 8, above the eye centre + temple_min_z_mm 0; eyes / brows still rejected); face_ok_box [x0,x1,y0,y1,z0,z1] (edit frame, not mirrored) passes too.",
      obj({"allow_temple": {"type": "boolean"}, "temple_inset_mm": {"type": "number"}, "temple_min_z_mm": {"type": "number"}, "face_ok_box": {"type": "array", "items": {"type": "number"}}, "side": {"type": "string", "enum": ["both", "left", "right"]}, "where": WHERE, "spacing_mm": {"type": "number"},
           "below_mm": {"type": "number"}, "reach_mm": {"type": "number"}, "cover_mm": {"type": "number"},
           "cover_height_mm": {"type": "number"}, "interp": {"type": "boolean"}, "min_length": {"type": "number"},
           "template_mm": {"type": "number"}, "length_scale": {"type": "number"}, "max_length": {"type": "number"},
           "lift_mm": {"type": "number"}, "span_mm": {"type": "number"}, "tuft_mm": {"type": "number"},
           "alpha_vis": {"type": "number"}, "brow_clear_mm": {"type": "number"}, "ear_pad_mm": {"type": "number"},
           "max_new": {"type": "integer"}, "max_ratio": {"type": "number"}, "ignore_budget": {"type": "boolean"},
           "shots": {"type": "array", "items": {"type": "integer"}, "description": "RECOMMENDED: closed loop - bare skin = the flat grey skin pixels SEEN in the scalp-overlay render of these cameras (or one shot / view / eye camera), ray-cast to the skin; replaces the strand-cover test (which misses bare skin between strands)"},
           "sample_px": {"type": "integer"},
           "list_only": {"type": "boolean"}, **SHOW}))
def t_fill_below(a):
    cmd = {"do": "fill_below_hairline", **{k: a[k] for k in (
        "side", "where", "spacing_mm", "below_mm", "reach_mm", "cover_mm", "cover_height_mm", "interp", "min_length",
        "template_mm", "length_scale", "max_length", "lift_mm", "span_mm", "tuft_mm", "alpha_vis", "brow_clear_mm",
        "ear_pad_mm", "max_new", "max_ratio", "ignore_budget", "shots", "sample_px", "list_only",
        "allow_temple", "temple_inset_mm", "temple_min_z_mm", "face_ok_box") if k in a}}
    cmd.update(cam_args(a))   # a camera = closed loop on that render (shot / view / eye)
    return edit(a, cmd)


@tool("studio_plant_flow", "Opt-in, Studio-only (2026-10-15): plant NEW guides on bare skin with a GIVEN flow instead of "
      "copying neighbours (hairline_fill / fill_below_hairline copy the nearest guide, e.g. a hanging front lock). Seeds: "
      "points [[x,y,z],..] (edit frame, snapped to the skin), or shots [ids] = the skin SEEN bare in those cameras' scalp "
      "overlay, or area samples of the head skin with no strand over it (uncovered_only, cover_mm 3); limit with box "
      "[x0,x1,y0,y1,z0,z1] (edit frame), near [x,y,z] + radius (m), side left|right|both. Poisson at spacing_mm (5), at "
      "least clear_mm from existing roots, planted nearest the existing hair first (growth faces chain outward). Each "
      "strand walks ALONG THE SKIN from its root in direction [x,y,z] (given for the +x side, x mirrored on the other side "
      "unless mirror_x false; default = the mean root direction of guides within flow_mm 30) for walk_mm (40), lift "
      "base_mm (1) -> lift_mm (2) above the skin, then joins the nearest existing guide running the same way within "
      "join_mm (30): its remaining shape, the gap fading over fade_mm (40) to fade_keep (0.5) of it, so copies do not stack on "
      "one guide (join false = walk only; never joins a guide hanging over the face, nor join_exclude; join_spread_mm (0) "
      "adds that much distance per earlier join onto the same guide, so a dense row spreads over several tails instead of "
      "bundling into one); length (m) caps it. "
      "Pushed out of the head (standoff_mm 2); over the face / eyes or on the ears rejected. uv [u,v] = texel of the new "
      "growth faces (see studio_face_texel). over_ear: roots in front of / below the ear top go up (reaching that height over_ear_front_mm (4) in front of the ear) "
      "to over_ear_clear_mm (9) above the ear, over it to over_ear_behind_mm (10) behind it (layered: higher / farther-forward roots pass up to over_ear_spread_mm (14) higher, plus over_ear_keep (0; 0.5 = "
      "parallel sweep) of the root's height above ear top - over_ear_base_mm (20; 0 with keep 1 = roots above the ear go straight "
      "back, only lower ones rise); over_ear_all: also roots above the clearance line, so they pass behind the ear before "
      "joining), then walk in direction and join (temple / sideburn hair "
      "swept back over the ear); stop_at_ear_mm (off): the walk ends that far before the ear (short flat sideburn hair "
      "swept back toward the ear, use with join false); max_turn_deg (off): reject strands whose summed bending exceeds it (kinked / looped short walks; 300 is a safe value); new faces over an ear are dropped (drop_ear_faces, on with over_ear; face_max_mm drops "
      "longer-edged new faces). ear_box [x0,x1,y0,y1,z0,z1] (the +x ear, mirrored) replaces the automatic ear boxes "
      "(too big on some heads: check ear_boxes in the result). Budget: information only, never refuses. list_only: plan only. allow_temple (opt-in 2026-10-17): the over-face test lets the temple skin pass (lateral of the outer eye corners + temple_inset_mm 8, above the eye centre + temple_min_z_mm 0; eyes / brows still rejected); face_ok_box [x0,x1,y0,y1,z0,z1] (edit frame, not mirrored) passes too. points_exact (opt-in 2026-10-17): every explicit point is projected onto the skin surface and kept as its own seed (no clear_mm / spacing_mm thinning).",
      obj({"points": {"type": "array"}, "points_exact": {"type": "boolean"}, "allow_temple": {"type": "boolean"}, "temple_inset_mm": {"type": "number"}, "temple_min_z_mm": {"type": "number"}, "face_ok_box": {"type": "array", "items": {"type": "number"}}, "shots": {"type": "array"}, "box": {"type": "array", "items": {"type": "number"}},
           "near": {"type": "array", "items": {"type": "number"}}, "radius": {"type": "number"},
           "side": {"type": "string", "enum": ["both", "left", "right"]}, "direction": {"type": "array", "items": {"type": "number"}},
           "mirror_x": {"type": "boolean"}, "spacing_mm": {"type": "number"}, "clear_mm": {"type": "number"},
           "walk_mm": {"type": "number"}, "step_mm": {"type": "number"}, "base_mm": {"type": "number"}, "lift_mm": {"type": "number"},
           "join": {"type": "boolean"}, "join_mm": {"type": "number"}, "fade_mm": {"type": "number"}, "fade_keep": {"type": "number"}, "join_exclude": WHERE, "join_spread_mm": {"type": "number"}, "length": {"type": "number"},
           "over_ear": {"type": "boolean"}, "ear_box": {"type": "array", "items": {"type": "number"}}, "over_ear_clear_mm": {"type": "number"}, "over_ear_behind_mm": {"type": "number"}, "over_ear_spread_mm": {"type": "number"}, "over_ear_front_mm": {"type": "number"}, "over_ear_keep": {"type": "number"}, "over_ear_base_mm": {"type": "number"}, "over_ear_all": {"type": "boolean"}, "stop_at_ear_mm": {"type": "number"}, "max_turn_deg": {"type": "number"},
           "drop_ear_faces": {"type": "boolean"}, "face_max_mm": {"type": "number"},
           "min_length": {"type": "number"}, "standoff_mm": {"type": "number"}, "flow_mm": {"type": "number"},
           "uncovered_only": {"type": "boolean"}, "cover_mm": {"type": "number"}, "cover_height_mm": {"type": "number"},
           "ear_pad_mm": {"type": "number"}, "brow_clear_mm": {"type": "number"}, "uv": {"type": "array"},
           "sample_px": {"type": "integer"}, "max_new": {"type": "integer"}, "max_ratio": {"type": "number"},
           "ignore_budget": {"type": "boolean"}, "list_only": {"type": "boolean"}, **SHOW}))
def t_plant_flow(a):
    keys = ("points", "shots", "box", "near", "radius", "side", "direction", "mirror_x", "spacing_mm", "clear_mm", "walk_mm",
            "step_mm", "base_mm", "lift_mm", "join", "join_mm", "fade_mm", "fade_keep", "join_exclude", "join_spread_mm", "length", "over_ear", "ear_box", "over_ear_clear_mm", "over_ear_behind_mm", "over_ear_spread_mm", "over_ear_front_mm", "over_ear_keep", "over_ear_base_mm", "over_ear_all", "stop_at_ear_mm", "max_turn_deg",
            "drop_ear_faces", "face_max_mm", "min_length", "standoff_mm", "flow_mm",
            "uncovered_only", "cover_mm", "cover_height_mm", "ear_pad_mm", "brow_clear_mm", "uv", "sample_px", "max_new",
            "max_ratio", "ignore_budget", "list_only", "points_exact", "allow_temple", "temple_inset_mm", "temple_min_z_mm", "face_ok_box")
    return edit(a, {"do": "plant_flow", **{k: a[k] for k in keys if k in a}})


@tool("studio_face_texel", "Opt-in, Studio-only (2026-10-15): per-area colour / density through the growth-face uv. Every "
      "texture-driven fur value (root / tip colour, density, root / tip width, clump, wave) is read at the face uv; this "
      "moves the corners of the guides in 'where' to the texel whose values best match the targets, all other maps held: "
      "root_brightness / tip_brightness (factor on the colour maps), density (factor on the density map: < 1 thins / "
      "feathers an edge), width (factor on the root width map). corners guide (default: only the selected guides' corners, "
      "so the hair blends into the neighbours) | face. The reachable range is what the fur's own maps hold (reported: "
      "texel_before / target / chosen). list_only: report only. Needs a fur with texture maps.",
      obj({"where": WHERE, "root_brightness": {"type": "number"}, "tip_brightness": {"type": "number"},
           "density": {"type": "number"}, "width": {"type": "number"},
           "corners": {"type": "string", "enum": ["guide", "face"]}, "grid": {"type": "integer"},
           "list_only": {"type": "boolean"}, **SHOW}, ["where"]))
def t_face_texel(a):
    keys = ("where", "root_brightness", "tip_brightness", "density", "width", "corners", "grid", "list_only")
    return edit(a, {"do": "face_texel", **{k: a[k] for k in keys if k in a}})


@tool("studio_soften_hairline", "Opt-in, Studio-only (2026-10-15): soften a square / blunt planted hairline without deleting "
      "anything. Row 1 = guides on the open edge of the growth mesh inside where (default front), row 2 = their face "
      "neighbours (rows 2): their growth-face texels get density [0.35, 0.7] and root width [0.6, 0.85] of the current "
      "texel (as studio_face_texel, corners guide, so it fades across the faces), and row-1 roots move jitter_mm (3) back or "
      "forward along their flow (seed) for an irregular line. list_only: rows only.",
      obj({"where": WHERE, "rows": {"type": "integer"}, "density": {"type": "array", "items": {"type": "number"}},
           "width": {"type": "array", "items": {"type": "number"}}, "jitter_mm": {"type": "number"}, "seed": {"type": "integer"},
           "list_only": {"type": "boolean"}, **SHOW}))
def t_soften_hairline(a):
    return edit(a, {"do": "soften_hairline", **{k: a[k] for k in ("where", "rows", "density", "width", "jitter_mm", "seed", "list_only") if k in a}})


@tool("studio_fade_hairline", "Opt-in, Studio-only (2026-10-15): make a hairline GROW out of the skin (sparse fine "
      "strands at the very edge, denser over a few mm, no stair steps / cut-off blocks). HairWorks samples the fur maps "
      "per hair at its uv interpolated across the growth face, so the corners of the edge rows (rows (3) face rings from "
      "the visible hairline of the growth mesh, inside where: an open edge with cover_min (2) roots within probe_radius_mm 5 "
      "or guides lying within cover_mm 5, at probe_mm 6 beyond it is a hole inside the hair / the nape under long hair and is "
      "left alone; cover_exclude = guides not counted as cover, e.g. front locks over the forehead) are put on a monotone DENSITY RAMP of the fur's own density map "
      "(the vanilla hairline fade): searched over the whole map (every stride_px 3) for a monotone ramp of at least "
      "min_ramp_px (8) down to density low (12), reached from the nearest interior guide's texel by a clean uv segment, "
      "with the other maps held within colour_tol 30 / width_tol 40 / clump_tol 60 / wave_tol 60; "
      "row k takes density[k] x the interior density (default [0.06, 0.39, 0.72]). The root part (flat_mm 6) of rows "
      "1..flat_rows (2) is laid onto the skin (lift_mm 0.6) so no stub stands out. Reports each group's ramp and texels. "
      "Guides and faces stay. Use instead of soften_hairline / feather_hairline (their far-off texels and deleted faces "
      "make speckles and saw teeth). list_only: report only.",
      obj({"where": WHERE, "rows": {"type": "integer"}, "density": {"type": "array", "items": {"type": "number"}},
           "inner_mm": {"type": "number"}, "stride_px": {"type": "integer"}, "min_ramp_px": {"type": "number"}, "low": {"type": "number"}, "probe_mm": {"type": "number"}, "probe_radius_mm": {"type": "number"}, "cover_mm": {"type": "number"}, "cover_min": {"type": "integer"}, "relax": {"type": "number"}, "facing": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}}, "facing_deg": {"type": "number"}, "extend_mm": {"type": "number"}, "extend_density": {"type": "number"}, "extend_length": {"type": "number"}, "extend_flat_mm": {"type": "number"}, "extend_lift_mm": {"type": "number"}, "flat_first_lift_mm": {"type": "number"}, "cover_exclude": WHERE,
           "colour_tol": {"type": "number"}, "width_tol": {"type": "number"}, "clump_tol": {"type": "number"},
           "wave_tol": {"type": "number"}, "flat_mm": {"type": "number"}, "lift_mm": {"type": "number"},
           "flat_rows": {"type": "integer"}, "list_only": {"type": "boolean"}, **SHOW}))
def t_fade_hairline(a):
    keys = ("where", "rows", "density", "inner_mm", "stride_px", "min_ramp_px", "low", "probe_mm", "probe_radius_mm", "cover_mm", "cover_min", "relax", "facing", "facing_deg", "extend_mm", "extend_density", "extend_length", "extend_flat_mm", "extend_lift_mm", "flat_first_lift_mm", "cover_exclude", "colour_tol", "width_tol", "clump_tol", "wave_tol",
            "flat_mm", "lift_mm", "flat_rows", "list_only")
    return edit(a, {"do": "fade_hairline", **{k: a[k] for k in keys if k in a}})


@tool("studio_centre_parting", "Shape a parting line and comb the hair away from it on both sides. line [[p0],[p1]] "
      "(edit frame, front point first; studio_pick scalp_edit), or automatic: the parting x from the vanilla cards (the "
      "thinnest column of card hair over the front top, search_mm 25 around the head's mid plane; x / x_offset_mm "
      "override; from_cards false = mid plane), from the painted front hairline back over the top for length_mm (110). "
      "Guides rooted within band_mm (50) of the line: roots closer than gap_mm/2 (2.5) move out to the gap edge, the root "
      "part (span_mm 35) turns away from the line (strength 0.9, weaker farther out; only the across-line part of the "
      "flow changes), points still across the line are pushed back (clear_mm 2), roots within hug_mm (12) lie flat "
      "(lift_mm 2); growth faces spanning the line are removed (split_faces) so no hair is interpolated across it. "
      "mode lay (default): the first lay_mm (50; x lay_far 0.4 at the band edge) of each strand is walked ALONG THE "
      "SKIN in the combed direction (away from the line + back x the line direction, back 0.5), the rest rejoins its "
      "old path over rejoin_mm (50) (fringe strands whose old path runs over the forehead / face are laid whole); each "
      "step is steered (steer 0.35) toward away-from-line + toward the back of the head; mode turn: only the root part (span_mm) turns (long fringes keep hanging over the face). "
      "t_min / t_max (-0.05 / 1.1) = extent along the line. list_only: report only. Compare top + front after.",
      obj({"line": {"type": "array"}, "x": {"type": "number"}, "x_offset_mm": {"type": "number"},
           "from_cards": {"type": "boolean"}, "search_mm": {"type": "number"}, "length_mm": {"type": "number"},
           "front_mm": {"type": "number"}, "band_mm": {"type": "number"}, "gap_mm": {"type": "number"},
           "strength": {"type": "number"}, "span_mm": {"type": "number"}, "clear_mm": {"type": "number"},
           "hug_mm": {"type": "number"}, "lift_mm": {"type": "number"}, "max_turn_deg": {"type": "number"},
           "split_faces": {"type": "boolean"}, "t_min": {"type": "number"}, "t_max": {"type": "number"},
           "mode": {"type": "string", "enum": ["lay", "turn"]}, "lay_mm": {"type": "number"}, "lay_far": {"type": "number"},
           "back": {"type": "number"}, "split_mm": {"type": "number"}, "rejoin_mm": {"type": "number"}, "steer": {"type": "number"},
           "where": WHERE, "list_only": {"type": "boolean"}, **SHOW}))
def t_centre_parting(a):
    return edit(a, {"do": "centre_parting", **{k: a[k] for k in (
        "line", "x", "x_offset_mm", "from_cards", "search_mm", "length_mm", "front_mm", "band_mm", "gap_mm", "strength",
        "span_mm", "clear_mm", "hug_mm", "lift_mm", "max_turn_deg", "split_faces", "t_min", "t_max", "where",
        "list_only", "mode", "lay_mm", "lay_far", "back", "split_mm", "rejoin_mm", "steer") if k in a}})


@tool("studio_regrow", "Opt-in, Studio-only: rebuild_from_cards (every studio_rebuild_from_cards key works) PLUS exclusion zones on "
      "the new roots and ear routing. exclude_ears (roots in the ear box + ear_pad_mm 8 dropped), exclude [{near [x,y,z], "
      "radius_mm}] (temples...), hairline_inset_mm (roots within this of unpainted skin dropped; inset_front_only true = "
      "only in front of the head centre), max_zigzag (guides with length / chord above it - folded chained lanes - are "
      "replaced by a rotated copy of the nearest straight neighbour; 2.5), smooth_passes (0), ear_mode behind (default: "
      "strand points passing in front of / over an ear are moved behind its back edge + behind_mm 8, lengths kept) | "
      "drop (also delete strands still crossing the ear) | keep. Faces of dropped roots go: hair recedes there.",
      obj({"where": WHERE, "exclude_ears": {"type": "boolean"}, "ear_pad_mm": {"type": "number"}, "behind_mm": {"type": "number"},
           "ear_mode": {"type": "string", "enum": ["behind", "drop", "keep"]}, "exclude": {"type": "array"},
           "hairline_inset_mm": {"type": "number"}, "inset_front_only": {"type": "boolean"}, "max_zigzag": {"type": "number"},
           "smooth_passes": {"type": "integer"}, "spacing_mm": {"type": "number"}, "budget_target": {"type": "number"},
           "hug": {"type": "number"}, "layer": {"type": "string"}, "ear_lat_mm": {"type": "number"}, "ear_top_mm": {"type": "number"},
           "ear_box": {"type": "array"}, **SHOW}))
def t_regrow(a):
    return edit(a, {"do": "regrow", **{k: v for k, v in a.items() if k not in ("show", "dry_run", "measure", "crop", "max_px", "size") and k not in CAM}})


@tool("studio_route_behind_ears", "Opt-in: strand points of the selected guides that pass in front of / over an ear are moved "
      "behind the ear's back edge (+ behind_mm 8, ear_pad_mm 8 margin), smoothed, lengths kept. ear_mode drop also deletes "
      "guides still crossing the ear; ear_mode split: strands entering the ear zone in front of the ear's middle "
      "(+ split_bias_mm) go IN FRONT of it (behind_mm = gap), the rest behind, and growth faces mixing the two sides are "
      "deleted (HairWorks would interpolate hair across the ear) - the one that actually frees a buried ear. Reports the ear boxes. Long / pointed ears (Eredin): ear_lat_mm (e.g. 30; default "
      "box = head vertices within 12 mm of the most lateral point) + ear_top_mm (50, above the eyes), or ear_box "
      "[[lo],[hi]] (character's left ear, mirrored) - same keys in studio_regrow.",
      obj({"where": WHERE, "ear_pad_mm": {"type": "number"}, "behind_mm": {"type": "number"},
           "ear_mode": {"type": "string", "enum": ["behind", "drop", "split"]}, "split_bias_mm": {"type": "number"}, "passes": {"type": "integer", "description": "split: routing passes (4)"}, "ear_lat_mm": {"type": "number"},
           "ear_top_mm": {"type": "number"}, "ear_box": {"type": "array"}, "list_only": {"type": "boolean"}, **SHOW}))
def t_route_ears(a):
    return edit(a, {"do": "route_behind_ears", **{k: a[k] for k in ("where", "ear_pad_mm", "behind_mm", "ear_mode", "split_bias_mm", "passes", "ear_lat_mm",
                                                                   "ear_top_mm", "ear_box", "list_only") if k in a}})


@tool("studio_unzigzag", "Opt-in: guides that fold back on themselves (length / root-tip chord > max_zigzag 1.5 - chained card "
      "lanes, Eredin's sides) are replaced by a rotated copy of the nearest straight guide (chord ratio < straight 1.25, "
      "longer than 4 cm, avoid_ears true = not crossing an ear box + ear_pad_mm 4) rooted within radius_mm (30); their "
      "growth faces stay. drop_unmatched deletes zigzags with no template. ear_lat_mm / ear_top_mm / ear_box as in "
      "studio_route_behind_ears. list_only = count only.",
      obj({"where": WHERE, "max_zigzag": {"type": "number"}, "straight": {"type": "number"}, "radius_mm": {"type": "number"},
           "avoid_ears": {"type": "boolean"}, "ear_pad_mm": {"type": "number"}, "drop_unmatched": {"type": "boolean"},
           "ear_lat_mm": {"type": "number"}, "ear_top_mm": {"type": "number"}, "ear_box": {"type": "array"},
           "list_only": {"type": "boolean"}, **SHOW}))
def t_unzigzag(a):
    keys = ("where", "max_zigzag", "straight", "radius_mm", "avoid_ears", "ear_pad_mm", "drop_unmatched", "ear_lat_mm",
            "ear_top_mm", "ear_box", "list_only")
    return edit(a, {"do": "unzigzag", **{k: a[k] for k in keys if k in a}})


@tool("studio_hairnet", "Opt-in HOOD fix: every strand point (past root_part 0) farther than max_mm (8) from the skin is pulled "
      "back to max_mm (x strength 1); below eye level + z_lo_mm (-20) the limit grows by slope (0.6) per metre of drop so "
      "long hair still hangs free; points over the face are skipped (skip_face). Smoothed, lengths kept (spare length "
      "slides down). For grooms puffing out above / around the ears where vanilla lies flat (Eredin).",
      obj({"where": WHERE, "max_mm": {"type": "number"}, "slope": {"type": "number"}, "z_lo_mm": {"type": "number"},
           "root_part": {"type": "number"}, "strength": {"type": "number"}, "skip_face": {"type": "boolean"},
           "x_abs_min_mm": {"type": "number", "description": "only guides rooted at least this far from the mid plane (sides; keeps the front-centre hairline cover)"},
           "list_only": {"type": "boolean"}, **SHOW}))
def t_hairnet(a):
    keys = ("where", "max_mm", "slope", "z_lo_mm", "root_part", "strength", "skip_face", "x_abs_min_mm", "list_only")
    return edit(a, {"do": "hairnet", **{k: a[k] for k in keys if k in a}})


@tool("studio_headband", "Generic: detect a HEADBAND / circlet / ribbon (a thin closed ring around the head in an outfit or card mesh: small height, "
      "wide azimuth coverage, close to the skull) and push the strand points that pass OVER it to sit just UNDER its inner surface "
      "(clearance_mm 1.5, standoff_mm 1.0 above the skin, passes 4), faded in / out above and below (blend_mm, auto) so strands stay continuous; lengths kept, roots unchanged. "
      "Reports the detected band, candidates, and strands crossing before / after (list_only: detect + count only). "
      "mesh = name / path substring to force a part, z_range [lo,hi] absolute height window, max_height_mm 60, min_coverage 0.6.",
      obj({"where": WHERE, "mesh": {"type": "string"}, "z_range": {"type": "array", "items": {"type": "number"}},
           "max_height_mm": {"type": "number"}, "min_coverage": {"type": "number"}, "clearance_mm": {"type": "number"}, "standoff_mm": {"type": "number"}, "passes": {"type": "integer"},
           "blend_mm": {"type": "number"}, "list_only": {"type": "boolean"}, **SHOW}))
def t_headband(a):
    keys = ("where", "mesh", "z_range", "max_height_mm", "min_coverage", "clearance_mm", "standoff_mm", "passes", "blend_mm", "list_only")
    return edit(a, {"do": "headband", **{k: a[k] for k in keys if k in a}})


@tool("studio_layer_lift", "Opt-in CLAY-SHELL breaker: every selected guide gets its own lift off the skin (lift_min_mm 0 .. "
      "lift_max_mm 8, skewed low by gamma 1.5) and a slow sideways sway (sway_mm 3, waves 1 along the strand), both ramping "
      "in from the root over ramp (0.35 of the length) and fading out from fade_from (1 = never) over fade_len (0.2), so buns / tied ends stay; lengths kept, seed varies the pattern. Guides at different heights "
      "overlap and shade each other: layers / strand groups instead of one fused surface (Triss vanilla).",
      obj({"where": WHERE, "lift_min_mm": {"type": "number"}, "lift_max_mm": {"type": "number"}, "gamma": {"type": "number"},
           "sway_mm": {"type": "number"}, "waves": {"type": "number"}, "ramp": {"type": "number"}, "seed": {"type": "integer"},
           "fade_from": {"type": "number"}, "fade_len": {"type": "number"},
           "list_only": {"type": "boolean"}, **SHOW}))
def t_layer_lift(a):
    keys = ("where", "lift_min_mm", "lift_max_mm", "gamma", "sway_mm", "waves", "ramp", "seed", "fade_from", "fade_len", "list_only")
    return edit(a, {"do": "layer_lift", **{k: a[k] for k in keys if k in a}})


@tool("studio_inflate", "Opt-in fuller bun / tail: the points (not roots) of the selected guides within radius_mm (50) of "
      "centre [x,y,z] move away from it by scale (1.3), smooth falloff to the radius; guides get longer, so the mass grows "
      "instead of thinning.",
      obj({"where": WHERE, "centre": {"type": "array", "items": {"type": "number"}}, "radius_mm": {"type": "number"},
           "scale": {"type": "number"}, "list_only": {"type": "boolean"}, **SHOW}, ["centre"]))
def t_inflate(a):
    keys = ("where", "centre", "radius_mm", "scale", "list_only")
    return edit(a, {"do": "inflate", **{k: a[k] for k in keys if k in a}})


@tool("studio_twist_roll", "Opt-in, Studio-only: gather guides into a ROLLED TWIST along a path (French-twist / nape roll ending "
      "at a point, e.g. Iris's nape roll ending at the rose). path [[x,y,z],...] start -> end (fit_cards true snaps it to "
      "the vanilla roll cards within fit_mm 25). Each guide: root -> along the skin to the roll (approach 'walk' (default) "
      "steps along the scalp toward the roll; 'chord' = straight chord pushed out of the skin, which bares the crown; attach = nearest axis "
      "point, or attach 'azimuth' = the axis point at the root's own angle around the head, i.e. combed straight down; "
      "az_centre_fwd_mm moves the angle centre forward so the crown combs back instead of splitting; "
      "attach_shift_mm), then a helix around the axis (radius_mm, default from the cards, x radius_scale; layer "
      "0.35 radius spread; pitch_mm 45; hand +1/-1) toward the end; at the end it spirals in (end_turns 0.6, tuck_r 0.4) and "
      "is cut end_mm (10) later. curl_mm / curl_turns add a ringlet wobble. Guides too short to reach the roll are left, or with "
      "extend_short lengthened to roll min_roll_mm (40) (approach up to max_approach_mm 200). "
      "over_ears (opt-in): a root in front of the ear whose roll entry is behind it walks via a point ear_pad_mm (8) above "
      "the ear top (ear_box / ear_lat_mm as in regrow) instead of through the ear. "
      "hold (true): stiffness map 1 on the rolled guides + the make_bun global hold (stiffness / strength / root / bend 1, "
      "flat curves); the result is settled. The base guide's per-point fur-vs-edit offset is dropped from rolled strands "
      "(keep_fur_delta true keeps it; on Iris it bent the roll into hanging loops).",
      obj({"where": WHERE, "path": {"type": "array"}, "fit_cards": {"type": "boolean"}, "fit_mm": {"type": "number"},
           "fit_off_mm": {"type": "number"}, "radius_mm": {"type": "number"}, "radius_scale": {"type": "number"},
           "layer": {"type": "number"}, "pitch_mm": {"type": "number"}, "hand": {"type": "number"},
           "standoff_mm": {"type": "number"}, "end_turns": {"type": "number"}, "tuck_r": {"type": "number"},
           "end_mm": {"type": "number"}, "curl_mm": {"type": "number"}, "curl_turns": {"type": "number"},
           "attach_shift_mm": {"type": "number"}, "attach": {"type": "string", "enum": ["nearest", "azimuth"]},
           "extend_short": {"type": "boolean"}, "az_centre_fwd_mm": {"type": "number"}, "approach": {"type": "string", "enum": ["walk", "chord"]}, "keep_fur_delta": {"type": "boolean"}, "min_roll_mm": {"type": "number"}, "max_approach_mm": {"type": "number"},
           "over_ears": {"type": "boolean"}, "ear_pad_mm": {"type": "number"}, "ear_box": {"type": "array"}, "ear_lat_mm": {"type": "number"}, "ear_top_mm": {"type": "number"},
           "hold": {"type": "boolean"}, "tip_stiffness": {"type": "number"},
           "list_only": {"type": "boolean"}, **SHOW}, ["path"]))
def t_twist_roll(a):
    keys = ("where", "path", "fit_cards", "fit_mm", "fit_off_mm", "radius_mm", "radius_scale", "layer", "pitch_mm", "hand",
            "standoff_mm", "end_turns", "tuck_r", "end_mm", "curl_mm", "curl_turns", "attach_shift_mm", "attach", "az_centre_fwd_mm", "approach", "keep_fur_delta", "extend_short", "min_roll_mm", "max_approach_mm",
            "over_ears", "ear_pad_mm", "ear_box", "ear_lat_mm", "ear_top_mm", "hold", "tip_stiffness", "list_only")
    return edit(a, {"do": "twist_roll", **{k: a[k] for k in keys if k in a}})


@tool("studio_plant_fringe", "Opt-in, Studio-only: NEW short guides (with growth faces) from the front hairline lying on the "
      "forehead: a subtle fringe / face-framing locks. Stations along the painted front hairline every spacing_mm (4) "
      "between x_range [x0, x1] (edit frame m; default centre +-3 cm), inset_mm (4) inside, or explicit roots. Each strand "
      "walks the skin down the forehead (standoff_mm 2.5) swept sideways by sweep (+ = toward +x; sweep_spread varies it "
      "across the stations), length_mm (60) +- length_jitter (0.25), stops brow_gap_mm (8) above the brows, tip lifted "
      "lift_tip_mm (4), wave_mm, up_mm (first part forward / up = a lifted lock). stiffness: map value for the new guides.",
      obj({"roots": {"type": "array"}, "x_range": {"type": "array", "items": {"type": "number"}}, "spacing_mm": {"type": "number"},
           "inset_mm": {"type": "number"}, "length_mm": {"type": "number"}, "length_jitter": {"type": "number"},
           "sweep": {"type": "number"}, "sweep_spread": {"type": "number"}, "standoff_mm": {"type": "number"},
           "lift_tip_mm": {"type": "number"}, "brow_gap_mm": {"type": "number"}, "wave_mm": {"type": "number"},
           "up_mm": {"type": "number"}, "root_bias": {"type": "number"}, "edge_mm": {"type": "number"}, "rmax_mm": {"type": "number"},
           "uv": {"type": "array"}, "stiffness": {"type": "number"}, "list_only": {"type": "boolean"}, **SHOW}))
def t_plant_fringe(a):
    keys = ("roots", "x_range", "spacing_mm", "inset_mm", "length_mm", "length_jitter", "sweep", "sweep_spread", "standoff_mm",
            "lift_tip_mm", "brow_gap_mm", "wave_mm", "up_mm", "root_bias", "edge_mm", "rmax_mm", "uv", "stiffness", "list_only")
    return edit(a, {"do": "plant_fringe", **{k: a[k] for k in keys if k in a}})


@tool("studio_parting_roots", "Opt-in, Studio-only: a clean PARTING without bare scalp. Line = line [[x,y,z],...] (front -> "
      "back, on the scalp) or automatic (x, length_mm 110 from the front painted hairline back along the mid plane). (1) "
      "comb (true): guides rooted within band_mm (25) whose root part crosses the line / does not go away from it are "
      "laid along the skin away from it (lay_mm 45, steer 0.5 toward their own flow, back 0.35 toward the back end), the "
      "rest rejoins over rejoin_mm (50); nothing stays across the line (gap_mm 1.5). (2) split (true): old growth faces "
      "spanning the line go. (3) NEW roots in rows_mm ([2.5, 6, 10]) on each side (sides both|plus|minus) every "
      "spacing_mm (5) along t_min..t_max, each a rotated copy of the nearest same-side guide laid away from the line; "
      "growth faces = Delaunay with the neighbours that never cross the line (edge_mm 18, rmax_mm 14, zone_mm 16). "
      "uv_mode nearest (default: uv of the old faces at the line) or uv [u,v]. list_only: plan only.",
      obj({"line": {"type": "array"}, "x": {"type": "number"}, "length_mm": {"type": "number"}, "spacing_mm": {"type": "number"},
           "rows_mm": {"type": "array", "items": {"type": "number"}}, "lay_mm": {"type": "number"}, "rejoin_mm": {"type": "number"},
           "band_mm": {"type": "number"}, "standoff_mm": {"type": "number"}, "steer": {"type": "number"}, "back": {"type": "number"},
           "t_min": {"type": "number"}, "t_max": {"type": "number"}, "min_length": {"type": "number"}, "gap_mm": {"type": "number"},
           "sides": {"type": "string", "enum": ["both", "plus", "minus"]}, "comb": {"type": "boolean"}, "split": {"type": "boolean"},
           "covered_mm": {"type": "number"}, "edge_mm": {"type": "number"}, "rmax_mm": {"type": "number"}, "zone_mm": {"type": "number"},
           "uv_mode": {"type": "string"}, "uv": {"type": "array"}, "no_face": {"type": "boolean"}, "list_only": {"type": "boolean"}, **SHOW}))
def t_parting_roots(a):
    keys = ("line", "x", "length_mm", "spacing_mm", "rows_mm", "lay_mm", "rejoin_mm", "band_mm", "standoff_mm", "steer", "back",
            "t_min", "t_max", "min_length", "gap_mm", "sides", "comb", "split", "covered_mm", "edge_mm", "rmax_mm", "zone_mm",
            "uv_mode", "uv", "no_face", "list_only")
    return edit(a, {"do": "parting_roots", **{k: a[k] for k in keys if k in a}})


@tool("studio_undo", "calls: n undoes the last n whole TOOL CALLS (use after multi-round tools such as clear_eye). Undo the last tool call (n times): one call = one group, even when it wrote many ops.", obj({"n": {"type": "integer"}, "calls": {"type": "integer"}}))
def t_undo(a):
    if a.get("calls") and not CALLS:
        raise RuntimeError("no tool call to undo in this session (opened projects' own edits are never undone by calls)")
    if a.get("calls"):
        # undo whole tool calls (a multi-round tool such as clear_eye adds several op groups)
        r = None
        for _ in range(int(a["calls"])):
            if not CALLS:
                break
            target = CALLS.pop()
            for _ in range(500):
                if S.call({"do": "status"})["status"]["ops"] <= target:
                    break
                r = S.call({"do": "undo", "n": 1})
        st = S.call({"do": "status"})
        st["undone_to_ops"] = st["status"]["ops"]
        st["calls_left"] = len(CALLS)
        return st
    return S.call({"do": "undo", "n": a.get("n", 1)})


@tool("studio_redo", "Redo (n times).", obj({"n": {"type": "integer"}}))
def t_redo(a):
    return S.call({"do": "redo", "n": a.get("n", 1)})


@tool("studio_reset_edits", "Drop every edit: back to the build as opened.", obj({}))
def t_reset(a):
    return S.call({"do": "reset_edits"})


# --- look, physics, lighting
@tool("studio_wind_natural", "2026-10-17 (FINDINGS 11.5 / 11.6): make the open groom move naturally in wind from near the root at a "
      "sane GPU cost. Measured on the Svanrige and Yennefer user saves: the upper half of every strand is held rigid by rootStiffness "
      "(and stiffness), not by stiffnessStrength; strength below 1 is what costs the GPU (15 -> 80-180 ms). Sets strength 1, "
      "rootStiffness root (0), stiffness (.9), tipStiffness 0, bendStiffness bend (.9) = the tuned recipe u1 (FINDINGS 11.6: 132 % of the "
      "Svanrige user save's motion, from the root; .3 / .3 = 250 %, wild); keeps everything else (curves, bone stiffness, "
      "collision, widths, colours). Returns the values before / after. Measure with tools/hairworks/toolarch/wind_motion.py.",
      obj({"root": {"type": "number", "description": "rootStiffness (0; .1-.2 for a little hold at the scalp)"},
           "stiffness": {"type": "number", "description": "stiffness (.9; .93 = the user save's amount of motion, .3 = 2.5x)"}, "bend": {"type": "number", "description": "bendStiffness (.9)"},
           "damping": {"type": "number", "description": "optional stiffnessDamping (.5) and simulation damping (.3) for less flutter"}}))
def t_wind_natural(a):
    S = "physicalMaterials.stiffness."
    keys = [S + k for k in ("stiffnessStrength", "rootStiffness", "stiffness", "tipStiffness", "bendStiffness", "stiffnessDamping")] + ["physicalMaterials.simulation.damping"]
    before = S_call_values(keys)
    sv = {S + "stiffnessStrength": 1.0, S + "rootStiffness": float(a.get("root", 0.0)), S + "stiffness": float(a.get("stiffness", 0.9)),
          S + "tipStiffness": 0.0, S + "bendStiffness": float(a.get("bend", 0.9))}
    if a.get("damping") is not None:
        sv[S + "stiffnessDamping"] = float(a["damping"]); sv["physicalMaterials.simulation.damping"] = 0.3
    r = t_lookset({"set": sv, "force": True})
    return {"before": before, "after": S_call_values(keys), "set": sv, "look": r}


def S_call_values(keys):
    return S.call({"do": "look_get", "keys": keys}).get("values", {})


@tool("studio_look_get", "Current look / physics values (fur parameter paths). keys: optional list of path prefixes.",
      obj({"keys": {"type": "array", "items": {"type": "string"}}}))
def t_lookget(a):
    return S.call({"do": "look_get", "keys": a.get("keys", [])})


@tool("studio_look_set", "Set look / physics values {param path: value}, e.g. graphicalMaterials.color.rootColor, "
      "physicalMaterials.stiffness.tipStiffness, graphicalMaterials.width.rootWidth, density... Caps are re-enforced.",
      obj({"set": {"type": "object"}, "force": {"type": "boolean", "description": "2026-10-17: also set a parameter the fur's own property pack "
           "does not carry (e.g. physicalMaterials.stiffness.stiffnessStrength on a vanilla hair fur: the UI slider sets it, look_get calls it unknown)"}, **SHOW}))
def t_lookset(a):
    sv = a.get("set")
    if sv is None:   # flat form {path: value, ...} accepted too
        sv = {k: v for k, v in a.items() if "." in k and k != "force"}
    if not sv:
        raise RuntimeError('nothing to set: use {"set": {"graphicalMaterials.color.rootColor": [r,g,b]}} (or the flat '
                           'form with dotted paths); got keys: ' + ", ".join(a.keys()))
    chk = S.call({"do": "look_get", "keys": list(sv.keys())}).get("values", {})
    bad = [k for k, v in chk.items() if isinstance(v, dict) and v.get("error")]
    if bad and a.get("force"):
        bad = [k for k in bad if not k.startswith(("physicalMaterials.", "graphicalMaterials.", "levelOfDetail."))]
    # 2026-10-17 (L593): a colour given as [r, g, b] (0-255) is stored in the UI's struct form; the Studio's preview reads only
    # the struct (hairsdk color()) and rendered a list-form colour as WHITE while the export / game used it correctly
    for k, v in list(sv.items()):
        if k.endswith("Color") and isinstance(v, list) and len(v) in (3, 4) and all(isinstance(x, (int, float)) for x in v):
            m = 255.0 if max(v) > 1.0 else 1.0
            sv[k] = {"Red": int(round(v[0] * 255 / m)), "Green": int(round(v[1] * 255 / m)), "Blue": int(round(v[2] * 255 / m)),
                     "Alpha": int(round(v[3] * 255 / m)) if len(v) == 4 else 255}
    if bad:
        raise RuntimeError("unknown look path(s): " + ", ".join(bad) + " (studio_look_get {} lists every path)")
    return edit(a, {"do": "look", "set": sv})


@tool("studio_materials", "List material presets, or apply one (name prefix; keep_colours default true).",
      obj({"apply": {"type": "string"}, "keep_colours": {"type": "boolean"}}))
def t_mat(a):
    if a.get("apply"):
        return S.call({"do": "material", "name": a["apply"], "keep_colours": a.get("keep_colours", True)})
    return S.call({"do": "materials_list"})


@tool("studio_stiffness", "Per-guide stiffness map values s / r (0..1) for selected guides; hold_curls: true raises "
      "stiffness so baked curls keep their shape.",
      obj({"where": WHERE, "s": {"type": "number"}, "r": {"type": "number"}, "hold_curls": {"type": "boolean"}}))
def t_stiff(a):
    if a.get("hold_curls"):
        return S.call({"do": "hold_curls"})
    return S.call({"do": "stiffness", "where": a.get("where", {"all": True}), **{k: a[k] for k in ("s", "r") if k in a}})


@tool("studio_rig", "Lighting rig: tod 0..8 (0 = HairLab capture rig), shadows, quality 0..2, sun, sky, exposure, ... "
      "(see AGENT_GUIDE 'rig').", obj({"set": {"type": "object"}}, ["set"]))
def t_rig(a):
    return S.call({"do": "rig", **a["set"]})


@tool("studio_game_accurate", "Game-accurate hair preview (DEFAULT ON): the hair as the game's path tracer builds it "
      "(32 hairs per growth face, no LOD, density texture per hair, the fur's width, root transparency from the user's "
      "dx12user.settings, closest hair only). on=false = the old HairWorks 1.2.1 raster preview (close-up detail LOD, ~2.8x more, "
      "thinner hairs). Optional: width_scale, hairs_per_face, root_alpha_mode 0/1/2, root_alpha_falloff, spline, hair_scatter "
      "(share of the light the hair blocks that still reaches the skin, 0.5). Returns the current values.",
      obj({"on": {"type": "boolean"}, "width_scale": {"type": "number"}, "hairs_per_face": {"type": "integer"},
           "root_alpha_mode": {"type": "integer"}, "root_alpha_falloff": {"type": "number"}, "spline": {"type": "integer"},
           "hair_scatter": {"type": "number"}, "wave_scale": {"type": "number"},
           "lod_emu": {"type": "boolean", "description": "detail-LOD emulation (default true, L480): width -> lerp(width, detailLODWidth, t), hairs kept x lerp(1, detailLODDensity, t), t from the camera distance"},
           "lod_dist_scale": {"type": "number", "description": "LOD distance = SDK camera distance x this (1.64)"},
           "lod_t": {"type": "number", "description": ">= 0: fixed LOD factor (calibration); -1 = from the camera"},
           "width_const": {"type": "number"}, "width_exp": {"type": "number"}, "scale_exp": {"type": "number"},
           "count_exp": {"type": "number", "description": "width x (hairs / 32)^-count_exp: fewer hairs look thicker in game (0.3)"},
           "roughness": {"type": "number", "description": "PTHairRoughness (-1 auto): a hair brightness gain (.15 x1.05, .8 x.60)"},
           "tb_exp": {"type": "number", "description": "textureBrightness gain exponent (1.28)"},
           "neutralise": {"type": "boolean", "description": "ignore the knobs the game ignores (specular, diffuseHairNormalWeight, ambientEnvScale, shadowSigma / DensityScale), default true"},
           "stiff_by_fur": {"type": "boolean", "description": "scale the bone-stiffness hold by the fur stiffness (default true: stiffness 0 lets the hair fall as in game)"}}))
def t_game_accurate(a):
    return S.call({"do": "game_accurate", **a})


@tool("studio_simulate","Run physics: preset idle / walk / run / turn / nod / spin / wind / rest for seconds "
      "(settle = rest pose). on=false keeps hair on the rest shape.",
      obj({"preset": {"type": "string"}, "seconds": {"type": "number"}, "on": {"type": "boolean"}, "reset": {"type": "boolean"},
           "amount": {"type": "number", "description": "motion / wind strength multiplier (1)"}, "speed": {"type": "number"},
           "wind_yaw_deg": {"type": "number", "description": "wind heading 0-360 (default 206.6 = front-left towards back)"},
           "wind_pitch_deg": {"type": "number", "description": "wind pitch -45..45 (default 2.85)"}}))
def t_sim(a):
    if "on" in a:
        return S.call({"do": "simulate", "on": a["on"]})
    p = a.get("preset", "rest")
    if p == "rest":
        return S.call({"do": "settle", "seconds": a.get("seconds", 2)})
    return S.call({"do": "motion", "preset": p, "seconds": a.get("seconds", 2), "reset": a.get("reset", False),
                   "amount": a.get("amount", 1.0), "speed": a.get("speed", 1.0),
                   **{k: a[k] for k in ("wind_yaw_deg", "wind_pitch_deg") if k in a}})


@tool("studio_dump_geo", "Read-only: write the groom as it is now (alive guides in order with all control points, edit frame, "
      "faces by alive index with per-corner uv, K) to a JSON file (path; default <out>/geo.json). For offline checks / diffs. "
      "guides[n] is NOT Studio guide n: use index[n] (its Studio index) before passing it to any other tool. "
      "render: true also writes render[n] = the points the Studio draws / exports (edit point + the build's per-CV "
      "fur-frame offset of the base guide, up to ~15 mm). Reshape ops are in the edit frame: when you design shapes "
      "relative to the head / scalp, design them in the render frame and write points = design - (render - guides).",
      obj({"path": {"type": "string"}, "render": {"type": "boolean"}}))
def t_dump_geo(a):
    return S.call({"do": "dump_geo", "path": a.get("path") or os.path.join(S.out, "geo.json"),
                   **({"render": True} if a.get("render") else {})})


# --- composite fixers (2026-10-16, agent tools): the recipes that worked, in one call each, with numbers before / after
def gate_cameras(names=None):
    cams = S.call({"do": "gate_cameras"})["cameras"]
    return [c for c in cams if not names or c["name"] in names]


def cam_keys(c):
    return {"eye": c["eye"], "target": c["target"], "fovy": c.get("fovy", 26)}


@tool("studio_fix_direction_all", "Wrong-direction strands, fixed and RE-VERIFIED in one call (user rule 2026-10-16: never a "
      "strand going the wrong way). Pass 1..passes (2): studio_check_direction {fix: true} at rest, then at every heading of "
      "wind_yaw_deg ([15, 105, 195, 285]) at wind_strength (5 = toolbar max, covers 1); stops early when a pass finds nothing. "
      "Then VERIFIES: rest + every heading at strengths [1, 5] without fix, and reports before / after / remaining per run "
      "(remaining lists the guide ids: explain or delete them). protect = selector of guides never re-combed. The fix is the "
      "proven one (mean shape of the non-offending neighbours within fix_nb_mm 25 from fix_from 0.25, gravity floor), one undo "
      "group per pass. ~1-3 min.",
      obj({"passes": {"type": "integer"}, "wind_yaw_deg": {"type": "array", "items": {"type": "number"}},
           "wind_strength": {"type": "number"}, "verify_strengths": {"type": "array", "items": {"type": "number"}},
           "protect": WHERE, "fix_nb_mm": {"type": "number"}, "fix_from": {"type": "number"}, "min_drop": {"type": "number"},
           "wind_s": {"type": "number"}}))
def t_fix_direction_all(a):
    yaws = a.get("wind_yaw_deg", [15, 105, 195, 285])
    strength = a.get("wind_strength", 5)
    extra = {k: a[k] for k in ("protect", "fix_nb_mm", "fix_from", "min_drop", "wind_s") if k in a}
    try:
        CALLS.append(S.call({"do": "status"})["status"]["ops"])
    except Exception:
        pass
    before = S.call({"do": "check_direction", "wind": False}).get("result", {})
    hist = []
    for p in range(a.get("passes", 2)):
        fixed = 0
        row = {"pass": p + 1, "runs": []}
        r = S.call({"do": "check_direction", "wind": False, "fix": True, **extra}, timeout=900).get("result", {})
        fixed += r.get("fixed", 0)
        row["runs"].append({"wind": 0, "rest_offenders": r.get("rest_offenders"), "fixed": r.get("fixed", 0), "after": r.get("rest_offenders_after_fix")})
        for y in yaws:
            r = S.call({"do": "check_direction", "wind": True, "wind_yaw_deg": y, "wind_strength": strength, "fix": True, **extra}, timeout=900).get("result", {})
            fixed += r.get("fixed", 0)
            row["runs"].append({"wind": strength, "yaw": y, "rest_offenders": r.get("rest_offenders"), "wind_offenders": r.get("wind_offenders"), "fixed": r.get("fixed", 0)})
        row["fixed"] = fixed
        hist.append(row)
        if fixed == 0:
            break
    # verify without fixing
    ver = {"rest": None, "wind": []}
    r = S.call({"do": "check_direction", "wind": False}).get("result", {})
    ver["rest"] = {"offenders": r.get("rest_offenders"), "list": r.get("rest_list", [])[:40]}
    remaining = set(e["i"] for e in r.get("rest_list", []))
    for s in a.get("verify_strengths", [1, 5]):
        for y in yaws:
            r = S.call({"do": "check_direction", "wind": True, "wind_yaw_deg": y, "wind_strength": s}, timeout=900).get("result", {})
            ver["wind"].append({"yaw": y, "strength": s, "offenders": r.get("wind_offenders"), "list": r.get("wind_list", [])[:20]})
            remaining.update(e["i"] for e in r.get("wind_list", []))
    total_wind = sum(w["offenders"] or 0 for w in ver["wind"])
    return {"before": {"rest_offenders": before.get("rest_offenders"), "rest_up": before.get("rest_up"), "rest_against": before.get("rest_against")},
            "passes": hist, "verify": ver, "remaining_guides": sorted(remaining),
            "clean": (ver["rest"]["offenders"] or 0) == 0 and total_wind == 0,
            "note": "clean = 0 rest and 0 wind offenders over the verify runs (the SDK wind is not deterministic: a single "
                    "offender that moves between runs is noise; re-run to confirm). Remaining guides: look at them in the wind "
                    "renders, then shorten / reshape / delete, never hand back with them unexplained."}


@tool("studio_fix_faces_skin", "Hair through the FACE / EARS / NECK that comes from growth faces, not guides (HairWorks draws a "
      "face's hair as blends of its 3 guides; a face whose guides pass on different sides of the head draws hair through the "
      "skin while every guide is clean: Corinne mcp_18, Keira mcp_1). One call: studio_check_faces at wind_strength (1 = the "
      "user's wind) deletes ONLY the faces whose interpolated hair goes through the SKIN (fix_what ['skin']: the deepest "
      "sample is nearest to a non-scalp vertex), never the under-scalp ones that cover a parting (TAF 8.4: a full "
      "penetration fix reopens holes); then refills what it opened with studio_fill_scalp from the cameras in refill "
      "(['topdown', '34_above', '34_above_left'] of the gate set; [] = no refill), then re-checks. Reports faces "
      "through the skin before / after, deleted, planted, scalp px per refill camera before / after. ~1-2 min.",
      obj({"wind_strength": {"type": "number"}, "refill": {"type": "array", "items": {"type": "string"}},
           "max_new": {"type": "integer"}, "spacing_mm": {"type": "number"}, "protect": WHERE, "where": WHERE}))
def t_fix_faces_skin(a):
    try:
        CALLS.append(S.call({"do": "status"})["status"]["ops"])
    except Exception:
        pass
    ws = a.get("wind_strength", 1)
    refill = a.get("refill", ["topdown", "34_above", "34_above_left"])
    cams = gate_cameras(refill) if refill else []
    scalp0 = {}
    for c in cams:
        png = S.png("ffs_before_" + c["name"])
        S.call({"do": "render", "path": png, "overlay": "scalp", "size": 1000, **cam_keys(c)})
        scalp0[c["name"]] = green_px(png)
    where = {k: a[k] for k in ("where", "protect") if k in a}
    r1 = S.call({"do": "check_faces", "wind_strength": ws, "fix": True, "fix_what": ["skin"], **where}, timeout=1800).get("result", {})
    pen1 = r1.get("penetration", {})
    planted = []
    for c in cams:
        f = S.call({"do": "fill_scalp", "size": 1000, "max_new": a.get("max_new", 40), **({"spacing_mm": a["spacing_mm"]} if "spacing_mm" in a else {}), **cam_keys(c)}, timeout=900)
        planted.append({"camera": c["name"], "planted": f.get("planted", 0)})
    r2 = S.call({"do": "check_faces", "wind_strength": ws, "list_only": True, **where}, timeout=1800).get("result", {})
    pen2 = r2.get("penetration", {})
    scalp1 = {}
    for c in cams:
        png = S.png("ffs_after_" + c["name"])
        S.call({"do": "render", "path": png, "overlay": "scalp", "size": 1000, **cam_keys(c)})
        scalp1[c["name"]] = green_px(png)
    return {"before": {"through_skin_rest": pen1.get("skin_rest"), "through_skin_wind": pen1.get("skin_wind"), "interp_only": pen1.get("interp_only"), "guides_inside": r1.get("guides_inside", {}).get("count")},
            "deleted_faces": r1.get("deleted_faces", 0), "planted": planted,
            "after": {"through_skin_rest": pen2.get("skin_rest"), "through_skin_wind": pen2.get("skin_wind"), "interp_only": pen2.get("interp_only"), "guides_inside": r2.get("guides_inside", {}).get("count"),
                      "list": [e for e in pen2.get("list", []) if e.get("skin")][:40]},
            "scalp_px": {k: {"before": scalp0.get(k), "after": scalp1.get(k)} for k in scalp0},
            "note": "faces still through the skin after the fix have a GUIDE inside the skin (guides_inside): fix the guide (push_out / "
                    "delete / collider) rather than the face. Scalp px that went UP = the deleted faces covered scalp: refill from that camera."}


def green_px(png):
    if Image is None:
        return None
    im = Image.open(png).convert("RGB")
    import numpy as np
    A = np.asarray(im)
    return int(((A[:, :, 1] > 200) & (A[:, :, 0] < 60) & (A[:, :, 2] < 60)).sum())


@tool("studio_scalp_report", "Bald patches, measured and LOCATED (checklist 5): for each gate camera (names; default the "
      "views agents skip: topdown, 34_above, 34_above_left, side_right, side_left, back, low_nape) renders the scalp overlay, "
      "counts the green scalp pixels now and in the reference (baseline = the build as opened, or a reference project "
      "path), and picks the centre of the biggest visible scalp blob (studio_pick auto scalp): scalp_world point, hair or "
      "skin, nearest guides, so the next studio_fill_scalp / studio_rebuild_from_cards {where: {near, radius}} has a target. "
      "worse = more than reference x 1.1 + 400 px. Returns the per-camera table and the overlay images' paths.",
      obj({"names": {"type": "array", "items": {"type": "string"}}, "reference": {"type": "string"}, "size": {"type": "integer"},
           "pick": {"type": "boolean", "description": "locate the biggest blob per camera (default true)"}}))
def t_scalp_report(a):
    names = a.get("names", ["topdown", "34_above", "34_above_left", "side_right", "side_left", "back", "low_nape"])
    cams = gate_cameras(names)
    size = a.get("size", 1000)
    ref = a.get("reference")
    rows = []
    for c in cams:
        png = S.png("scalp_" + c["name"])
        S.call({"do": "render", "path": png, "overlay": "scalp", "size": size, **cam_keys(c)})
        row = {"camera": c["name"], "scalp_px": green_px(png), "png": png}
        if not ref or ref == "baseline":
            pngb = S.png("scalpbase_" + c["name"])
            S.call({"do": "render", "path": pngb, "overlay": "scalp", "size": size, "baseline": True, **cam_keys(c)})
            row["reference_px"] = green_px(pngb)
        if row.get("reference_px") is not None and row["scalp_px"] is not None:
            row["worse"] = row["scalp_px"] > row["reference_px"] * 1.1 + 400
        if a.get("pick", True) and (row["scalp_px"] or 0) > 0:
            try:
                p = S.call({"do": "pick", "auto": "scalp", "size": size, "n": 4, "tag": "scalp_" + c["name"], **cam_keys(c)})
                row["biggest_blob"] = {"scalp_world": p.get("scalp_world"), "scalp_edit": p.get("scalp_edit"), "top": p.get("top"),
                                       "nearest_guides_by_root": p.get("nearest_guides_by_root", [])[:4], "pixel": p.get("pixel")}
            except Exception as e:
                row["biggest_blob"] = {"error": str(e)}
        rows.append(row)
    if ref and ref != "baseline":
        g = S.call({"do": "handback_gate", "reference": ref, "skip": ["roots", "loops", "direction", "faces", "colliders", "silhouette", "views", "export"], "tag": "scalp_report", "size": size}, timeout=3600)
        refpx = {c["camera"]: c["reference_px"] for c in g.get("items", {}).get("scalp", {}).get("cameras", [])}
        for row in rows:
            row["reference_px"] = refpx.get(row["camera"])
            if row["reference_px"] is not None and row["scalp_px"] is not None:
                row["worse"] = row["scalp_px"] > row["reference_px"] * 1.1 + 400
    return {"cameras": rows, "worse": [r["camera"] for r in rows if r.get("worse")],
            "note": "fix order (AGENT_DIFFICULTIES 1b): too few guides -> rebuild_from_cards {where: {near: blob.scalp_edit, radius: 0.04}, spacing_mm: 6, uv_mode: nearest}; "
                    "a local hole -> fill_scalp from that camera (several passes); hair inside the skull -> check_faces interp_only + plant_parting. Never density / width tweaks."}


@tool("studio_hairline_fix", "Forehead / cheek / neck roots, fixed and re-measured in one call (user rules 2026-10-15: no hair "
      "roots from the forehead or the neck). (1) studio_hairline_check lists roots on skin outside the painted scalp and guides "
      "over the face; (2) the roots farther than tolerance_mm (4) from the hairline are deleted (delete_off_scalp true) and "
      "guides over the face too (delete_over_face true); roots within tolerance are moved onto the hairline "
      "(studio_align_to_hairline inset_mm 3); (3) hairline_fill part (default 'all'; '' = none) refills the gap from the scalp "
      "side; (4) fade_hairline (false) softens the new front; (5) re-check. Reports before / after counts and the deleted "
      "guides. Not for grooms whose scalp mask is wrong (Birna: hundreds of roots 'off' that are on the real hairline): "
      "check roots_off_scalp_count against the reference first (studio_handback_gate reports both).",
      obj({"tolerance_mm": {"type": "number"}, "delete_off_scalp": {"type": "boolean"}, "delete_over_face": {"type": "boolean"},
           "inset_mm": {"type": "number"}, "part": {"type": "string"}, "fade": {"type": "boolean"}, "max_delete": {"type": "integer"},
           "fill_args": {"type": "object"}}))
def t_hairline_fix(a):
    try:
        CALLS.append(S.call({"do": "status"})["status"]["ops"])
    except Exception:
        pass
    tol = a.get("tolerance_mm", 4)
    before = S.call({"do": "hairline_check"})
    off = before.get("roots_off_scalp", [])
    far = [e["i"] for e in off if e.get("to_hairline_mm", 0) > tol]
    over = [e["i"] for e in before.get("over_face", [])]
    todel = sorted(set((far if a.get("delete_off_scalp", True) else []) + (over if a.get("delete_over_face", True) else [])))
    mx = a.get("max_delete", 400)
    deleted = 0
    if todel and len(todel) <= mx:
        S.call({"do": "delete", "where": {"indices": todel}})
        deleted = len(todel)
    elif len(todel) > mx:
        return {"error": "%d guides to delete, more than max_delete %d: the scalp mask is probably wrong for this head (compare with the reference); "
                         "pass max_delete to force" % (len(todel), mx), "before": {"roots_off_scalp": len(off), "guides_over_face": len(over)}}
    moved = S.call({"do": "align_to_hairline", "tolerance_mm": 0.5, "inset_mm": a.get("inset_mm", 3)}).get("moved", 0)
    filled = None
    if a.get("part", "all"):
        filled = S.call({"do": "hairline_fill", "part": a.get("part", "all"), **(a.get("fill_args") or {})}, timeout=900)
        filled = {k: filled.get(k) for k in ("planted", "rows", "part", "skipped") if k in filled}
    faded = None
    if a.get("fade"):
        faded = S.call({"do": "fade_hairline"}, timeout=900)
        faded = {k: faded.get(k) for k in ("changed", "planted", "guides") if k in faded}
    after = S.call({"do": "hairline_check"})
    return {"before": {"roots_off_scalp": len(off), "guides_over_face": len(over), "far_than_tol": len(far)},
            "deleted_guides": deleted, "deleted_ids": todel, "moved_to_hairline": moved, "hairline_fill": filled, "fade_hairline": faded,
            "after": {"roots_off_scalp": after.get("roots_off_scalp_count"), "guides_over_face": after.get("guides_over_face"),
                      "lifted": after.get("lifted_count"), "flow_outliers": after.get("flow_outliers_count")},
            "note": "then LOOK: front close-up with overlay guides, both 3/4 at full size; a hard new edge -> studio_soften_hairline on the "
                    "front row (never on freshly planted temple rows: it thins them to bare)."}


@tool("studio_crown_metric", "Crown ropes / knots, measured (TOOL_ARCHITECT_FINDINGS 1, tools/hairworks/toolarch/crown_metric.py): "
      "for every guide rooted within crown_mm (60) of the head top, the angle between its drawn direction at 25 / 50 / 75 % "
      "of its length and the mean direction of the guides rooted within nb_mm (20). Targets: median < 15 deg and p90 < 45 "
      "at 25 % (approved packs 12-16 / 34-45; Shani's ropes 22.7 / 68.7). Also face-area percentiles (ratio p95/p5 < 10; "
      "slivers < 4 mm2 are dense bundles). Fix that worked: studio_regrid {count_scale 0.75, keep_cross true, cross_dot 0.6, "
      "oppose_dot -1} + fill_scalp x 3 cameras + studio_fix_direction_all; NOT conform_shell alone, crown_swirl r70/90, "
      "deleting slivers, flow_whorl.",
      obj({"crown_mm": {"type": "number"}, "nb_mm": {"type": "number"}}))
def t_crown_metric(a):
    geo = os.path.join(S.out, "crown_metric_geo.json")
    S.call({"do": "dump_geo", "path": geo, "render": True})
    script = os.path.join(REPO, "tools", "hairworks", "toolarch", "crown_metric.py")
    r = subprocess.run([sys.executable, script, geo, str(a.get("crown_mm", 60)), str(a.get("nb_mm", 20))], capture_output=True, text=True, cwd=REPO, timeout=600)
    if r.returncode:
        return {"error": r.stderr[-800:]}
    out = {"raw": r.stdout.strip().splitlines(), "fractions": {}}
    import re as _re
    for l in out["raw"]:
        m = _re.match(r"frac (\S+): median\s+([\d.]+) deg\s+p90\s+([\d.]+)\s+>45deg\s+(\d+)\s+of (\d+)", l)
        if m:
            out["fractions"][m.group(1)] = {"median_deg": float(m.group(2)), "p90_deg": float(m.group(3)), "over_45": int(m.group(4)), "crown_guides": int(m.group(5))}
        m = _re.match(r"faces (\d+): area mm2 p5/p50/p95 = ([\d.]+) / ([\d.]+) / ([\d.]+)\s+ratio p95/p5 ([\d.]+)\s+<4mm2: (\d+)", l)
        if m:
            out["faces"] = {"count": int(m.group(1)), "area_p5": float(m.group(2)), "area_p50": float(m.group(3)), "area_p95": float(m.group(4)), "ratio_p95_p5": float(m.group(5)), "slivers_under_4mm2": int(m.group(6))}
    f25 = out["fractions"].get("0.25", {})
    out["pass"] = bool(f25) and f25["median_deg"] < 15 and f25["p90_deg"] < 45
    out["target"] = "25 %: median < 15 deg, p90 < 45; face area ratio p95/p5 < 10"
    return out


@tool("studio_parting_check", "A parting, measured (checklist 4 / 6): line [[x,y,z],[x,y,z]] (edit frame, front -> back) and "
      "band_mm (12). Reports (1) faces across the line: divergent faces (studio_check_faces, rest only) whose centroid lies "
      "within the band, split into 'opposite' (guide directions > 120 deg apart: hair blended INSIDE the skull, the bare-strip "
      "mechanism) and 'divergent'; (2) interp-only penetrations in the band (invisible hair); (3) scalp px from the top-down "
      "and 3/4-above gate cameras now vs the baseline (build as opened). Fix that worked: delete the opposite faces "
      "(studio_check_faces fix_what ['divergent'] where {near: mid, radius}) then studio_plant_parting on the line + "
      "studio_fill_scalp from the top; design new shapes in the drawn frame (studio_dump_geo render: true).",
      obj({"line": {"type": "array"}, "band_mm": {"type": "number"}, "size": {"type": "integer"}}, ["line"]))
def t_parting_check(a):
    import numpy as np
    p0, p1 = np.array(a["line"][0], float), np.array(a["line"][1], float)
    band = a.get("band_mm", 12) / 1000.0
    L = p1 - p0
    def dist(c):
        c = np.array(c, float)
        t = float(np.clip(np.dot(c - p0, L) / max(np.dot(L, L), 1e-9), 0, 1))
        return float(np.linalg.norm(p0 + t * L - c))
    mid = ((p0 + p1) / 2).tolist()
    radius = float(np.linalg.norm(L) / 2 + 0.03)
    f = S.call({"do": "check_faces", "wind": False, "list_only": True, "where": {"near": mid, "radius": radius}}, timeout=1800).get("result", {})
    div = [e for e in f.get("divergent", {}).get("list", []) if dist(e["centroid"]) < band]
    opp = [e for e in div if e.get("angle_deg", 0) > 120]
    pen = [e for e in f.get("penetration", {}).get("list", []) if e.get("interp_only") and dist(e["centroid"]) < band]
    rows = []
    for c in gate_cameras(["topdown", "34_above", "34_above_left"]):
        png = S.png("part_" + c["name"]); pngb = S.png("partbase_" + c["name"])
        S.call({"do": "render", "path": png, "overlay": "scalp", "size": a.get("size", 1000), **cam_keys(c)})
        S.call({"do": "render", "path": pngb, "overlay": "scalp", "size": a.get("size", 1000), "baseline": True, **cam_keys(c)})
        rows.append({"camera": c["name"], "scalp_px": green_px(png), "baseline_px": green_px(pngb), "png": png})
    return {"faces_in_band": {"divergent": len(div), "opposite": len(opp), "interp_only_penetrations": len(pen)},
            "opposite_faces": opp[:60], "divergent_faces": [e for e in div if e not in opp][:40], "penetrations": pen[:40],
            "scalp": rows, "band_mm": a.get("band_mm", 12), "line": a["line"],
            "note": "opposite faces = guides from both sides of the parting in one face: their in-between hair lies inside the skull. "
                    "Delete them (ids above, studio_check_faces fix_what ['divergent'] with where near the line), replant with studio_plant_parting, refill, re-run."}


# --- hand-back gate (2026-10-16): the whole checklist of tools/hairworks/AGENT_DIFFICULTIES.md in one call
GATE_KEYS = ("reference", "tag", "size", "skip", "use_shots", "cam_dist", "cam_fovy", "tile", "wind_yaw_deg", "wind_strengths", "wind_s",
             "faces_wind", "max_roots_off", "max_over_face", "max_loops", "max_rest", "max_wind", "max_through_skin", "small_mm",
             "scalp_tol", "scalp_abs", "flare_px", "flare_frac", "flare_min_px", "loss_frac", "silhouette_info_only")


def labelled_sheet(pngs, names, out, tile=500, cols=5):
    """contact sheet with the camera name on every tile (the Studio writes an unlabelled one)"""
    if Image is None or not pngs:
        return None
    from PIL import ImageDraw
    rows = (len(pngs) + cols - 1) // cols
    sh = Image.new("RGB", (cols * tile, rows * (tile + 22)), (20, 20, 24))
    d = ImageDraw.Draw(sh)
    for i, (p, n) in enumerate(zip(pngs, names)):
        try:
            im = Image.open(p).convert("RGB").resize((tile, tile))
        except Exception:
            continue
        x, y = (i % cols) * tile, (i // cols) * (tile + 22)
        sh.paste(im, (x, y + 22))
        d.text((x + 6, y + 5), n, fill=(230, 220, 200))
    sh.save(out)
    return out


def run_export_audit(cfg, project, fur=None):
    """tools/hairworks/export_audit.py on the open build (JSON); None when it cannot run"""
    script = os.path.join(REPO, "tools", "hairworks", "export_audit.py")
    if not os.path.exists(script):
        return None
    cmd = [sys.executable, script, cfg, "--json"]
    if project:
        cmd += ["--project", project]
    if fur:
        cmd += ["--fur", fur]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, timeout=1800)
        res = json.loads(r.stdout)[0]
    except Exception as e:
        return {"error": "export_audit.py failed: %s" % e}
    furs = res.get("furs", [])
    return {"checked": sum(f.get("n_checked", 0) for f in furs), "ok": sum(f.get("n_ok", 0) for f in furs),
            "diff": sum(f.get("n_diff", 0) for f in furs), "missing": sum(f.get("n_missing", 0) for f in furs),
            "problems": [p for f in furs for p in f.get("problems", [])][:40], "project_how": res.get("project_how"),
            "notes": res.get("notes", [])[:10]}


@tool("studio_handback_gate", "MANDATORY before handing a groom back (2026-10-16): runs the whole hand-back checklist of "
      "tools/hairworks/AGENT_DIFFICULTIES.md on the open project and returns pass / fail with the defect list and a contact "
      "sheet. Items: roots (hairline_check: roots_off_scalp 0, guides_over_face 0), loops (0), direction (rest + wind at yaw "
      "15/105/195/285 x strengths [1, 5]: 0 offenders), faces (check_faces at wind 1: 0 faces drawing hair through the SKIN, "
      "face / ears / neck; under-scalp penetrations and divergent faces are reported only), colliders (misbound 0; small "
      "isolated spheres reported), scalp (green scalp px per camera vs the reference project, must not be worse than "
      "reference x (1 + scalp_tol 0.1) + scalp_abs 400), silhouette (hair outside the reference silhouette: flare > max(flare_min_px 300, "
      "flare_frac 0.03 x reference hair px) fails; loss of covered hair > loss_frac 0.06 fails), views (front, both 3/4, both "
      "sides, back, top-down, 3/4 above both sides, low nape -> contact sheet), export (the Studio's own dev export must "
      "succeed; look.extra_set of the exported config carries every project look key with the same value; no "
      "textureBrightness / strandBlendScale 0; beards on Coarse short). reference = the user snapshot project you started from "
      "(<char>_user_HHMM.groomproj.json; default: the build as opened). export_audit: true also runs export_audit.py on the "
      "build (slow: builds the pick). skip = item names to leave out (explain why in your report). The verdict is recorded: "
      "while it is FAIL, studio_save / export / save_project are refused unless they pass override='<reason>' (logged). "
      "Takes 2-6 min (8 wind runs + 4 for faces + ~30 renders). Returns the JSON report and the sheet image.",
      obj({"reference": {"type": "string", "description": "reference project path (.groomproj.json): the user's snapshot; 'baseline' = the build as opened"},
           "tag": {"type": "string"}, "size": {"type": "integer", "description": "render size px (1000)"},
           "skip": {"type": "array", "items": {"type": "string"}, "description": "items to skip: roots loops direction faces colliders scalp silhouette views export"},
           "export_audit": {"type": "boolean", "description": "also run tools/hairworks/export_audit.py (minutes)"},
           "wind_strengths": {"type": "array", "items": {"type": "number"}}, "wind_yaw_deg": {"type": "array", "items": {"type": "number"}},
           "wind_s": {"type": "number"}, "faces_wind": {"type": "number"},
           "max_roots_off": {"type": "integer"}, "max_over_face": {"type": "integer"}, "max_loops": {"type": "integer"},
           "max_rest": {"type": "integer"}, "max_wind": {"type": "integer"}, "max_through_skin": {"type": "integer"},
           "small_mm": {"type": "number"}, "scalp_tol": {"type": "number"}, "scalp_abs": {"type": "integer"},
           "flare_px": {"type": "number"}, "flare_frac": {"type": "number"}, "flare_min_px": {"type": "integer"}, "loss_frac": {"type": "number"},
           "silhouette_info_only": {"type": "boolean"}, "use_shots": {"type": "boolean"}, "tile": {"type": "integer"},
           "show": {"type": "boolean", "description": "return the contact sheet image (default true)"}}))
def t_handback_gate(a):
    cmd = {"do": "handback_gate", **{k: a[k] for k in GATE_KEYS if k in a}}
    r = S.call(cmd, timeout=7200)
    views = r.get("items", {}).get("views", {})
    if views.get("pngs"):
        lab = labelled_sheet(views["pngs"], views.get("order", []), os.path.join(r.get("dir", S.out), (a.get("tag") or "gate") + "_sheet_labelled.png"), a.get("tile", 500))
        if lab:
            r["sheet_labelled"] = lab
    if a.get("export_audit"):
        st = S.call({"do": "status"})["status"]
        r["export_audit"] = run_export_audit(st["config"], r.get("project") or None)
        ea = r["export_audit"] or {}
        if ea.get("error") or ea.get("missing") or ea.get("diff"):
            r["pass"] = False
            r.setdefault("failed", []).append("export_audit: %s" % (ea.get("error") or "%d MISSING, %d DIFF" % (ea.get("missing", 0), ea.get("diff", 0))))
            S.call({"do": "gate_fail", "why": r["failed"][-1]})
    out = [r]
    if a.get("show", True) and (r.get("sheet_labelled") or r.get("sheet")):
        out.append(image_content(r.get("sheet_labelled") or r["sheet"], None, a.get("max_px", 1400)))
    return out


@tool("studio_gate_status", "The last hand-back gate verdict on the open build: ran, pass, when, stale (edits since), failed items, "
      "and whether the save / export guard applies.", obj({}))
def t_gate_status(a):
    return S.call({"do": "gate_status"})


# --- saving
@tool("studio_save", "Save the current edits under a NEW variant name into build/desktop_studio/mcp/<name>/ "
      "(edits.json, look, project). Never overwrites approved builds. export: true also builds the mod zip "
      "(not installed); release: true adds the Core requirement. REFUSED while the last studio_handback_gate on this build "
      "FAILED, unless override='<reason>' is given (the reason is logged in <out>/gate_overrides.log).",
      obj({"name": {"type": "string"}, "export": {"type": "boolean"}, "release": {"type": "boolean"},
           "override": {"type": "string", "description": "reason to save despite a failed gate (logged)"}}, ["name"]))
def t_save(a):
    d = os.path.join(S.out, a["name"])
    os.makedirs(d, exist_ok=True)
    ov = {"override": a["override"]} if a.get("override") else {}
    r = S.call({"do": "save_edits", "path": os.path.join(d, "edits.json"), **ov})
    r["project"] = S.call({"do": "save_project", "path": os.path.join(d, a["name"] + ".groomproj.json"), **ov})
    if a.get("export"):
        r["export"] = S.call({"do": "export", "tag": a["name"], "release": a.get("release", False), **ov}, timeout=1200)
    r["dir"] = d
    return r


@tool("studio_batch", "Run several tools in one call, in order: calls = [{tool: 'studio_...', args: {...}}, ...]. "
      "Returns every result (images included). stop_on_error (true) stops at the first failing call. Use it for "
      "edit + measure + compare loops.",
      obj({"calls": {"type": "array", "items": {"type": "object"}}, "stop_on_error": {"type": "boolean"}}, ["calls"]))
def t_batch(a):
    out = []
    for i, c in enumerate(a["calls"]):
        t = TOOLS.get(c.get("tool"))
        if not t or c.get("tool") == "studio_batch":
            out.append({"call": i, "error": f"unknown tool {c.get('tool')}"})
            if a.get("stop_on_error", True):
                break
            continue
        try:
            res = t["fn"](c.get("args") or {})
            items = res if isinstance(res, list) else [res]
            out.append({"call": i, "tool": c["tool"]})
            out.extend(items)
        except Exception as e:
            out.append({"call": i, "tool": c["tool"], "error": f"{type(e).__name__}: {e}"})
            if a.get("stop_on_error", True):
                break
    return out


# ============================================================================================ MCP plumbing
def handle(msg):
    m = msg.get("method")
    if m == "initialize":
        return {"protocolVersion": msg.get("params", {}).get("protocolVersion", PROTOCOL),
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "hairwork-studio", "version": "0.4.0"},
                "instructions": "Live Hairwork Studio session (Witcher 3 HairWorks grooms). Start with studio_open, "
                                "studio_shots, studio_compare (vanilla | as opened | current). Find bald patches with "
                                "studio_render overlay=scalp + studio_scalp_coverage, locate them with studio_pick / "
                                "studio_pick_region, fix with studio_densify / studio_duplicate / studio_move_root_to_scalp, "
                                "verify with studio_compare. Always judge against the vanilla still. Save with studio_save "
                                "under a new name."}
    if m == "ping":
        return {}
    if m == "tools/list":
        return {"tools": [{k: v for k, v in t.items() if k != "fn"} for t in TOOLS.values()]}
    if m == "tools/call":
        p = msg["params"]
        t = TOOLS.get(p["name"])
        if not t:
            return {"content": [{"type": "text", "text": "unknown tool " + p["name"]}], "isError": True}
        try:
            res = t["fn"](p.get("arguments") or {})
            items = res if isinstance(res, list) else [res]
            content = [i if isinstance(i, dict) and i.get("type") == "image" else
                       {"type": "text", "text": json.dumps(i, indent=1)} for i in items]
            return {"content": content}
        except Exception as e:
            return {"content": [{"type": "text", "text": f"{type(e).__name__}: {e}"}], "isError": True}
    raise KeyError(m)


def main():
    inp = sys.stdin.buffer
    out = sys.stdout.buffer
    try:
        for line in inp:
            if not line.strip():
                continue
            msg = json.loads(line)
            if "id" not in msg:   # notification
                continue
            try:
                resp = {"jsonrpc": "2.0", "id": msg["id"], "result": handle(msg)}
            except KeyError:
                resp = {"jsonrpc": "2.0", "id": msg["id"], "error": {"code": -32601, "message": "method not found"}}
            except Exception as e:
                resp = {"jsonrpc": "2.0", "id": msg["id"], "error": {"code": -32603, "message": str(e)}}
            out.write((json.dumps(resp) + "\n").encode())
            out.flush()
    finally:
        S.stop()


if __name__ == "__main__":
    main()
