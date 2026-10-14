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
                 "wind_check"}

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

    def png(self, tag):
        self.n += 1
        return os.path.join(self.out, f"{self.n:04d}_{tag}.png")


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


@tool("studio_status", "Current build, guide / face / op counts, unmatched roots, hair budget vs the reference (limit 1.2).",
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
           "guides_body": {"type": "boolean", "description": "with overlay 'guides': hide guide parts behind the body (default: studio_guide_view)"}}))
def t_render(a):
    extra = {"baseline": a.get("baseline", False), "hair": a.get("hair", True), "vanilla": a.get("vanilla", False)}
    for k in ("guides_front", "guides_body"):
        if k in a:
            extra[k] = a[k]
    png, r = render(a, "render", extra)
    return [r, image_content(png, a.get("crop"), a.get("max_px", 1000))]


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
      "spans_under_skin / span_guides: guides whose straight span BETWEEN two CVs dips > 1 mm under the skin (long root "
      "segments across the forehead; drawn as broken specks). Those are not pushed: delete them (where indices).",
      obj({"where": WHERE, "standoff_mm": {"type": "number"}, "depth_mm": {"type": "number"}, "delete": {"type": "boolean"},
           "list_only": {"type": "boolean"}, **SHOW}))
def t_push(a):
    return edit(a, {"do": "push_out", "where": a.get("where", {"all": True}),
                    **{k: a[k] for k in ("standoff_mm", "depth_mm", "delete", "list_only") if k in a}})


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


@tool("studio_rebuild_from_cards", "DRASTIC, opt-in: delete ALL guides in 'where' (default all) and trace new ones from "
      "the game's own hair cards (growth.py idea: lanes along each card's root->tip texture direction, trimmed where the "
      "card alpha has no hair; roots = Poisson points on the scalp under visible card hair, spacing_mm apart, a "
      "target count, or by default spaced so the hair budget matches the build as opened (budget_target 1.0); each root follows a card lane passing near it; stacked duplicate cards dropped; strands through "
      "the head / over the eyes rejected; new growth faces = Delaunay of the roots). For builds that look nothing like "
      "vanilla (too few / wrong guides). One undoable op ('rebuild', Studio-only: the HairLab pipeline ignores it). "
      "Spacing is raised until the budget ratio is <= the limit (1.2) unless ignore_budget. replace false = add "
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
           "fill": {"type": "boolean", "description": "after gathering, densify a thin crown / sides: the widest gap between strands lying on the skin (card footprint, fill_reach_mm 20, above head centre - fill_z_below_mm 60, not ears / under the bun) gets a new guide with its own bun path, repeated while a gap is wider than cover_mm (7), fill_spacing_mm (8) apart, up to fill_max_new (60), within max_ratio (1.2) of the budget"},
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
           "min_coverage": {"type": "number"}}))
def t_band_sim(a):
    keys = ("amounts", "seconds", "repeats", "wind_noise", "margin_mm", "worst", "sway_below_z", "mesh", "z_range", "max_height_mm", "min_coverage")
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
      "hair renders. straighten: true plants along a smoothed line (local mean over smooth_mm 12). Budget: refuses when "
      "the budget ratio is above max_ratio (1.2) and drops the outermost new guides until it is back under. list_only: "
      "report the planned roots without planting. Then studio_compare front + both 3/4 sides.",
      obj({"part": {"type": "string", "enum": ["all", "front", "left_temple", "right_temple", "temples", "ears", "sides"]},
           "rows": {"type": "integer"}, "offsets_mm": {"type": "array"}, "spacing_mm": {"type": "number"},
           "min_length": {"type": "number"}, "taper": {"type": "number"}, "lift_mm": {"type": "number"},
           "span_mm": {"type": "number"}, "template_mm": {"type": "number"}, "straighten": {"type": "boolean"},
           "smooth_mm": {"type": "number"}, "max_new": {"type": "integer"}, "max_ratio": {"type": "number"},
           "covered_mm": {"type": "number"}, "brow_clear_mm": {"type": "number"}, "ear_pad_mm": {"type": "number"},
           "front_deg": {"type": "number"}, "temple_deg": {"type": "number"}, "ear_deg": {"type": "number"},
           "edge_faces": {"type": "boolean", "description": "default true: new roots past the growth-mesh edge get growth faces (false = split the nearest face only)"},
           "list_only": {"type": "boolean"}, **SHOW}))
