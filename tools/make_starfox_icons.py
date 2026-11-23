"""Generate original Corneria Tactical HUD icon & Arwing emblem artwork for SteelTemple Star Fox.

Zero copyrighted game assets: 100% procedural high-resolution supersampled vector artwork rendered
with Pillow. Emits:
  runtime/starfox.ico            Multi-size Windows icon (16..256 px) for starfox.exe & Setup Wizard
  runtime/starfox.png            512 px rounded tactical space-navy Arwing tile (Setup header & Launcher)
  runtime/starfox_emblem.png     512 px floating Arwing + hexagonal HUD lock-on crest on alpha
  launcher/assets/starfox.ico    Launcher executable icon (synced from runtime/starfox.ico)
"""

from __future__ import annotations

import math
import shutil
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent


def _pt(cx: float, cy: float, s: float, x: float, y: float) -> tuple[float, float]:
    return (cx + x * s, cy + y * s)


def _draw_hex(draw: ImageDraw.ImageDraw, cx: float, cy: float, r: float, rot: float, outline, width: int) -> None:
    pts = [
        (cx + r * math.cos(rot + i * math.pi / 3.0), cy + r * math.sin(rot + i * math.pi / 3.0))
        for i in range(6)
    ]
    draw.polygon(pts, outline=outline)
    for i in range(6):
        draw.line([pts[i], pts[(i + 1) % 6]], fill=outline, width=width)


def _draw_arwing(img: Image.Image, cx: float, cy: float, s: float) -> None:
    """Draw a sleek, razor-sharp metallic Arwing fighter sweeping upward with glowing emerald thruster."""
    # Soft thruster + wingtip plasma bloom layer
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    # Main engine exhaust plume (emerald-cyan core + gold inner flare)
    gd.polygon(
        [
            _pt(cx, cy, s, -0.09, 0.30),
            _pt(cx, cy, s, 0.09, 0.30),
            _pt(cx, cy, s, 0.03, 0.72),
            _pt(cx, cy, s, 0.00, 0.80),
            _pt(cx, cy, s, -0.03, 0.72),
        ],
        fill=(0, 229, 160, 210),
    )
    gd.ellipse(
        [cx - 0.24 * s, cy + 0.14 * s, cx + 0.24 * s, cy + 0.62 * s],
        fill=(0, 229, 160, 110),
    )
    # Subtle laser cannon tip glows
    for sign in (-1.0, 1.0):
        tx, ty = _pt(cx, cy, s, sign * 0.22, -0.14)
        gd.ellipse([tx - 0.06 * s, ty - 0.06 * s, tx + 0.06 * s, ty + 0.06 * s], fill=(74, 247, 194, 150))
    glow = glow.filter(ImageFilter.GaussianBlur(radius=max(2, int(s * 0.055))))
    img.alpha_composite(glow)

    d = ImageDraw.Draw(img)

    # Outer swept main wings (dark titanium underside -> bright silver-white topside)
    for sign in (-1.0, 1.0):
        # Rear shadow sweep
        d.polygon(
            [
                _pt(cx, cy, s, sign * 0.14, -0.06),
                _pt(cx, cy, s, sign * 0.84, 0.34),
                _pt(cx, cy, s, sign * 0.78, 0.44),
                _pt(cx, cy, s, sign * 0.18, 0.24),
            ],
            fill=(112, 134, 164, 255),
        )
        # Main silver wing face
        d.polygon(
            [
                _pt(cx, cy, s, sign * 0.14, -0.10),
                _pt(cx, cy, s, sign * 0.85, 0.31),
                _pt(cx, cy, s, sign * 0.72, 0.36),
                _pt(cx, cy, s, sign * 0.16, 0.17),
            ],
            fill=(220, 232, 246, 255) if sign < 0 else (188, 206, 228, 255),
        )
        # Cobalt-blue G-Diffuser wing stripe
        d.polygon(
            [
                _pt(cx, cy, s, sign * 0.26, -0.01),
                _pt(cx, cy, s, sign * 0.74, 0.27),
                _pt(cx, cy, s, sign * 0.63, 0.31),
                _pt(cx, cy, s, sign * 0.24, 0.12),
            ],
            fill=(30, 118, 236, 255) if sign < 0 else (20, 92, 202, 255),
        )
        # Inner laser pod + forward cannon housing
        d.polygon(
            [
                _pt(cx, cy, s, sign * 0.19, -0.22),
                _pt(cx, cy, s, sign * 0.26, -0.06),
                _pt(cx, cy, s, sign * 0.27, 0.27),
                _pt(cx, cy, s, sign * 0.17, 0.24),
            ],
            fill=(54, 146, 248, 255) if sign < 0 else (32, 108, 214, 255),
        )
        # Upper stabilizer fin
        d.polygon(
            [
                _pt(cx, cy, s, sign * 0.16, 0.04),
                _pt(cx, cy, s, sign * 0.44, 0.46),
                _pt(cx, cy, s, sign * 0.34, 0.49),
                _pt(cx, cy, s, sign * 0.14, 0.24),
            ],
            fill=(164, 184, 210, 255) if sign < 0 else (130, 152, 180, 255),
        )
        # Wing outline highlight
        d.line(
            [_pt(cx, cy, s, sign * 0.14, -0.10), _pt(cx, cy, s, sign * 0.85, 0.31)],
            fill=(245, 251, 255, 235),
            width=max(2, int(s * 0.014)),
        )

    # Central fuselage — left lit silver facet, right shaded titanium facet
    d.polygon(
        [
            _pt(cx, cy, s, 0.00, -0.78),
            _pt(cx, cy, s, -0.17, 0.14),
            _pt(cx, cy, s, -0.13, 0.34),
            _pt(cx, cy, s, 0.00, 0.38),
        ],
        fill=(238, 245, 254, 255),
    )
    d.polygon(
        [
            _pt(cx, cy, s, 0.00, -0.78),
            _pt(cx, cy, s, 0.17, 0.14),
            _pt(cx, cy, s, 0.13, 0.34),
            _pt(cx, cy, s, 0.00, 0.38),
        ],
        fill=(172, 192, 216, 255),
    )
    # Squadron gold-crimson nose chevron accent
    d.polygon(
        [
            _pt(cx, cy, s, 0.00, -0.52),
            _pt(cx, cy, s, -0.08, -0.26),
            _pt(cx, cy, s, 0.00, -0.32),
            _pt(cx, cy, s, 0.08, -0.26),
        ],
        fill=(255, 200, 69, 255),
    )
    # Cockpit canopy (deep orbital emerald-tinted glass + specular highlight)
    d.polygon(
        [
            _pt(cx, cy, s, 0.00, -0.26),
            _pt(cx, cy, s, -0.075, 0.00),
            _pt(cx, cy, s, -0.055, 0.17),
            _pt(cx, cy, s, 0.055, 0.17),
            _pt(cx, cy, s, 0.075, 0.00),
        ],
        fill=(10, 42, 64, 255),
    )
    d.polygon(
        [
            _pt(cx, cy, s, 0.00, -0.22),
            _pt(cx, cy, s, -0.055, 0.00),
            _pt(cx, cy, s, -0.02, 0.13),
            _pt(cx, cy, s, 0.015, -0.05),
        ],
        fill=(0, 229, 160, 210),
    )
    # Bright cockpit glint
    d.line(
        [_pt(cx, cy, s, -0.01, -0.18), _pt(cx, cy, s, -0.045, -0.02)],
        fill=(220, 255, 245, 240),
        width=max(2, int(s * 0.014)),
    )

    # Thruster nozzle ring & bright white-emerald core
    d.polygon(
        [
            _pt(cx, cy, s, -0.09, 0.33),
            _pt(cx, cy, s, 0.09, 0.33),
            _pt(cx, cy, s, 0.06, 0.42),
            _pt(cx, cy, s, -0.06, 0.42),
        ],
        fill=(32, 46, 66, 255),
    )
    d.polygon(
        [
            _pt(cx, cy, s, -0.055, 0.39),
            _pt(cx, cy, s, 0.055, 0.39),
            _pt(cx, cy, s, 0.00, 0.65),
        ],
        fill=(196, 255, 236, 255),
    )
    # Crisp fuselage ridge line
    d.line(
        [_pt(cx, cy, s, 0.00, -0.78), _pt(cx, cy, s, 0.00, -0.26)],
        fill=(255, 255, 255, 220),
        width=max(2, int(s * 0.012)),
    )


