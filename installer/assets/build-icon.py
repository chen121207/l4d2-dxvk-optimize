"""Convert the generated master artwork into a Windows multi-size application icon.

This is a deterministic packaging conversion, not a runtime dependency.
Requires Pillow. Run from any working directory with:
    python installer/assets/build-icon.py
"""

from pathlib import Path

from PIL import Image


ASSETS = Path(__file__).resolve().parent
SOURCE = ASSETS / "l4d2-dxvk-rt-source.png"
PREVIEW = ASSETS / "l4d2-dxvk-rt.png"
ICON = ASSETS / "l4d2-dxvk-rt.ico"
SIZES = (16, 24, 32, 48, 64, 128, 256)


def main() -> None:
    image = Image.open(SOURCE).convert("RGBA")
    # The master has almost-invisible glow pixels far outside the tile. Use
    # the solid artwork bounds so the Windows taskbar icon remains legible.
    alpha = image.getchannel("A")
    solid = alpha.point(lambda value: 255 if value > 8 else 0)
    bounds = solid.getbbox()
    if bounds is None:
        raise ValueError("Master artwork has no visible alpha content")

    left, top, right, bottom = bounds
    side = max(right - left, bottom - top)
    padding = max(8, round(side * 0.05))
    side += 2 * padding
    center_x = (left + right) // 2
    center_y = (top + bottom) // 2
    box = (center_x - side // 2, center_y - side // 2,
           center_x - side // 2 + side, center_y - side // 2 + side)
    tile = image.crop(box).resize((256, 256), Image.Resampling.LANCZOS)
    tile.save(PREVIEW, optimize=True)
    tile.save(ICON, format="ICO", sizes=[(size, size) for size in SIZES])

    with Image.open(ICON) as icon:
        actual_sizes = {size[0] for size in icon.ico.sizes()}
    if actual_sizes != set(SIZES):
        raise ValueError(f"ICO sizes differ from requested sizes: {actual_sizes}")
    print(f"Preview: {PREVIEW}")
    print(f"Windows icon: {ICON}; sizes: {', '.join(map(str, SIZES))}")


if __name__ == "__main__":
    main()