def t_hairline_fill(a):
    return edit(a, {"do": "hairline_fill", **{k: a[k] for k in (
        "part", "rows", "offsets_mm", "spacing_mm", "min_length", "taper", "lift_mm", "span_mm", "template_mm", "straighten",
        "smooth_mm", "max_new", "max_ratio", "covered_mm", "brow_clear_mm", "ear_pad_mm", "front_deg", "temple_deg", "ear_deg",
        "edge_faces", "list_only") if k in a}})


@tool("studio_free_budget", "Make room in the hair budget so fills can land when the ratio is pinned at the limit (1.2). "
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
      "Budget: refuses above max_ratio (1.2), drops the last copies to stay under. list_only: plan only.",
      obj({"side": {"type": "string", "enum": ["both", "left", "right"]}, "where": WHERE, "spacing_mm": {"type": "number"},
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
        "ear_pad_mm", "max_new", "max_ratio", "ignore_budget", "shots", "sample_px", "list_only") if k in a}}
    cmd.update(cam_args(a))   # a camera = closed loop on that render (shot / view / eye)
    return edit(a, cmd)


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
@tool("studio_look_get", "Current look / physics values (fur parameter paths). keys: optional list of path prefixes.",
      obj({"keys": {"type": "array", "items": {"type": "string"}}}))
def t_lookget(a):
    return S.call({"do": "look_get", "keys": a.get("keys", [])})


@tool("studio_look_set", "Set look / physics values {param path: value}, e.g. graphicalMaterials.color.rootColor, "
      "physicalMaterials.stiffness.tipStiffness, graphicalMaterials.width.rootWidth, density... Caps are re-enforced.",
      obj({"set": {"type": "object"}, **SHOW}))
def t_lookset(a):
    sv = a.get("set")
    if sv is None:   # flat form {path: value, ...} accepted too
        sv = {k: v for k, v in a.items() if "." in k}
    if not sv:
        raise RuntimeError('nothing to set: use {"set": {"graphicalMaterials.color.rootColor": [r,g,b]}} (or the flat '
                           'form with dotted paths); got keys: ' + ", ".join(a.keys()))
    chk = S.call({"do": "look_get", "keys": list(sv.keys())}).get("values", {})
    bad = [k for k, v in chk.items() if isinstance(v, dict) and v.get("error")]
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


@tool("studio_simulate", "Run physics: preset idle / walk / run / turn / nod / spin / wind / rest for seconds "
      "(settle = rest pose). on=false keeps hair on the rest shape.",
      obj({"preset": {"type": "string"}, "seconds": {"type": "number"}, "on": {"type": "boolean"}, "reset": {"type": "boolean"},
           "amount": {"type": "number", "description": "motion / wind strength multiplier (1)"}, "speed": {"type": "number"}}))
def t_sim(a):
    if "on" in a:
        return S.call({"do": "simulate", "on": a["on"]})
    p = a.get("preset", "rest")
    if p == "rest":
        return S.call({"do": "settle", "seconds": a.get("seconds", 2)})
    return S.call({"do": "motion", "preset": p, "seconds": a.get("seconds", 2), "reset": a.get("reset", False),
                   "amount": a.get("amount", 1.0), "speed": a.get("speed", 1.0)})


@tool("studio_dump_geo", "Read-only: write the groom as it is now (alive guides in order with all control points, edit frame, "
      "faces by alive index with per-corner uv, K) to a JSON file (path; default <out>/geo.json). For offline checks / diffs. "
      "guides[n] is NOT Studio guide n: use index[n] (its Studio index) before passing it to any other tool.",
      obj({"path": {"type": "string"}}))
def t_dump_geo(a):
    return S.call({"do": "dump_geo", "path": a.get("path") or os.path.join(S.out, "geo.json")})


# --- saving
@tool("studio_save", "Save the current edits under a NEW variant name into build/desktop_studio/mcp/<name>/ "
      "(edits.json, look, project). Never overwrites approved builds. export: true also builds the mod zip "
      "(not installed); release: true adds the Core requirement.",
      obj({"name": {"type": "string"}, "export": {"type": "boolean"}, "release": {"type": "boolean"}}, ["name"]))
def t_save(a):
    d = os.path.join(S.out, a["name"])
    os.makedirs(d, exist_ok=True)
    r = S.call({"do": "save_edits", "path": os.path.join(d, "edits.json")})
    r["project"] = S.call({"do": "save_project", "path": os.path.join(d, a["name"] + ".groomproj.json")})
    if a.get("export"):
        r["export"] = S.call({"do": "export", "tag": a["name"], "release": a.get("release", False)}, timeout=1200)
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
                "serverInfo": {"name": "hairwork-studio", "version": "0.2.1"},
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
