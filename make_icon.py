# -*- coding: utf-8 -*-
"""生成 PeekStock 应用图标: 深色圆角底 + 眼睛 + 眼瞳内的上升行情线
输出: PeekStock.ico (含 256/128/64/48/32/24/16) 与 docs/icon_preview.png
"""
from PIL import Image, ImageDraw
import math

S = 256  # 主画布尺寸


def bez(p0, c, p1, n=64):
    """二次贝塞尔采样"""
    pts = []
    for i in range(n + 1):
        t = i / n
        x = (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t ** 2 * p1[0]
        y = (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t ** 2 * p1[1]
        pts.append((x, y))
    return pts


def draw_icon(size):
    s = size / S  # 缩放系数
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # 1) 深色圆角底 (与应用面板色一致) + 微弱描边
    m = round(8 * s)
    r = round(52 * s)
    d.rounded_rectangle([m, m, size - m, size - m], radius=r,
                        fill=(30, 30, 30, 255), outline=(64, 64, 64, 255),
                        width=max(1, round(3 * s)))

    # 2) 眼睛轮廓 (杏仁形), 暖白 — 与眼瞳同色 (C方案定稿)
    eye_gray = (238, 234, 229, 255)
    w_eye = max(1, round(13 * s))
    L, Rt, cy = round(46 * s), round(210 * s), round(130 * s)
    top = bez((L, cy), (128 * s, 44 * s), (Rt, cy))
    bot = bez((L, cy), (128 * s, 216 * s), (Rt, cy))
    d.line(top + bot[::-1], fill=eye_gray, width=w_eye, joint='curve')

    # 3) 眼瞳: 暖白圆底 (与眼眶同色, 衬托红色行情线)
    iris_r = round(38 * s)
    cx, icy = round(128 * s), round(130 * s)
    d.ellipse([cx - iris_r, icy - iris_r, cx + iris_r, icy + iris_r],
              fill=(238, 234, 229, 255))

    # 4) 瞳内上升折线 (深色, 呼应行情)
    w_line = max(1, round(9 * s))
    line_red = (205, 112, 108, 255)   # 舒适的柔和红
    pts = [(round(106 * s), round(146 * s)), (round(121 * s), round(128 * s)),
           (round(133 * s), round(138 * s)), (round(152 * s), round(112 * s))]
    d.line(pts, fill=line_red, width=w_line, joint='curve')
    # 线端小箭头 (右上方向)
    ax, ay = pts[-1]
    ah = round(10 * s)
    d.line([(ax - ah, ay + round(2.5 * s)), (ax, ay)], fill=line_red, width=w_line)
    d.line([(ax, ay), (ax - round(2.5 * s), ay + ah)], fill=line_red, width=w_line)
    return img


# 生成多尺寸 ico
base = draw_icon(S)
sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)]
base.save('PeekStock.ico', sizes=sizes)
print('PeekStock.ico ok')

# 预览图: 各尺寸一排 (深底/白底各一行)
prev = Image.new('RGBA', (7 * 140, 300), (24, 24, 26, 255))
for i, sz in enumerate([256, 128, 64, 48, 32, 24, 16]):
    ic = draw_icon(sz)
    prev.paste(ic, (10 + i * 140 - (sz - sz) // 2 + (140 - sz) // 2, 20), ic)
prev.convert('RGB').save('docs/icon_preview.png')
# 白底行
prev2 = Image.new('RGBA', (7 * 140, 300), (255, 255, 255, 255))
for i, sz in enumerate([256, 128, 64, 48, 32, 24, 16]):
    ic = draw_icon(sz)
    prev2.paste(ic, (10 + i * 140 + (140 - sz) // 2, 20), ic)
prev2.convert('RGB').save('docs/icon_preview_white.png')
print('预览图 ok')
