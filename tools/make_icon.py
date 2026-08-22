import struct
import math
import os

def create_icon_rgba(size):
    """Draws a 32-bit RGBA buffer of dimension (size x size)."""
    pixels = [[0, 0, 0, 0] for _ in range(size * size)]
    
    def set_pixel(x, y, r, g, b, a):
        if 0 <= x < size and 0 <= y < size:
            idx = (size - 1 - y) * size + x  # Bottom-up for BMP
            # Alpha blending
            old_a = pixels[idx][3] / 255.0
            new_a = a / 255.0
            out_a = new_a + old_a * (1.0 - new_a)
            if out_a > 0:
                out_r = (r * new_a + pixels[idx][0] * old_a * (1.0 - new_a)) / out_a
                out_g = (g * new_a + pixels[idx][1] * old_a * (1.0 - new_a)) / out_a
                out_b = (b * new_a + pixels[idx][2] * old_a * (1.0 - new_a)) / out_a
                pixels[idx] = [int(out_r), int(out_g), int(out_b), int(out_a * 255)]

    # 1. Background: Rounded squircle tile
    radius = size * 0.22
    margin = size * 0.04
    for y in range(size):
        for x in range(size):
            # Check rounded box distance
            dx = max(margin + radius - x, 0, x - (size - 1 - margin - radius))
            dy = max(margin + radius - y, 0, y - (size - 1 - margin - radius))
            dist = math.sqrt(dx * dx + dy * dy)
            if dist <= radius:
                alpha = 1.0
                if dist > radius - 1.0:
                    alpha = radius - dist
                # Gradient from deep indigo top to darker slate bottom
                t = y / size
                r = int(30 + 15 * (1 - t))
                g = int(27 + 35 * (1 - t))
                b = int(75 + 100 * (1 - t))
                set_pixel(x, y, r, g, b, int(alpha * 255))
            elif dist <= radius + 0.5:
                alpha = (radius + 0.5 - dist) * 0.5
                set_pixel(x, y, 99, 102, 241, int(alpha * 255))

    # 2. Keyboard Base (bottom region)
    kb_x0 = int(size * 0.16)
    kb_x1 = int(size * 0.84)
    kb_y0 = int(size * 0.46)
    kb_y1 = int(size * 0.82)
    
    # Draw keyboard plate
    for y in range(kb_y0, kb_y1 + 1):
        for x in range(kb_x0, kb_x1 + 1):
            set_pixel(x, y, 15, 23, 42, 200)

    # Draw keycaps grid
    rows = 3
    cols = 5
    key_h = (kb_y1 - kb_y0) / (rows + 0.6)
    key_w = (kb_x1 - kb_x0) / (cols + 0.4)
    
    for r in range(rows):
        ky0 = int(kb_y0 + r * (key_h + 1) + 2)
        ky1 = int(ky0 + key_h - 1)
        # Space bar on last row
        if r == rows - 1:
            # Side keys
            for c in [0, 4]:
                kx0 = int(kb_x0 + c * (key_w + 1) + 2)
                kx1 = int(kx0 + key_w - 1)
                for y in range(ky0, ky1 + 1):
                    for x in range(kx0, kx1 + 1):
                        set_pixel(x, y, 99, 102, 241, 230)
            # Center spacebar
            kx0 = int(kb_x0 + 1 * (key_w + 1) + 2)
            kx1 = int(kb_x0 + 3 * (key_w + 1) + key_w - 1)
            for y in range(ky0, ky1 + 1):
                for x in range(kx0, kx1 + 1):
                    set_pixel(x, y, 56, 189, 248, 240)
        else:
            for c in range(cols):
                kx0 = int(kb_x0 + c * (key_w + 1) + 2)
                kx1 = int(kx0 + key_w - 1)
                for y in range(ky0, ky1 + 1):
                    for x in range(kx0, kx1 + 1):
                        # Gradient on keycaps
                        highlight = (r == 0 and c == 2) or (r == 1 and c == 1)
                        if highlight:
                            set_pixel(x, y, 129, 140, 248, 255)
                        else:
                            set_pixel(x, y, 71, 85, 105, 220)

    # 3. AI Sparkle / Starburst (top center/right)
    spark_cx = size * 0.50
    spark_cy = size * 0.28
    spark_r = size * 0.22
    
    for y in range(int(spark_cy - spark_r - 2), int(spark_cy + spark_r + 3)):
        for x in range(int(spark_cx - spark_r - 2), int(spark_cx + spark_r + 3)):
            dx = abs(x - spark_cx)
            dy = abs(y - spark_cy)
            # 4-point star formula: (dx/r)^0.5 + (dy/r)^0.5 <= 1
            if spark_r > 0:
                val = math.sqrt(dx / spark_r) + math.sqrt(dy / spark_r)
                if val <= 1.0:
                    intensity = 1.0 - val
                    # Core is bright white-cyan, outer is cyan/violet
                    cr = int(220 + 35 * intensity)
                    cg = int(240 + 15 * intensity)
                    cb = 255
                    set_pixel(x, y, cr, cg, cb, int(min(1.0, intensity * 2.0 + 0.3) * 255))
                elif val <= 1.25:
                    alpha = (1.25 - val) / 0.25 * 0.4
                    set_pixel(x, y, 56, 189, 248, int(alpha * 255))

    # Secondary small sparkle (top right)
    s2_cx = size * 0.74
    s2_cy = size * 0.18
    s2_r = size * 0.11
    for y in range(int(s2_cy - s2_r - 1), int(s2_cy + s2_r + 2)):
        for x in range(int(s2_cx - s2_r - 1), int(s2_cx + s2_r + 2)):
            dx = abs(x - s2_cx)
            dy = abs(y - s2_cy)
            if s2_r > 0:
                val = math.sqrt(dx / s2_r) + math.sqrt(dy / s2_r)
                if val <= 1.0:
                    set_pixel(x, y, 250, 204, 21, 240)

    # Flatten into 32-bit BGRA bytes
    raw_bytes = bytearray()
    for r, g, b, a in pixels:
        raw_bytes.extend([b, g, r, a])
    return bytes(raw_bytes)

def make_ico(sizes, output_path):
    # ICO Header: idReserved=0, idType=1 (icon), idCount=len(sizes)
    images = []
    for s in sizes:
        rgba = create_icon_rgba(s)
        # BITMAPINFOHEADER (40 bytes) + color data + 1-bit AND mask (all zeros)
        bih = struct.pack('<IIIHHIIIIII',
                          40,        # biSize
                          s,         # biWidth
                          s * 2,     # biHeight (doubled for XOR+AND mask in ICO)
                          1,         # biPlanes
                          32,        # biBitCount
                          0,         # biCompression (BI_RGB)
                          len(rgba), # biSizeImage
                          0, 0, 0, 0)
        and_mask = b'\x00' * (((s + 31) // 32) * 4 * s)
        img_data = bih + rgba + and_mask
        images.append((s, img_data))

    header = struct.pack('<HHH', 0, 1, len(sizes))
    entries = bytearray()
    offset = 6 + len(sizes) * 16
    
    for s, data in images:
        width = 0 if s == 256 else s
        height = 0 if s == 256 else s
        entry = struct.pack('<BBBBHHII',
                            width, height, 0, 0, 1, 32, len(data), offset)
        entries.extend(entry)
        offset += len(data)

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(entries)
        for _, data in images:
            f.write(data)
    print(f"Generated {output_path} with sizes: {sizes}")

if __name__ == '__main__':
    make_ico([16, 24, 32, 48, 64, 128, 256], 'resources/AIInputEngine.ico')
