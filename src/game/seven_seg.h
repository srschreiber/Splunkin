#pragma once

// 7-segment digit: calls box(u0,v0,u1,v1) for each lit segment of digit `d`, in a
// local cell of width w / height h / segment thickness t (origin at bottom-left).
// Callers map the boxes to whatever they draw (HUD rects, billboarded quads, ...).
//
// Segment layout (the shape of an 8) and their bit positions in `seg`:
//        aaaa            a = bit 0
//       f    b           b = bit 1
//       f    b           c = bit 2
//        gggg            d = bit 3
//       e    c           e = bit 4
//       e    c           f = bit 5
//        dddd            g = bit 6
// e.g. seg['8'] lights all 7; seg['1'] lights only b,c.
template <class Box>
inline void seven_seg(int d, float w, float h, float t, Box box) {
    static const unsigned char seg[10] =
        { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    if (d < 0 || d > 9) return;
    const unsigned char m = seg[d];
    const float top = h, mid = h * 0.5f, bot = 0.0f;
    auto on = [&](int bit) { return (m >> bit) & 1; };
    if (on(0)) box(t,        top - t,        w - t, top);              // a (top)
    if (on(1)) box(w - t,    mid,            w,     top);              // b (top-right)
    if (on(2)) box(w - t,    bot,            w,     mid);              // c (bottom-right)
    if (on(3)) box(t,        bot,            w - t, bot + t);          // d (bottom)
    if (on(4)) box(0.0f,     bot,            t,     mid);              // e (bottom-left)
    if (on(5)) box(0.0f,     mid,            t,     top);              // f (top-left)
    if (on(6)) box(t,        mid - t * 0.5f, w - t, mid + t * 0.5f);   // g (middle)
}
