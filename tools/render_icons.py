"""Regenerate shipped icons from SVG (requires CairoSVG and Pillow)."""
from io import BytesIO
from pathlib import Path

import cairosvg
from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets'
SIZES = [(size, size) for size in (16, 20, 24, 32, 48, 64, 128, 256)]


def render(svg, destination):
    png = cairosvg.svg2png(bytestring=svg.encode('utf-8'), output_width=768, output_height=768)
    icon = Image.open(BytesIO(png)).convert('RGBA').resize((256, 256), Image.Resampling.LANCZOS)
    icon.save(destination, sizes=SIZES)


if __name__ == '__main__':
    source = (ASSETS / 'clear-copy.svg').read_text(encoding='utf-8-sig')
    render(source, ASSETS / 'clear-copy.ico')
    render(source.replace('#E6C17A', '#A6ADB4'), ASSETS / 'clear-copy-paused.ico')