def make_tile_icon(px: int = 512) -> Image.Image:
    """Rounded Corneria Tactical HUD icon tile with glowing Arwing fighter."""
    ss = px * 2
    img = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    margin = int(ss * 0.045)
    radius = int(ss * 0.21)

    # Rounded tile mask
    mask = Image.new("L", (ss, ss), 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle([margin, margin, ss - margin, ss - margin], radius=radius, fill=255)

    # Deep orbital space-navy vertical gradient + emerald aurora glow
    bg = Image.new("RGBA", (ss, ss), (7, 17, 30, 255))
    bd = ImageDraw.Draw(bg)
    for y in range(ss):
        t = y / max(1, ss - 1)
        r = int(7 + 10 * t)
        g = int(16 + 22 * (1.0 - abs(t - 0.55) * 1.4))
        b = int(30 + 28 * (1.0 - t * 0.45))
        bd.line([(0, y), (ss, y)], fill=(max(0, r), max(0, g), max(0, b), 255))

    aurora = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    ad = ImageDraw.Draw(aurora)
    ad.ellipse([ss * 0.14, ss * 0.12, ss * 0.86, ss * 0.84], fill=(0, 229, 160, 56))
    ad.ellipse([ss * 0.25, ss * 0.26, ss * 0.75, ss * 0.76], fill=(32, 134, 246, 52))
    aurora = aurora.filter(ImageFilter.GaussianBlur(radius=int(ss * 0.10)))
    bg.alpha_composite(aurora)

    # Subtle tactical starfield & hexagonal radar ring inside tile
    sd = ImageDraw.Draw(bg)
    stars = [
        (0.18, 0.20, 4), (0.82, 0.18, 5), (0.16, 0.76, 4), (0.84, 0.78, 5),
        (0.28, 0.15, 3), (0.72, 0.86, 3), (0.86, 0.45, 3), (0.14, 0.48, 3),
    ]
    for fx, fy, sr in stars:
        x, y = fx * ss, fy * ss
        sd.ellipse([x - sr, y - sr, x + sr, y + sr], fill=(200, 245, 255, 190))
    _draw_hex(sd, ss * 0.5, ss * 0.50, ss * 0.37, math.pi / 6.0, (0, 229, 160, 90), max(3, ss // 220))
    _draw_hex(sd, ss * 0.5, ss * 0.50, ss * 0.31, math.pi / 6.0, (255, 200, 69, 65), max(2, ss // 300))

    # Corner HUD lock-on brackets
    bpad = int(ss * 0.13)
    blen = int(ss * 0.085)
    bw = max(4, ss // 140)
    bcol = (0, 229, 160, 175)
    for sx, sy in ((0, 0), (1, 0), (0, 1), (1, 1)):
        x0 = bpad if sx == 0 else ss - bpad
        y0 = bpad if sy == 0 else ss - bpad
        dx = blen if sx == 0 else -blen
        dy = blen if sy == 0 else -blen
        sd.line([(x0, y0), (x0 + dx, y0)], fill=bcol, width=bw)
        sd.line([(x0, y0), (x0, y0 + dy)], fill=bcol, width=bw)

    img.paste(bg, (0, 0), mask)

    # Center Arwing fighter
    _draw_arwing(img, ss * 0.50, ss * 0.51, ss * 0.42)

    # Crisp double tactical border (outer metallic titanium + inner emerald-cyan rim)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(
        [margin, margin, ss - margin, ss - margin],
        radius=radius,
        outline=(0, 229, 160, 235),
        width=max(6, ss // 85),
    )
    inset = margin + max(10, ss // 60)
    d.rounded_rectangle(
        [inset, inset, ss - inset, ss - inset],
        radius=max(8, radius - max(10, ss // 60)),
        outline=(255, 200, 69, 115),
        width=max(3, ss // 200),
    )

    return img.resize((px, px), Image.Resampling.LANCZOS)


def make_emblem(px: int = 512) -> Image.Image:
    """Floating Corneria Star Fox tactical crest + Arwing on transparent background."""
    ss = px * 2
    img = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))

    # Radial orbital glow halo
    halo = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    hd = ImageDraw.Draw(halo)
    hd.ellipse([ss * 0.14, ss * 0.14, ss * 0.86, ss * 0.86], fill=(0, 229, 160, 48))
    hd.ellipse([ss * 0.26, ss * 0.26, ss * 0.74, ss * 0.74], fill=(42, 138, 255, 58))
    halo = halo.filter(ImageFilter.GaussianBlur(radius=int(ss * 0.08)))
    img.alpha_composite(halo)

    d = ImageDraw.Draw(img)
    cx, cy = ss * 0.50, ss * 0.50

    # Tactical concentric HUD rings + tick marks
    r_out = ss * 0.42
    r_mid = ss * 0.34
    d.ellipse([cx - r_out, cy - r_out, cx + r_out, cy + r_out], outline=(0, 229, 160, 150), width=max(4, ss // 180))
    _draw_hex(d, cx, cy, r_mid, math.pi / 6.0, (74, 247, 194, 190), max(4, ss // 190))
    _draw_hex(d, cx, cy, ss * 0.28, 0.0, (255, 200, 69, 135), max(3, ss // 240))

    # Cardinal targeting reticles
    for deg in (0, 90, 180, 270):
        ang = math.radians(deg)
        p0 = (cx + math.cos(ang) * (r_out - ss * 0.03), cy + math.sin(ang) * (r_out - ss * 0.03))
        p1 = (cx + math.cos(ang) * (r_out + ss * 0.035), cy + math.sin(ang) * (r_out + ss * 0.035))
        d.line([p0, p1], fill=(0, 229, 160, 220), width=max(5, ss // 150))

    # Swept Arwing centerpiece
    _draw_arwing(img, cx, cy + ss * 0.01, ss * 0.44)

    return img.resize((px, px), Image.Resampling.LANCZOS)


def main() -> None:
    tile = make_tile_icon(512)
    emblem = make_emblem(512)

    png_out = ROOT / "runtime" / "starfox.png"
    emb_out = ROOT / "runtime" / "starfox_emblem.png"
    ico_out = ROOT / "runtime" / "starfox.ico"
    launcher_ico = ROOT / "launcher" / "assets" / "starfox.ico"

    tile.save(png_out, format="PNG", optimize=True)
    emblem.save(emb_out, format="PNG", optimize=True)

    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    tile.save(ico_out, format="ICO", sizes=sizes)

    launcher_ico.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ico_out, launcher_ico)

    print(f"Wrote {png_out} ({png_out.stat().st_size} B)")
    print(f"Wrote {emb_out} ({emb_out.stat().st_size} B)")
    print(f"Wrote {ico_out} ({ico_out.stat().st_size} B)")
    print(f"Synced {launcher_ico}")


if __name__ == "__main__":
    main()
