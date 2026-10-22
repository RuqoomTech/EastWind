"""Makes RA3-style material textures for the stock Zero Hour unit and building textures.

For every texture the game's W3D models use, writes <name>_nrm.dds (tangent-space normal map,
DirectX convention) and <name>_spm.dds (SpecMap: R specular, G glow, B house colour), DXT1 with
all mip levels (the engine does not make mip levels for DDS files), into one archive, EwMaterialMapsZH.big, in the game folder. The game loads every .big in its folder, so
deleting that file restores the stock look. The DX9 build draws the textures when Options.ini
has EwPerPixelLighting = yes and EwNormalMaps = yes (EwMeshShader, M3.1b).

The textures are derived from the diffuse colour, so they are a stand-in until artists make
real ones: fine surface detail becomes bumps, bright unsaturated paint (metal, glass) becomes
shiny, and nothing glows or takes the house colour (the game recolours ZHC* textures itself).

Usage: make_material_maps.py <Zero Hour folder> [--generals <Generals folder>] [--out <file.big>]
"""

import argparse
import io
import os
import struct
import sys

import numpy as np
from PIL import Image

W3D_CHUNK_TEXTURE_NAME = 0x00000032


# --- .big archives -------------------------------------------------------------------------------

def read_big(path):
    """Returns {lower-case name: (path, offset, size)} for a BIGF/BIG4 archive."""
    entries = {}
    with open(path, 'rb') as f:
        header = f.read(16)
        if header[:4] not in (b'BIGF', b'BIG4'):
            return entries
        count = struct.unpack('>I', header[8:12])[0]
        for _ in range(count):
            offset, size = struct.unpack('>II', f.read(8))
            name = bytearray()
            while True:
                c = f.read(1)
                if not c or c == b'\0':
                    break
                name += c
            entries[name.decode('latin-1').replace('/', '\\').lower()] = (path, offset, size)
    return entries


def read_entry(entry):
    path, offset, size = entry
    with open(path, 'rb') as f:
        f.seek(offset)
        return f.read(size)


def write_big(path, files):
    """files: list of (name, bytes). Writes a BIGF archive."""
    names = [n.encode('latin-1') for n, _ in files]
    header_size = 16 + sum(8 + len(n) + 1 for n in names)
    offset = header_size
    table = bytearray()
    for name, (_, data) in zip(names, files):
        table += struct.pack('>II', offset, len(data)) + name + b'\0'
        offset += len(data)
    with open(path, 'wb') as f:
        f.write(b'BIGF' + struct.pack('<I', offset) + struct.pack('>II', len(files), header_size))
        f.write(table)
        for _, data in files:
            f.write(data)


# --- W3D -----------------------------------------------------------------------------------------

def w3d_texture_names(data, start=0, end=None, out=None):
    """Collects the texture names in a W3D file by walking its chunks."""
    if end is None:
        end = len(data)
    if out is None:
        out = set()
    pos = start
    while pos + 8 <= end:
        chunk_type, size = struct.unpack('<II', data[pos:pos + 8])
        has_children = bool(size & 0x80000000)
        size &= 0x7fffffff
        body = pos + 8
        if body + size > end:
            break
        if chunk_type == W3D_CHUNK_TEXTURE_NAME:
            name = data[body:body + size].split(b'\0')[0].decode('latin-1').strip().lower()
            if name:
                out.add(name)
        elif has_children:
            w3d_texture_names(data, body, body + size, out)
        pos = body + size
    return out


# --- Map generation -------------------------------------------------------------------------------

def blur(a, radius):
    """Box blur, repeated three times (close to a Gaussian); wraps at the edges like the UVs do."""
    if radius < 1:
        return a
    for _ in range(3):
        for axis in (0, 1):
            acc = np.zeros_like(a)
            for d in range(-radius, radius + 1):
                acc += np.roll(a, d, axis=axis)
            a = acc / (2 * radius + 1)
    return a


def make_maps(img):
    rgba = np.asarray(img.convert('RGBA'), dtype=np.float32) / 255.0
    rgb = rgba[..., :3]
    h, w = rgb.shape[:2]
    lum = rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)

    # Height: the detail of the paint, without its large-scale shading (painted light and
    # shadow would otherwise turn into slopes). Two scales: fine detail (panel lines, rivets)
    # and medium shapes (plates, vents).
    size_scale = max(w, h) / 256.0
    broad = blur(lum, max(1, int(round(8 * size_scale))))
    fine = lum - blur(lum, max(1, int(round(1 * size_scale))))
    medium = blur(lum, max(1, int(round(1 * size_scale)))) - broad
    height = blur(fine, 1 if size_scale >= 2 else 0) * 0.35 + medium * 1.0

    # Normal from the height gradient. DirectX convention: +x right, +y down the image (+v).
    strength = 2.5 * size_scale
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    n = np.stack([-dx * strength, -dy * strength, np.ones_like(height)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)

    # SpecMap. R: brighter, less saturated paint shines more (metal, glass); crevices
    # (darker than their surroundings) shine less. G (glow) and B (house colour) stay 0.
    mx = rgb.max(axis=-1)
    mn = rgb.min(axis=-1)
    sat = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0.0)
    cavity = np.clip(0.5 + (lum - blur(lum, max(1, int(round(2 * size_scale))))) * 4.0, 0.0, 1.0)
    spec = np.clip((lum ** 2.0) * (1.0 - 0.8 * sat) * (0.3 + 0.7 * cavity) * 0.9, 0.0, 1.0)
    spm = np.zeros((h, w, 3), dtype=np.float32)
    spm[..., 0] = spec

    return n, spm


