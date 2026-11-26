import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

here = os.path.dirname(os.path.abspath(__file__))
font_path = os.path.join(here, "..", "fonts", "CascadiaMono.ttf")
out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "ccc.iconset")
os.makedirs(out_dir, exist_ok=True)

S = 1024
inset, side, radius = 100, 824, 185
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(shadow).rounded_rectangle((inset, inset + 12, inset + side, inset + side + 12), radius, fill=(0, 0, 0, 110))
img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(18)))

body = Image.new("RGBA", (S, S), (0, 0, 0, 0))
top, bottom = (44, 46, 52), (18, 19, 22)
grad = Image.new("RGBA", (S, S))
gd = ImageDraw.Draw(grad)
for y in range(S):
    t = min(max((y - inset) / side, 0), 1)
    gd.line((0, y, S, y), fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle((inset, inset, inset + side, inset + side), radius, fill=255)
body.paste(grad, (0, 0), mask)
img.alpha_composite(body)

d = ImageDraw.Draw(img)
d.rounded_rectangle((inset, inset, inset + side, inset + side), radius, outline=(255, 255, 255, 38), width=4)
bar = inset + 150
d.line((inset + 40, bar, inset + side - 40, bar), fill=(255, 255, 255, 30), width=3)
for i, c in enumerate([(255, 95, 87), (254, 188, 46), (40, 200, 64)]):
    cx = inset + 95 + i * 62
    d.ellipse((cx - 20, inset + 75 - 20, cx + 20, inset + 75 + 20), fill=c + (255,))

font = ImageFont.truetype(font_path, 300)
try:
    font.set_variation_by_axes([700])
except Exception:
    pass
d.text((inset + 110, inset + 250), ">", font=font, fill=(235, 238, 242, 255))
d.rectangle((inset + 315, inset + 455, inset + 505, inset + 500), fill=(58, 150, 221, 255))

for px in (16, 32, 128, 256, 512):
    img.resize((px, px), Image.LANCZOS).save(os.path.join(out_dir, f"icon_{px}x{px}.png"))
    img.resize((px * 2, px * 2), Image.LANCZOS).save(os.path.join(out_dir, f"icon_{px}x{px}@2x.png"))
