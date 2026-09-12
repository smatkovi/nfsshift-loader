#!/usr/bin/env python3
"""Turn framebuffer dumps (shotNNN.ppm) into a contact sheet PNG."""
import glob, sys
from PIL import Image
d, out = sys.argv[1], sys.argv[2]
first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
fs = sorted(glob.glob(d + '/shot*.ppm'))[first:first + 8]
if not fs:
    sys.exit('no shots')
W, H = 594, 270
ims = [Image.open(f).resize((W, H)) for f in fs]
sheet = Image.new('RGB', (W * 2, H * ((len(ims) + 1) // 2)))
for i, im in enumerate(ims):
    sheet.paste(im, ((i % 2) * W, (i // 2) * H))
sheet.save(out)
print(len(fs), 'frames ->', out)