def half(a):
    """Next mip level: 2x2 average (a 1-pixel side stays 1)."""
    h, w = a.shape[:2]
    if h > 1:
        a = (a[0:h - h % 2:2] + a[1:h - h % 2:2]) * 0.5
    if w > 1:
        a = (a[:, 0:w - w % 2:2] + a[:, 1:w - w % 2:2]) * 0.5
    return a


def dxt1_level(rgb01):
    """DXT1 blocks of one level (Pillow's encoder, without its header)."""
    img = Image.fromarray((rgb01 * 255.0 + 0.5).clip(0, 255).astype(np.uint8), 'RGB')
    buf = io.BytesIO()
    img.save(buf, format='DDS', pixel_format='DXT1')
    return buf.getvalue()[128:]


def dds_bytes(levels):
    """A DXT1 DDS file with the given mip levels (float RGB in 0..1, largest first)."""
    h, w = levels[0].shape[:2]
    data = b''.join(dxt1_level(l) for l in levels)
    DDSD = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # CAPS HEIGHT WIDTH PIXELFORMAT MIPMAPCOUNT LINEARSIZE
    CAPS = 0x1000 | 0x400000 | 0x8  # TEXTURE MIPMAP COMPLEX
    linear = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8
    header = struct.pack('<7I', 124, DDSD, h, w, linear, 0, len(levels)) + bytes(44)
    header += struct.pack('<2I4s5I', 32, 0x4, b'DXT1', 0, 0, 0, 0, 0)
    header += struct.pack('<5I', CAPS, 0, 0, 0, 0)
    return b'DDS ' + header + data


def mip_chain(a, normalize):
    levels = [a]
    while a.shape[0] > 1 or a.shape[1] > 1:
        a = half(a)
        if normalize:
            a = a / np.maximum(np.linalg.norm(a, axis=-1, keepdims=True), 1e-6)
        levels.append(a)
    return levels


# --- Main ----------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('zh', help='Zero Hour folder')
    parser.add_argument('--generals', help='Generals folder (default: next to the Zero Hour folder)')
    parser.add_argument('--out', help='output archive (default: <Zero Hour folder>/EwMaterialMapsZH.big)')
    parser.add_argument('--limit', type=int, default=0, help='only the first N textures (testing)')
    args = parser.parse_args()

    generals = args.generals or os.path.join(os.path.dirname(os.path.normpath(args.zh)), 'Command and Conquer Generals')
    out = args.out or os.path.join(args.zh, 'EwMaterialMapsZH.big')

    # Zero Hour archives win over Generals ones, as in the game.
    files = {}
    for folder in (generals, args.zh):
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            if name.lower().endswith('.big') and os.path.abspath(os.path.join(folder, name)) != os.path.abspath(out):
                files.update(read_big(os.path.join(folder, name)))
    print(f'{len(files)} archived files')

    names = set()
    for name, entry in files.items():
        if name.endswith('.w3d'):
            w3d_texture_names(read_entry(entry), out=names)
    print(f'{len(names)} textures used by W3D models')

    outputs = []
    made = missing = failed = 0
    for name in sorted(names):
        base = os.path.splitext(name)[0]
        if base.endswith('_nrm') or base.endswith('_spm'):
            continue
        # The game swaps .tga for .dds when the .dds exists (W3DFileSystem).
        entry = None
        for ext in ('.dds', '.tga'):
            entry = files.get('art\\textures\\' + base + ext)
            if entry:
                break
        if entry is None:
            missing += 1
            continue
        try:
            img = Image.open(io.BytesIO(read_entry(entry)))
            img.load()
            n, spm = make_maps(img)
            normal_dds = dds_bytes([l * 0.5 + 0.5 for l in mip_chain(n, True)])
            spm_dds = dds_bytes(mip_chain(spm, False))
        except Exception as e:  # unsupported formats, broken files
            print(f'  skipped {base}: {e}')
            failed += 1
            continue
        outputs.append(('Art\\Textures\\' + base + '_nrm.dds', normal_dds))
        outputs.append(('Art\\Textures\\' + base + '_spm.dds', spm_dds))
        made += 1
        if made % 100 == 0:
            print(f'  {made} textures done')
        if args.limit and made >= args.limit:
            break

    write_big(out, outputs)
    size = os.path.getsize(out) / (1024 * 1024)
    print(f'{made} textures got maps, {missing} not found, {failed} skipped')
    print(f'wrote {out} ({size:.0f} MB)')


if __name__ == '__main__':
    sys.exit(main())
