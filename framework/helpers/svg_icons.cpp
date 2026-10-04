#include "svg_icons.h"

#include "data/custom_icons.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace svg_icons
{
    namespace
    {
        constexpr float k_pi = 3.14159265358979323846f;

        struct contour
        {
            std::vector<ImVec2> points;
            bool  closed  = false;
            bool  filled  = false;
            float stroke  = 0.f;   // 0 = no stroke
        };

        struct parsed_icon
        {
            bool  valid = false;
            float vb_x = 0.f, vb_y = 0.f, vb_w = 24.f, vb_h = 24.f;
            std::vector<contour> contours;
        };

        std::map<std::string, parsed_icon> g_cache;

        // ------------------------------------------------------------------
        // Tiny scanners
        // ------------------------------------------------------------------

        bool is_space(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',';
        }

        // Reads the value of attribute `name` from a tag body ("circle cx="1" ...").
        bool attr(const std::string& tag, const char* name, std::string& out)
        {
            const size_t name_len = std::strlen(name);
            size_t pos = 0;
            while ((pos = tag.find(name, pos)) != std::string::npos)
            {
                // Must be a whole attribute name: preceded by space, followed by '='.
                const bool left_ok = pos == 0 || is_space(tag[pos - 1]) || tag[pos - 1] == ' ';
                size_t after = pos + name_len;
                while (after < tag.size() && (tag[after] == ' ' || tag[after] == '\t'))
                    ++after;
                if (!left_ok || after >= tag.size() || tag[after] != '=')
                {
                    pos += name_len;
                    continue;
                }

                ++after;
                while (after < tag.size() && (tag[after] == ' ' || tag[after] == '\t'))
                    ++after;
                if (after >= tag.size() || (tag[after] != '"' && tag[after] != '\''))
                    return false;

                const char quote = tag[after++];
                const size_t end = tag.find(quote, after);
                if (end == std::string::npos)
                    return false;

                out = tag.substr(after, end - after);
                return true;
            }
            return false;
        }

        float attr_float(const std::string& tag, const char* name, float fallback)
        {
            std::string value;
            if (!attr(tag, name, value))
                return fallback;
            return (float)std::atof(value.c_str());
        }

        // ------------------------------------------------------------------
        // Path data
        // ------------------------------------------------------------------

        struct scanner
        {
            const char* p   = nullptr;
            const char* end = nullptr;

            void skip()
            {
                while (p < end && is_space(*p))
                    ++p;
            }

            bool number(float& out)
            {
                skip();
                if (p >= end)
                    return false;

                char* stop = nullptr;
                const float v = std::strtof(p, &stop);
                if (stop == p)
                    return false;

                p = stop;
                out = v;
                return true;
            }

            // SVG allows flags to be written without separators ("1 0" or "10").
            bool flag(float& out)
            {
                skip();
                if (p >= end || (*p != '0' && *p != '1'))
                    return number(out);
                out = (*p == '1') ? 1.f : 0.f;
                ++p;
                return true;
            }

            bool command(char& out)
            {
                skip();
                if (p >= end)
                    return false;
                const char c = *p;
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                {
                    out = c;
                    ++p;
                    return true;
                }
                return false;
            }

            bool more_numbers() const
            {
                const char* q = p;
                while (q < end && is_space(*q))
                    ++q;
                if (q >= end)
                    return false;
                return (*q >= '0' && *q <= '9') || *q == '-' || *q == '+' || *q == '.';
            }
        };

        void push_point(contour& c, const ImVec2& pt)
        {
            // Skip duplicates, they only cost tessellation work.
            if (!c.points.empty())
            {
                const ImVec2& last = c.points.back();
                if (ImFabs(last.x - pt.x) < 0.0005f && ImFabs(last.y - pt.y) < 0.0005f)
                    return;
            }
            c.points.push_back(pt);
        }

        void cubic_to(contour& c, const ImVec2& p0, const ImVec2& p1,
            const ImVec2& p2, const ImVec2& p3)
        {
            constexpr int steps = 16;
            for (int i = 1; i <= steps; ++i)
            {
                const float t  = (float)i / (float)steps;
                const float u  = 1.f - t;
                const float w0 = u * u * u;
                const float w1 = 3.f * u * u * t;
                const float w2 = 3.f * u * t * t;
                const float w3 = t * t * t;
                push_point(c, {
                    w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
                    w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y });
            }
        }

        void quad_to(contour& c, const ImVec2& p0, const ImVec2& p1, const ImVec2& p2)
        {
            // Elevate to a cubic and reuse the same tessellation.
            const ImVec2 c1 = { p0.x + 2.f / 3.f * (p1.x - p0.x), p0.y + 2.f / 3.f * (p1.y - p0.y) };
            const ImVec2 c2 = { p2.x + 2.f / 3.f * (p1.x - p2.x), p2.y + 2.f / 3.f * (p1.y - p2.y) };
            cubic_to(c, p0, c1, c2, p2);
        }

        // Endpoint -> centre parameterisation, per the SVG implementation notes.
        void arc_to(contour& c, const ImVec2& p0, float rx, float ry, float rot_deg,
            bool large_arc, bool sweep, const ImVec2& p1)
        {
            if (rx == 0.f || ry == 0.f)
            {
                push_point(c, p1);
                return;
            }

            rx = ImFabs(rx);
            ry = ImFabs(ry);

            const float phi = rot_deg * k_pi / 180.f;
            const float cos_phi = cosf(phi);
            const float sin_phi = sinf(phi);

            const float dx2 = (p0.x - p1.x) * 0.5f;
            const float dy2 = (p0.y - p1.y) * 0.5f;
            const float x1p =  cos_phi * dx2 + sin_phi * dy2;
            const float y1p = -sin_phi * dx2 + cos_phi * dy2;

            float rx2 = rx * rx;
            float ry2 = ry * ry;
            const float x1p2 = x1p * x1p;
            const float y1p2 = y1p * y1p;

            // Scale the radii up when they are too small to span the endpoints.
            const float lambda = x1p2 / rx2 + y1p2 / ry2;
            if (lambda > 1.f)
            {
                const float s = sqrtf(lambda);
                rx *= s;
                ry *= s;
                rx2 = rx * rx;
                ry2 = ry * ry;
            }

            float denom = rx2 * y1p2 + ry2 * x1p2;
            float factor = 0.f;
            if (denom > 0.f)
            {
                const float num = rx2 * ry2 - denom;
                factor = sqrtf(ImMax(0.f, num / denom));
            }
            if (large_arc == sweep)
                factor = -factor;

            const float cxp =  factor * rx * y1p / ry;
            const float cyp = -factor * ry * x1p / rx;

            const ImVec2 centre = {
                cos_phi * cxp - sin_phi * cyp + (p0.x + p1.x) * 0.5f,
                sin_phi * cxp + cos_phi * cyp + (p0.y + p1.y) * 0.5f };

            const float ux = (x1p - cxp) / rx;
            const float uy = (y1p - cyp) / ry;
            const float vx = (-x1p - cxp) / rx;
            const float vy = (-y1p - cyp) / ry;

            const float theta0 = atan2f(uy, ux);
            float delta = atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
            if (!sweep && delta > 0.f)
                delta -= 2.f * k_pi;
            else if (sweep && delta < 0.f)
                delta += 2.f * k_pi;

            const int steps = ImMax(4, (int)(ImFabs(delta) / (k_pi / 12.f)) + 1);
            for (int i = 1; i <= steps; ++i)
            {
                const float t = theta0 + delta * ((float)i / (float)steps);
                const float ct = cosf(t);
                const float st = sinf(t);
                push_point(c, {
                    centre.x + cos_phi * rx * ct - sin_phi * ry * st,
                    centre.y + sin_phi * rx * ct + cos_phi * ry * st });
            }
        }

        void parse_path(const std::string& data, bool filled, float stroke,
            std::vector<contour>& out)
        {
            scanner sc;
            sc.p   = data.c_str();
            sc.end = data.c_str() + data.size();

            contour current;
            current.filled = filled;
            current.stroke = stroke;

            ImVec2 cursor    = { 0.f, 0.f };
            ImVec2 start     = { 0.f, 0.f };
            ImVec2 last_ctrl = { 0.f, 0.f };
            char   prev_cmd  = 0;
            char   cmd       = 0;

            const auto flush = [&](bool closed)
            {
                if (current.points.size() >= 2)
                {
                    current.closed = closed;
                    out.push_back(current);
                }
                current.points.clear();
            };

            while (true)
            {
                char next = 0;
                if (sc.command(next))
                    cmd = next;
                else if (!sc.more_numbers())
                    break;
                else if (cmd == 'M')
                    cmd = 'L';          // implicit lineto after a moveto
                else if (cmd == 'm')
                    cmd = 'l';

                const bool rel = cmd >= 'a' && cmd <= 'z';
                const char up  = (char)(rel ? cmd - 32 : cmd);

                if (up == 'Z')
                {
                    if (!current.points.empty())
                    {
                        flush(true);
                        cursor = start;
                    }
                    prev_cmd = up;
                    continue;
                }

                float a = 0.f, b = 0.f, c1x = 0.f, c1y = 0.f, c2x = 0.f, c2y = 0.f;
                switch (up)
                {
                case 'M':
                    if (!sc.number(a) || !sc.number(b))
                        return;
                    flush(false);
                    cursor = rel ? ImVec2(cursor.x + a, cursor.y + b) : ImVec2(a, b);
                    start  = cursor;
                    current.points.clear();
                    push_point(current, cursor);
                    break;

                case 'L':
                    if (!sc.number(a) || !sc.number(b))
                        return;
                    cursor = rel ? ImVec2(cursor.x + a, cursor.y + b) : ImVec2(a, b);
                    push_point(current, cursor);
                    break;

                case 'H':
                    if (!sc.number(a))
                        return;
                    cursor.x = rel ? cursor.x + a : a;
                    push_point(current, cursor);
                    break;

                case 'V':
                    if (!sc.number(a))
                        return;
                    cursor.y = rel ? cursor.y + a : a;
                    push_point(current, cursor);
                    break;

                case 'C':
                {
                    if (!sc.number(c1x) || !sc.number(c1y) || !sc.number(c2x)
                        || !sc.number(c2y) || !sc.number(a) || !sc.number(b))
                        return;
                    const ImVec2 p1 = rel ? ImVec2(cursor.x + c1x, cursor.y + c1y) : ImVec2(c1x, c1y);
                    const ImVec2 p2 = rel ? ImVec2(cursor.x + c2x, cursor.y + c2y) : ImVec2(c2x, c2y);
                    const ImVec2 p3 = rel ? ImVec2(cursor.x + a, cursor.y + b) : ImVec2(a, b);
                    cubic_to(current, cursor, p1, p2, p3);
                    last_ctrl = p2;
                    cursor = p3;
                    break;
                }

                case 'S':
                {
                    if (!sc.number(c2x) || !sc.number(c2y) || !sc.number(a) || !sc.number(b))
                        return;
                    const bool smooth = prev_cmd == 'C' || prev_cmd == 'S';
                    const ImVec2 p1 = smooth
                        ? ImVec2(2.f * cursor.x - last_ctrl.x, 2.f * cursor.y - last_ctrl.y)
                        : cursor;
                    const ImVec2 p2 = rel ? ImVec2(cursor.x + c2x, cursor.y + c2y) : ImVec2(c2x, c2y);
                    const ImVec2 p3 = rel ? ImVec2(cursor.x + a, cursor.y + b) : ImVec2(a, b);
                    cubic_to(current, cursor, p1, p2, p3);
                    last_ctrl = p2;
                    cursor = p3;
                    break;
                }

                case 'Q':
                {
                    if (!sc.number(c1x) || !sc.number(c1y) || !sc.number(a) || !sc.number(b))
                        return;
                    const ImVec2 p1 = rel ? ImVec2(cursor.x + c1x, cursor.y + c1y) : ImVec2(c1x, c1y);
                    const ImVec2 p2 = rel ? ImVec2(cursor.x + a, cursor.y + b) : ImVec2(a, b);
                    quad_to(current, cursor, p1, p2);
                    last_ctrl = p1;
                    cursor = p2;
                    break;
                }

                case 'T':
                {
                    if (!sc.number(a) || !sc.number(b))
                        return;
                    const bool smooth = prev_cmd == 'Q' || prev_cmd == 'T';
                    const ImVec2 p1 = smooth
                        ? ImVec2(2.f * cursor.x - last_ctrl.x, 2.f * cursor.y - last_ctrl.y)
                        : cursor;
                    const ImVec2 p2 = rel ? ImVec2(cursor.x + a, cursor.y + b) : ImVec2(a, b);
                    quad_to(current, cursor, p1, p2);
                    last_ctrl = p1;
                    cursor = p2;
                    break;
                }

                case 'A':
                {
                    float rx = 0.f, ry = 0.f, rot = 0.f, large = 0.f, sweep = 0.f;
                    if (!sc.number(rx) || !sc.number(ry) || !sc.number(rot)
                        || !sc.flag(large) || !sc.flag(sweep)
                        || !sc.number(a) || !sc.number(b))
                        return;
                    const ImVec2 target = rel ? ImVec2(cursor.x + a, cursor.y + b) : ImVec2(a, b);
                    arc_to(current, cursor, rx, ry, rot, large != 0.f, sweep != 0.f, target);
                    cursor = target;
                    break;
                }

                default:
                    return;
                }

                prev_cmd = up;
            }

            flush(false);
        }

        // ------------------------------------------------------------------
        // Primitive shapes
        // ------------------------------------------------------------------

        void add_ellipse(std::vector<contour>& out, float cx, float cy, float rx, float ry,
            bool filled, float stroke)
        {
            if (rx <= 0.f || ry <= 0.f)
                return;

            contour c;
            c.filled = filled;
            c.stroke = stroke;
            c.closed = true;
            constexpr int steps = 40;
            for (int i = 0; i < steps; ++i)
            {
                const float t = (float)i / (float)steps * 2.f * k_pi;
                c.points.push_back({ cx + rx * cosf(t), cy + ry * sinf(t) });
            }
            out.push_back(c);
        }

        void add_rect(std::vector<contour>& out, float x, float y, float w, float h,
            float rx, float ry, bool filled, float stroke)
        {
            if (w <= 0.f || h <= 0.f)
                return;

            rx = ImClamp(rx, 0.f, w * 0.5f);
            ry = ImClamp(ry, 0.f, h * 0.5f);

            contour c;
            c.filled = filled;
            c.stroke = stroke;
            c.closed = true;

            if (rx <= 0.f || ry <= 0.f)
            {
                c.points = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
                out.push_back(c);
                return;
            }

            constexpr int steps = 8;
            const float cx[4] = { x + w - rx, x + w - rx, x + rx, x + rx };
            const float cy[4] = { y + ry, y + h - ry, y + h - ry, y + ry };
            const float a0[4] = { -k_pi * 0.5f, 0.f, k_pi * 0.5f, k_pi };
            for (int corner = 0; corner < 4; ++corner)
            {
                for (int i = 0; i <= steps; ++i)
                {
                    const float t = a0[corner] + (k_pi * 0.5f) * ((float)i / (float)steps);
                    c.points.push_back({ cx[corner] + rx * cosf(t), cy[corner] + ry * sinf(t) });
                }
            }
            out.push_back(c);
        }

        void add_points_list(std::vector<contour>& out, const std::string& data,
            bool closed, bool filled, float stroke)
        {
            contour c;
            c.filled = filled;
            c.stroke = stroke;
            c.closed = closed;

            scanner sc;
            sc.p   = data.c_str();
            sc.end = data.c_str() + data.size();
            float x = 0.f, y = 0.f;
            while (sc.number(x) && sc.number(y))
                c.points.push_back({ x, y });

            if (c.points.size() >= 2)
                out.push_back(c);
        }

        // ------------------------------------------------------------------
        // Document walk
        // ------------------------------------------------------------------

        struct style
        {
            bool  filled = false;
            float stroke = 2.f;
        };

        style read_style(const std::string& tag, const style& inherited)
        {
            style s = inherited;

            std::string value;
            if (attr(tag, "fill", value))
                s.filled = value != "none" && !value.empty();
            if (attr(tag, "stroke", value))
                s.stroke = value == "none" ? 0.f : (s.stroke > 0.f ? s.stroke : 2.f);
            if (attr(tag, "stroke-width", value))
                s.stroke = (float)std::atof(value.c_str());

            return s;
        }

        parsed_icon parse(const char* svg)
        {
            parsed_icon icon;
            if (!svg)
                return icon;

            const std::string text = svg;
            style root;
            bool root_seen = false;

            size_t pos = 0;
            while ((pos = text.find('<', pos)) != std::string::npos)
            {
                const size_t close = text.find('>', pos);
                if (close == std::string::npos)
                    break;

                std::string tag = text.substr(pos + 1, close - pos - 1);
                pos = close + 1;

                if (tag.empty() || tag[0] == '/' || tag[0] == '!' || tag[0] == '?')
                    continue;

                // Tag name up to the first space.
                size_t name_end = tag.find_first_of(" \t\r\n/");
                const std::string name = tag.substr(0, name_end == std::string::npos ? tag.size() : name_end);

                if (name == "svg")
                {
                    std::string vb;
                    if (attr(tag, "viewBox", vb))
                    {
                        scanner sc;
                        sc.p   = vb.c_str();
                        sc.end = vb.c_str() + vb.size();
                        float v[4] = { 0.f, 0.f, 24.f, 24.f };
                        for (float& f : v)
                            sc.number(f);
                        icon.vb_x = v[0];
                        icon.vb_y = v[1];
                        icon.vb_w = v[2] > 0.f ? v[2] : 24.f;
                        icon.vb_h = v[3] > 0.f ? v[3] : 24.f;
                    }
                    else
                    {
                        icon.vb_w = attr_float(tag, "width", 24.f);
                        icon.vb_h = attr_float(tag, "height", 24.f);
                    }

                    root = read_style(tag, style{});
                    root_seen = true;
                    continue;
                }

                const style s = read_style(tag, root_seen ? root : style{});

                if (name == "path")
                {
                    std::string d;
                    if (attr(tag, "d", d))
                        parse_path(d, s.filled, s.stroke, icon.contours);
                }
                else if (name == "circle")
                {
                    const float r = attr_float(tag, "r", 0.f);
                    add_ellipse(icon.contours, attr_float(tag, "cx", 0.f),
                        attr_float(tag, "cy", 0.f), r, r, s.filled, s.stroke);
                }
                else if (name == "ellipse")
                {
                    add_ellipse(icon.contours, attr_float(tag, "cx", 0.f),
                        attr_float(tag, "cy", 0.f), attr_float(tag, "rx", 0.f),
                        attr_float(tag, "ry", 0.f), s.filled, s.stroke);
                }
                else if (name == "rect")
                {
                    const float rx = attr_float(tag, "rx", -1.f);
                    const float ry = attr_float(tag, "ry", -1.f);
                    add_rect(icon.contours, attr_float(tag, "x", 0.f), attr_float(tag, "y", 0.f),
                        attr_float(tag, "width", 0.f), attr_float(tag, "height", 0.f),
                        rx < 0.f ? ImMax(ry, 0.f) : rx,
                        ry < 0.f ? ImMax(rx, 0.f) : ry,
                        s.filled, s.stroke);
                }
                else if (name == "line")
                {
                    contour c;
                    c.filled = false;
                    c.stroke = s.stroke;
                    c.points = {
                        { attr_float(tag, "x1", 0.f), attr_float(tag, "y1", 0.f) },
                        { attr_float(tag, "x2", 0.f), attr_float(tag, "y2", 0.f) } };
                    icon.contours.push_back(c);
                }
                else if (name == "polyline")
                {
                    std::string pts;
                    if (attr(tag, "points", pts))
                        add_points_list(icon.contours, pts, false, s.filled, s.stroke);
                }
                else if (name == "polygon")
                {
                    std::string pts;
                    if (attr(tag, "points", pts))
                        add_points_list(icon.contours, pts, true, s.filled, s.stroke);
                }
            }

            icon.valid = !icon.contours.empty();
            return icon;
        }

        const parsed_icon* lookup(const char* name)
        {
            if (!name || !*name)
                return nullptr;

            auto cached = g_cache.find(name);
            if (cached != g_cache.end())
                return cached->second.valid ? &cached->second : nullptr;

            for (int i = 0; i < custom_icons::table_count; ++i)
            {
                if (std::strcmp(custom_icons::table[i].name, name) != 0)
                    continue;

                const parsed_icon& stored = (g_cache[name] = parse(custom_icons::table[i].svg));
                return stored.valid ? &stored : nullptr;
            }

            g_cache[name] = parsed_icon{};
            return nullptr;
        }
    }

    bool has(const char* name)
    {
        return lookup(name) != nullptr;
    }

    bool draw(ImDrawList* dl, const char* name, const ImRect& bb, ImU32 col, float stroke_scale)
    {
        const parsed_icon* icon = lookup(name);
        if (!dl || !icon)
            return false;

        const float scale = ImMin(bb.GetWidth() / icon->vb_w, bb.GetHeight() / icon->vb_h);
        if (scale <= 0.f)
            return false;

        const ImVec2 origin = {
            bb.Min.x + (bb.GetWidth() - icon->vb_w * scale) * 0.5f,
            bb.Min.y + (bb.GetHeight() - icon->vb_h * scale) * 0.5f };

        for (const contour& c : icon->contours)
        {
            if (c.points.size() < 2)
                continue;

            for (const ImVec2& pt : c.points)
            {
                dl->PathLineTo({
                    origin.x + (pt.x - icon->vb_x) * scale,
                    origin.y + (pt.y - icon->vb_y) * scale });
            }

            if (c.filled)
            {
                dl->PathFillConvex(col);
                continue;
            }

            const float thickness = ImMax(1.f, c.stroke * scale * stroke_scale);
            dl->PathStroke(col, c.closed ? ImDrawFlags_Closed : 0, thickness);
        }

        return true;
    }
}
