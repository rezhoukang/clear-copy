"""Stage the approved multi-size icons using only the standard library."""
from pathlib import Path
from shutil import copyfile


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[1]
    output = root / 'build'
    output.mkdir(exist_ok=True)
    for name in ('clear-copy.ico', 'clear-copy-paused.ico'):
        copyfile(root / 'assets' / name, output / name)
