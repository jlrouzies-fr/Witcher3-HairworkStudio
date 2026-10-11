r"""make_brand_assets - logo upscales, splash art and the app icon from the mod logo (the original file is not changed).

  py desktop_studio\tools\make_brand_assets.py            everything below
  py desktop_studio\tools\make_brand_assets.py --studio   only logo_studio.png (the other files are left as they are)

Inputs : desktop_studio\assets\hairwork_for_all_logo.jpg (566 x 369, the mod logo), assets\fonts\Cinzel.ttf
Outputs: assets\logo_lanczos.png   3x Lanczos + light denoise / sharpen of the whole picture (comparison)
         assets\logo_rebuilt.png   3x rebuild: charcoal gradient redrawn, the hair swoosh upscaled (Lanczos + denoise +
                                   unsharp), title / subtitle redrawn crisply with Cinzel, amber line as a glowing gradient
         assets\logo_hybrid.png    4x hybrid (splash / About): same rebuild, but the swoosh comes from the 4x AI upscale
                                   (4x-UltraSharp, made by ai_logo_upscale.py into build\desktop_studio\ai\logo\)
         assets\logo_studio.png    4x hybrid with the app's title HAIRWORK STUDIO (splash / About of Hairwork Studio);
                                   same swoosh, subtitle, amber line and proportions as logo_hybrid.png
         assets\icon.ico           16 / 24 / 32 / 48 / 64 / 128 / 256 (swoosh on charcoal, thin amber accent; the AI swoosh
                                   for 64 px and up when the AI upscale exists, else Lanczos)
         assets\icon_preview.png   the icon sizes side by side
AI swoosh: run  D:\AI-Upscale\venv-restyle\Scripts\python.exe desktop_studio\tools\ai_logo_upscale.py  first (local GPU);
without it the hybrid is skipped and the icon falls back to Lanczos. cv2 is used when present.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
AS = os.path.join(os.path.dirname(HERE), 'assets')
SRC = os.path.join(AS, 'hairwork_for_all_logo.jpg')
AI_SWOOSH = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'build', 'desktop_studio', 'ai', 'logo',
                         'logo_x4_ultrasharp.png')      # 4x AI upscale of the whole logo (only the swoosh is used)
FONT = os.path.join(AS, 'fonts', 'Cinzel.ttf')
WEIGHT = 560          # Cinzel is a variable font (400..900): the logo's strokes are a little heavier than Regular


def font(size):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_axes([WEIGHT])
    except Exception:
        pass
    return f
S = 3                                   # upscale factor
BG0, BG1 = (0x12, 0x14, 0x16), (0x1c, 0x1f, 0x22)
AMBER0, AMBER1 = (0xc8, 0x76, 0x1e), (0xf0, 0xa0, 0x40)

try:
    import cv2
except Exception:
    cv2 = None


def denoise_sharpen(img):
    a = np.asarray(img.convert('RGB'))
    if cv2 is not None:
        a = cv2.fastNlMeansDenoisingColored(a, None, 3, 3, 7, 21)
    im = Image.fromarray(a)
    return im.filter(ImageFilter.UnsharpMask(radius=2.0, percent=70, threshold=2))


def background(w, h):
    """soft charcoal gradient (darker towards the bottom corners, like the original)"""
    y = np.linspace(0, 1, h)[:, None]
    x = np.linspace(-1, 1, w)[None, :]
    t = np.clip(0.55 * y + 0.35 * np.abs(x) * y, 0, 1)
    c0, c1 = np.array(BG1, float), np.array(BG0, float) * 0.75
    a = c0[None, None, :] * (1 - t[..., None]) + c1[None, None, :] * t[..., None]
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))


def swoosh_layer(src, s=S, ai=None):
    """the silver hair swoosh: crop, s x upscale (ai: the 4x AI upscale of the whole logo, resized to s; else Lanczos +
    denoise + sharpen), alpha from brightness above the background"""
    box = (40, 70, 526, 196)
    if ai is not None:
        up = ai.crop(tuple(v * 4 for v in box)).convert('RGB')
        if s != 4:
            up = up.resize(((box[2] - box[0]) * s, (box[3] - box[1]) * s), Image.LANCZOS)
    else:
        crop = src.crop(box)
        up = denoise_sharpen(crop.resize((crop.width * s, crop.height * s), Image.LANCZOS))
    a = np.asarray(up, float)
    lum = a.mean(2)
    alpha = np.clip((lum - 34) / 70.0, 0, 1)
    # feather the crop border
    f = 10.0 * s
    fy = np.clip(np.minimum(np.arange(up.height), up.height - 1 - np.arange(up.height)) / f, 0, 1)[:, None]
    fx = np.clip(np.minimum(np.arange(up.width), up.width - 1 - np.arange(up.width)) / f, 0, 1)[None, :]
    alpha *= fy * fx
    rgba = np.dstack([a, alpha * 255]).astype(np.uint8)
    return Image.fromarray(rgba, 'RGBA'), (box[0] * s, box[1] * s)


def silver_text(draw_img, xy, text, size, tracking):
    """text with a silver vertical gradient and wide tracking, drawn letter by letter"""
    f = font(size)
    x, y = xy
    w_total = 0
    for ch in text:
        w_total += f.getlength(ch) + tracking
    return f, w_total - tracking


def draw_tracked(img, text, size, cx, baseline, tracking, gradient=((246, 247, 249), (176, 180, 186))):
    f = font(size)
    widths = [f.getlength(ch) for ch in text]
    total = sum(widths) + tracking * (len(text) - 1)
    x = cx - total / 2
    mask = Image.new('L', img.size, 0)
    md = ImageDraw.Draw(mask)
    asc = f.getmetrics()[0]
    for ch, w in zip(text, widths):
        md.text((x, baseline - asc), ch, font=f, fill=255)
        x += w + tracking
    h = img.size[1]
    g = np.linspace(0, 1, h)[:, None, None]
    c0, c1 = np.array(gradient[0], float), np.array(gradient[1], float)
    top, bot = baseline - asc, baseline
    t = np.clip((np.arange(h)[:, None, None] - top) / max(bot - top, 1), 0, 1)
    col = (c0 * (1 - t) + c1 * t) * np.ones((1, img.size[0], 1))
    layer = Image.fromarray(col.astype(np.uint8), 'RGB')
    img.paste(layer, (0, 0), mask)
    return total


def title_words(img, cx, baseline, big, small, tracking, fit_width=None):
    """HAIRWORK for ALL: two big words and a smaller 'FOR' (small caps look of the logo); fit_width scales it to the
    original's text width"""
    if fit_width:
        # sizes fixed by the original's cap heights; the tracking is solved so the line keeps the original width
        fonts = [font(big), font(small), font(big)]
        glyphs = sum(sum(f.getlength(c) for c in t) for t, f in zip(('HAIRWORK', 'FOR', 'ALL'), fonts))
        tracking = max(0.0, (fit_width - glyphs - big * 0.42 * 2) / (7 + 2 + 2))
    parts = [('HAIRWORK', big), ('FOR', small), ('ALL', big)]
    gap = big * 0.42
    fonts = [font(s) for _, s in parts]
    widths = [sum(f.getlength(c) for c in t) + tracking * (len(t) - 1) for (t, _), f in zip(parts, fonts)]
    total = sum(widths) + gap * 2
    x = cx - total / 2
    for (t, s), w in zip(parts, widths):
        draw_tracked(img, t, s, x + w / 2, baseline, tracking)
        x += w + gap


