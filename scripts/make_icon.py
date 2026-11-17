"""Draw Wave's icon: a three-band waveform and a playhead on a dark rounded square. Run once; the
images are committed (rb_waveform_core/data/icon.png and icon.ico)."""
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

S = 1024
out = Path(__file__).resolve().parents[1] / 'rb_waveform_core' / 'data'
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((40, 40, S - 40, S - 40), radius=210, fill=(14, 16, 20, 255), outline=(42, 46, 53, 255), width=10)
mid, left, right = S // 2, 130, S - 130
for x in range(left, right, 6):
    t = (x - left) / (right - left)
    beat = abs(math.sin(t * math.pi * 7)) ** 3
    envelope = .35 + .65 * math.sin(t * math.pi) ** .6
    low = 250 * envelope * (.45 + .55 * beat)
    mid_band = low * .62
    high = low * .26
    for height, colour in ((low, (0, 84, 226)), (mid_band, (255, 166, 0)), (high, (246, 236, 217))):
        d.rectangle((x, mid - height, x + 4, mid + height), fill=colour)
play = int(left + (right - left) * .32)
glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
ImageDraw.Draw(glow).rectangle((play - 10, 150, play + 10, S - 150), fill=(255, 255, 255, 120))
img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(12)))
d = ImageDraw.Draw(img)
d.rectangle((play - 7, 150, play + 7, S - 150), fill=(0, 0, 0, 255))
d.rectangle((play - 4, 150, play + 4, S - 150), fill=(248, 248, 248, 255))
img.save(out / 'icon.png')
img.save(out / 'icon.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print(out / 'icon.png')
