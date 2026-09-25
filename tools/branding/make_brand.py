"""Generate the WinDosDX branding bitmaps that replace ReactOS artwork.

Style matches dll/branding/rosbrand/resources/bmp/brand_banner.bmp: a rounded
square with a diagonal light-to-dark blue split and a dark Segoe UI Bold
"WinDosDX" wordmark.

Outputs:
  rosbitmap.bmp, rosbitmap_mask.bmp -> base/system/userinit/res/  (LiveCD logo)
  158.bmp                           -> base/shell/explorer/res/bmp/ (Start menu banner)
  143.bmp                           -> base/shell/explorer/res/bmp/ (Start button)
  146-153.bmp, 170.bmp, 171.bmp     -> base/shell/explorer/res/bmp/ (Taskbar and
                                       Start Menu Properties previews, patched
                                       in place: Start flag and side banner)

Usage (needs Pillow and Segoe UI Bold, i.e. a Windows host):
  python tools/branding/make_brand.py [OUTDIR]     # write bitmaps + previews to OUTDIR
  python tools/branding/make_brand.py --install    # also copy bitmaps into the tree

The build does not track bitmaps included by .rc files, so after --install
touch base/system/userinit/userinit.rc and base/shell/explorer/explorer.rc
before rebuilding.
"""
import argparse
import os
import shutil
from PIL import Image, ImageDraw, ImageFont, ImageFilter

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
_parser = argparse.ArgumentParser(description="Generate WinDosDX branding bitmaps.")
_parser.add_argument("outdir", nargs="?", default=os.path.join(REPO, "output-branding"),
                     help="where to write bitmaps and previews (default: output-branding/)")
_parser.add_argument("--install", action="store_true",
                     help="copy the generated bitmaps to their source-tree locations")
ARGS = _parser.parse_args()
OUT = ARGS.outdir
os.makedirs(OUT, exist_ok=True)
INSTALL = {
    "rosbitmap.bmp": "base/system/userinit/res",
    "rosbitmap_mask.bmp": "base/system/userinit/res",
    "158.bmp": "base/shell/explorer/res/bmp",
    "143.bmp": "base/shell/explorer/res/bmp",
}
FONT_B = "C:/Windows/Fonts/segoeuib.ttf"
LIGHT = (127, 175, 212)
DARK = (57, 107, 154)
TEXT = (61, 69, 76)
SS = 4  # supersampling factor


def icon(size):
    """Rounded square, light top-left triangle and dark bottom-right, RGBA."""
    s = size * SS
    grad = Image.new("RGB", (s, s))
    px = grad.load()
    for y in range(s):
        for x in range(s):
            t = (x + y) / (2 * s)
            if x + y < s:  # upper-left triangle: lighter
                a, b = LIGHT, (100, 150, 196)
            else:
                a, b = (78, 130, 180), DARK
            px[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, s - 1, s - 1), radius=s // 7, fill=255)
    im = grad.convert("RGBA")
    im.putalpha(mask)
    return im.resize((size, size), Image.LANCZOS)


def livecd_logo():
    """320x159 logo + 8-bit grayscale alpha mask for userinit's LiveCD dialog."""
    W, H = 320, 159
    canvas = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    ic = icon(54).resize((54 * SS, 54 * SS), Image.LANCZOS)
    font = ImageFont.truetype(FONT_B, 38 * SS)
    d = ImageDraw.Draw(canvas)
    text = "WinDosDX"
    tb = d.textbbox((0, 0), text, font=font)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]
    gap = 12 * SS
    total = ic.width + gap + tw
    x0 = (W * SS - total) // 2
    y_icon = (H * SS - ic.height) // 2
    canvas.alpha_composite(ic, (x0, y_icon))
    ty = (H * SS - th) // 2 - tb[1]
    d.text((x0 + ic.width + gap, ty), text, font=font, fill=TEXT + (255,))
    img = canvas.resize((W, H), Image.LANCZOS)

    # Soft white glow behind everything, like the original ReactOS logo mask,
    # so the logo reads on both the grey dialog and darker themes.
    alpha = img.getchannel("A")
    glow = alpha.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(3))
    glow = glow.point(lambda v: min(255, int(v * 0.55)))
    base = Image.new("RGBA", (W, H), (255, 255, 255, 0))
    base.putalpha(glow)
    base.alpha_composite(img)

    # livecd.c premultiplies by the mask itself, so store straight
    # (non-premultiplied) colour here.
    base.convert("RGB").save(f"{OUT}/rosbitmap.bmp")

    # Palette index == gray value, so the mask stays exact.
    mask = Image.frombytes("P", (W, H), base.getchannel("A").tobytes())
    mask.putpalette([c for i in range(256) for c in (i, i, i)])
    mask.save(f"{OUT}/rosbitmap_mask.bmp")
    base.save(f"{OUT}/preview_logo.png")