def title_studio(img, cx, baseline, big, tracking, gap):
    """HAIRWORK STUDIO: two words at the big size; letter spacing and word gap scaled from the original (STUDIO_TRACK / STUDIO_GAP), so the line
    is only a little wider than HAIRWORK for ALL"""
    f = font(big)
    parts = ('HAIRWORK', 'STUDIO')
    widths = [sum(f.getlength(c) for c in t) + tracking * (len(t) - 1) for t in parts]
    x = cx - (sum(widths) + gap) / 2
    for t, w in zip(parts, widths):
        draw_tracked(img, t, big, x + w / 2, baseline, tracking)
        x += w + gap


def amber_line(img, y, x0, x1, thick):
    w = img.size[0]
    line = Image.new('RGBA', img.size, (0, 0, 0, 0))
    a = np.zeros((img.size[1], w, 4), float)
    xs = np.arange(w)
    t = np.clip((xs - x0) / max(x1 - x0, 1), 0, 1)
    fade = np.sin(np.pi * t) ** 0.8 * ((xs >= x0) & (xs <= x1))
    mid = np.abs(t - 0.5) * 2
    col = np.array(AMBER1, float)[None, :] * (1 - mid[:, None]) + np.array(AMBER0, float)[None, :] * mid[:, None]
    for dy in range(-thick, thick + 1):
        yy = y + dy
        k = 1.0 if dy == 0 else 0.55 / abs(dy)
        a[yy, :, :3] = col
        a[yy, :, 3] = np.maximum(a[yy, :, 3], 255 * fade * k)
    line = Image.fromarray(a.astype(np.uint8), 'RGBA')
    glow = line.filter(ImageFilter.GaussianBlur(radius=thick * 4))
    img.alpha_composite(glow)
    img.alpha_composite(line)


def rebuilt(src, S=S, ai=None, studio=False):
    """S: scale (3 = logo_rebuilt, 4 = logo_hybrid); ai: the 4x AI upscale for the swoosh; studio: title HAIRWORK STUDIO
    (logo_studio.png) instead of HAIRWORK for ALL"""
    W, H = src.width * S, src.height * S
    img = background(W, H).convert('RGBA')
    sw, pos = swoosh_layer(src, S, ai)
    img.alpha_composite(sw, pos)
    # original: caps 209..241 px (cap height 32 = 0.72 x font size), 'FOR' smaller, line 30..531 px
    if studio:
        big = int(44.4 * S)
        # the tracking title_words solves for the original line (about 5 px at 1x), and its word gap
        fb, fs = font(big), font(int(30 * S))
        glyphs = sum(sum(f.getlength(c) for c in t) for t, f in zip(('HAIRWORK', 'FOR', 'ALL'), (fb, fs, fb)))
        tr = max(0.0, ((531 - 30) * S - glyphs - big * 0.42 * 2) / 11)
        title_studio(img, W / 2, 241 * S, big, tr * STUDIO_TRACK, big * 0.42 * STUDIO_GAP)
    else:
        title_words(img, W / 2, 241 * S, int(44.4 * S), int(30 * S), 0, fit_width=(531 - 30) * S)
    # subtitle: caps 261..273 px, 209..343 px wide
    sz = int(16.7 * S)
    f = font(sz)
    glyphs = sum(f.getlength(c) for c in 'The Witcher 3')
    tr = max(0.0, ((343 - 209) * S - glyphs) / 12)
    draw_tracked(img, 'The Witcher 3', sz, W / 2, 273 * S, tr, gradient=((222, 224, 228), (190, 194, 200)))
    amber_line(img, 293 * S, 60 * S, 506 * S, 2)
    return img.convert('RGB')