def start_banner():
    """21x233 vertical Start menu banner: blue gradient, rotated wordmark, icon."""
    W, H = 21, 233
    top, bottom = (138, 179, 219), (90, 127, 177)
    im = Image.new("RGB", (W * SS, H * SS))
    d = ImageDraw.Draw(im)
    for y in range(H * SS):
        t = y / (H * SS - 1)
        d.line([(0, y), (W * SS, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))

    ic_size = 15
    ic = icon(ic_size).resize((ic_size * SS, ic_size * SS), Image.LANCZOS)
    ic_y = (H - 3 - ic_size) * SS
    im.paste(ic, ((W - ic_size) * SS // 2, ic_y), ic)

    font = ImageFont.truetype(FONT_B, 15 * SS)
    text = "WinDosDX"
    tb = font.getbbox(text)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]
    layer = Image.new("RGBA", (tw + 8 * SS, th + 8 * SS), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((4 * SS - tb[0], 4 * SS - tb[1]), text, font=font, fill=(255, 255, 255, 255))
    layer = layer.rotate(90, expand=True)
    lx = (W * SS - layer.width) // 2
    ly = ic_y - 3 * SS - layer.height
    im.paste(layer, (lx, ly), layer)
    im = im.resize((W, H), Image.LANCZOS)
    im.save(f"{OUT}/158.bmp")
    im.resize((W * 3, H * 3), Image.NEAREST).save(f"{OUT}/preview_banner.png")


def start_button():
    """25x20 Start button icon (143.bmp): 32bpp BGRA, straight alpha, bottom-up,
    plain BITMAPINFOHEADER - the same layout as the original file."""
    import struct
    W, H = 25, 20
    ic = icon(18)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    im.alpha_composite(ic, ((W - 18) // 2, (H - 18) // 2))
    rows = []
    for y in range(H - 1, -1, -1):
        row = bytearray()
        for x in range(W):
            r, g, b, a = im.getpixel((x, y))
            if a == 0:
                r = g = b = 0
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    pixels = b"".join(rows)
    info = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 32, 0, len(pixels), 3780, 3780, 0, 0)
    header = struct.pack("<2sIHHI", b"BM", 14 + len(info) + len(pixels), 0, 0, 14 + len(info))
    with open(f"{OUT}/143.bmp", "wb") as fp:
        fp.write(header + info + pixels)
    im.resize((W * 10, H * 10), Image.NEAREST).save(f"{OUT}/preview_start.png")


def patch_previews():
    """Replace the ReactOS Start flag (and the 171 side banner) inside the
    Taskbar and Start Menu Properties preview bitmaps. Each patch fills a fixed
    rectangle with its surrounding colour and redraws, so re-running is
    idempotent."""
    src_dir = os.path.join(REPO, "base/shell/explorer/res/bmp")
    face = (212, 208, 200)
    for n in range(146, 154):
        # Taskbar previews: flag drawn at 1:1 on the Start button.
        im = Image.open(os.path.join(src_dir, f"{n}.bmp")).convert("RGB")
        ImageDraw.Draw(im).rectangle((3, 13, 19, 28), fill=face)
        ic = icon(14)
        im.paste(ic, (5, 14), ic)
        im.save(f"{OUT}/{n}.bmp")
    banner = Image.open(f"{OUT}/158.bmp").convert("RGB")
    for n in (170, 171):
        # Start menu previews: scaled-down screenshots of the whole screen.
        im = Image.open(os.path.join(src_dir, f"{n}.bmp")).convert("RGB")
        bg = im.getpixel((3, 175))
        ImageDraw.Draw(im).rectangle((4, 173, 11, 177), fill=bg)
        ic = icon(5)
        im.paste(ic, (5, 173), ic)
        if n == 171:
            im.paste(banner.resize((9, 101), Image.LANCZOS), (2, 69))
        im.save(f"{OUT}/{n}.bmp")
        INSTALL[f"{n}.bmp"] = "base/shell/explorer/res/bmp"
    for n in range(146, 154):
        INSTALL[f"{n}.bmp"] = "base/shell/explorer/res/bmp"


livecd_logo()
start_banner()
start_button()
patch_previews()
print("wrote bitmaps and previews to", OUT)

if ARGS.install:
    for name, dest in INSTALL.items():
        shutil.copyfile(os.path.join(OUT, name), os.path.join(REPO, dest, name))
        print("installed", f"{dest}/{name}")