def icon_image(size, src, ai=None):
    """square icon: charcoal rounded tile, the swoosh (procedural strokes at small sizes), thin amber accent;
    ai: the 4x AI upscale of the logo (the swoosh is cut from it instead of a Lanczos 3x)"""
    N = 512
    img = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((8, 8, N - 8, N - 8), radius=96, fill=BG1 + (255,), outline=(60, 64, 70, 255), width=6)
    if size >= 64:
        if ai is not None:
            sw = ai.crop((60 * 4, 86 * 4, 482 * 4, 188 * 4)).convert('RGB')
        else:
            sw = src.crop((60, 86, 482, 188))
            sw = denoise_sharpen(sw.resize((sw.width * 3, sw.height * 3), Image.LANCZOS))
        a = np.asarray(sw, float)
        alpha = np.clip((a.mean(2) - 36) / 60.0, 0, 1) * 255
        layer = Image.fromarray(np.dstack([a, alpha]).astype(np.uint8), 'RGBA')
        layer = layer.resize((560, int(560 * layer.height / layer.width)), Image.LANCZOS)
        layer = layer.rotate(24, resample=Image.BICUBIC, expand=True)          # diagonal: fills the square
        k = 470 / max(layer.width, layer.height)
        layer = layer.resize((int(layer.width * k), int(layer.height * k)), Image.LANCZOS)
        img.alpha_composite(layer, ((N - layer.width) // 2, (N - layer.height) // 2 - 18))
    else:
        # three silver strands in an S-swoosh, readable at 16-48 px
        for k, (off, w, c) in enumerate(((-40, 34, 235), (0, 30, 205), (40, 26, 170))):
            pts = [(70 + t * 3.72, 256 + off + 70 * np.sin((t / 100.0) * 2 * np.pi * 0.9 + 0.6) * (1 - abs(t - 50) / 120)) for t in range(0, 101, 2)]
            d.line(pts, fill=(c, c + 2, c + 6, 255), width=w, joint='curve')
    d.line((150, 420, N - 150, 420), fill=AMBER1 + (255,), width=14 if size < 64 else 7)
    return img.resize((size, size), Image.LANCZOS)


STUDIO_TRACK, STUDIO_GAP = 0.85, 0.9  # logo_studio: letter spacing / word gap relative to the original title (a touch
#                                        tighter: STUDIO is wider than "for ALL"; keeps the line inside the swoosh span)


def load_ai(src):
    ai = Image.open(AI_SWOOSH).convert('RGB') if os.path.exists(AI_SWOOSH) else None
    if ai is not None and ai.size != (src.width * 4, src.height * 4):
        ai = None
    return ai


def make_studio(src, ai):
    if ai is None:
        print('no AI upscale at %s: logo_studio.png skipped' % AI_SWOOSH)
        return
    st = rebuilt(src, 4, ai, studio=True)
    st.save(os.path.join(AS, 'logo_studio.png'))
    print('wrote logo_studio.png %s (AI swoosh from %s)' % (st.size, AI_SWOOSH))


def main():
    src = Image.open(SRC).convert('RGB')
    if '--studio' in sys.argv[1:]:
        make_studio(src, load_ai(src))
        return
    lz = denoise_sharpen(src.resize((src.width * S, src.height * S), Image.LANCZOS))
    lz.save(os.path.join(AS, 'logo_lanczos.png'))
    rb = rebuilt(src)
    rb.save(os.path.join(AS, 'logo_rebuilt.png'))
    ai = load_ai(src)
    make_studio(src, ai)
    if ai is not None:
        hy = rebuilt(src, 4, ai)
        hy.save(os.path.join(AS, 'logo_hybrid.png'))
        print('wrote logo_hybrid.png %s (AI swoosh from %s)' % (hy.size, AI_SWOOSH))
    else:
        print('no AI upscale at %s: logo_hybrid.png skipped, icon uses Lanczos' % AI_SWOOSH)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    icons = [icon_image(s, src, ai) for s in sizes]
    icons[-1].save(os.path.join(AS, 'icon.ico'), sizes=[(s, s) for s in sizes], append_images=icons[:-1])
    prev = Image.new('RGBA', (sum(sizes) + 20 * len(sizes) + 20, 296), BG0 + (255,))
    x = 20
    for s, im in zip(sizes, icons):
        prev.alpha_composite(im, (x, 20 + (256 - s)))
        x += s + 20
    prev.convert('RGB').save(os.path.join(AS, 'icon_preview.png'))
    print('wrote logo_lanczos.png %s, logo_rebuilt.png %s, icon.ico (%s), icon_preview.png' % (lz.size, rb.size, sizes))


if __name__ == '__main__':
    main()
