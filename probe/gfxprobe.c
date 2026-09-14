// Graphics canary -- and a dry run of the actual architecture.
//
// The engine will render into a CPU bitmap and hand it to a display process.
// This does exactly that end to end, with every library the WebKit Cairo port
// leans on: fontconfig picks a font, FreeType loads it, HarfBuzz shapes real
// text, Cairo rasterises glyphs and vector art through pixman, and the result is
// written out as a PNG via libpng. If this produces a correct image on device,
// the whole lower half of the engine stack is proven.
//
// Usage: n9gfxprobe [out.png]

#include <cairo.h>
#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <hb.h>
#include <hb-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 854
#define H 480

static int failures = 0;
static void report(const char *name, int ok, const char *detail)
{
    printf("[%s] %-26s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
    if (!ok) ++failures;
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "n9gfx.png";
    printf("cairo    : %s\nfreetype : compiled in\nharfbuzz : %s\n\n",
           cairo_version_string(), hb_version_string());

    // --- fontconfig: can it read the device's 2011-era fonts.conf? -----------
    char fontfile[1024] = {0};
    {
        int ok = FcInit();
        if (ok) {
            // NOT FcNameParse("sans-serif"): fontconfig's name syntax is
            // family-size:opt=val, so the '-' is the SIZE separator and that
            // string parses as family "sans" with a bogus size "serif" -- no
            // generic-family rule then matches. Build the pattern directly.
            FcPattern *pat = FcPatternCreate();
            FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *) "sans-serif");
            FcConfigSubstitute(NULL, pat, FcMatchPattern);
            FcDefaultSubstitute(pat);
            FcResult res;
            FcPattern *m = FcFontMatch(NULL, pat, &res);
            FcChar8 *file = NULL;
            if (m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch && file) {
                snprintf(fontfile, sizeof fontfile, "%s", (const char *) file);
            } else {
                ok = 0;
            }
            if (m) FcPatternDestroy(m);
            FcPatternDestroy(pat);
        }
        report("fontconfig match", ok, fontfile[0] ? fontfile : "no font matched");
        if (!ok) return 1;
    }

    // --- FreeType ------------------------------------------------------------
    FT_Library ftlib; FT_Face face;
    {
        int ok = (FT_Init_FreeType(&ftlib) == 0) && (FT_New_Face(ftlib, fontfile, 0, &face) == 0);
        char d[256];
        if (ok) {
            snprintf(d, sizeof d, "%s %s, %ld glyphs",
                     face->family_name ? face->family_name : "?",
                     face->style_name ? face->style_name : "?", (long) face->num_glyphs);
            FT_Set_Pixel_Sizes(face, 0, 34);
        }
        report("FreeType load face", ok, ok ? d : NULL);
        if (!ok) return 1;
    }

    // --- HarfBuzz shaping ----------------------------------------------------
    const char *text = "Flexbox & Grid \xE2\x80\x94 2026";
    hb_buffer_t *buf = hb_buffer_create();
    unsigned nglyphs = 0;
    hb_glyph_info_t *ginfo = NULL; hb_glyph_position_t *gpos = NULL;
    {
        hb_font_t *hbfont = hb_ft_font_create(face, NULL);
        hb_buffer_add_utf8(buf, text, -1, 0, -1);
        hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
        hb_buffer_set_language(buf, hb_language_from_string("en", -1));
        hb_shape(hbfont, buf, NULL, 0);
        ginfo = hb_buffer_get_glyph_infos(buf, &nglyphs);
        gpos  = hb_buffer_get_glyph_positions(buf, &nglyphs);
        char d[64]; snprintf(d, sizeof d, "%u glyphs from %u chars", nglyphs, (unsigned) strlen(text));
        // every glyph must resolve; id 0 is .notdef
        int ok = nglyphs > 0;
        for (unsigned i = 0; i < nglyphs; i++) if (ginfo[i].codepoint == 0) ok = 0;
        report("HarfBuzz shape", ok, d);
        hb_font_destroy(hbfont);
    }

    // --- Cairo: raster through pixman ---------------------------------------
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(surf);
    {
        // background gradient
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, H);
        cairo_pattern_add_color_stop_rgb(g, 0.0, 0.09, 0.11, 0.16);
        cairo_pattern_add_color_stop_rgb(g, 1.0, 0.02, 0.03, 0.05);
        cairo_set_source(cr, g); cairo_paint(cr); cairo_pattern_destroy(g);

        // three boxes, as a flex row would lay them out
        const double pad = 40, gap = 24, top = 90, bh = 150;
        const double bw = (W - 2 * pad - 2 * gap) / 3.0;
        const double col[3][3] = {{0.25,0.55,0.95},{0.95,0.45,0.30},{0.35,0.80,0.50}};
        for (int i = 0; i < 3; i++) {
            double x = pad + i * (bw + gap);
            cairo_set_source_rgb(cr, col[i][0], col[i][1], col[i][2]);
            // rounded rect via arcs -- exercises the path/curve rasteriser
            double r = 14;
            cairo_new_sub_path(cr);
            cairo_arc(cr, x + bw - r, top + r,      r, -1.5708, 0);
            cairo_arc(cr, x + bw - r, top + bh - r, r, 0, 1.5708);
            cairo_arc(cr, x + r,      top + bh - r, r, 1.5708, 3.1416);
            cairo_arc(cr, x + r,      top + r,      r, 3.1416, 4.7124);
            cairo_close_path(cr);
            cairo_fill(cr);
        }

        // the shaped text, drawn as positioned glyphs from HarfBuzz
        cairo_font_face_t *cff = cairo_ft_font_face_create_for_ft_face(face, 0);
        cairo_set_font_face(cr, cff);
        cairo_set_font_size(cr, 34);
        cairo_glyph_t *glyphs = cairo_glyph_allocate((int) nglyphs);
        double gx = pad, gy = 300;
        for (unsigned i = 0; i < nglyphs; i++) {
            glyphs[i].index = ginfo[i].codepoint;
            glyphs[i].x = gx + gpos[i].x_offset / 64.0;
            glyphs[i].y = gy - gpos[i].y_offset / 64.0;
            gx += gpos[i].x_advance / 64.0;
            gy -= gpos[i].y_advance / 64.0;
        }
        cairo_set_source_rgb(cr, 0.96, 0.96, 0.98);
        cairo_show_glyphs(cr, glyphs, (int) nglyphs);
        cairo_glyph_free(glyphs);
        cairo_font_face_destroy(cff);

        // a stroked curve, to exercise stroking and antialiasing
        cairo_set_source_rgb(cr, 0.55, 0.65, 0.85);
        cairo_set_line_width(cr, 5);
        cairo_move_to(cr, pad, 380);
        cairo_curve_to(cr, W * 0.35, 330, W * 0.65, 440, W - pad, 380);
        cairo_stroke(cr);

        report("cairo draw", cairo_status(cr) == CAIRO_STATUS_SUCCESS,
               cairo_status_to_string(cairo_status(cr)));
    }

    // --- verify pixels actually changed, then write the PNG ------------------
    {
        cairo_surface_flush(surf);
        unsigned char *data = cairo_image_surface_get_data(surf);
        int stride = cairo_image_surface_get_stride(surf);
        long lit = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint32_t px = *(uint32_t *)(data + y * stride + x * 4);
                if ((px & 0x00FFFFFF) > 0x00303030) lit++;
            }
        char d[64]; snprintf(d, sizeof d, "%ld bright px of %d", lit, W * H);
        report("raster produced pixels", lit > 20000, d);

        cairo_status_t st = cairo_surface_write_to_png(surf, out);
        report("write PNG (libpng)", st == CAIRO_STATUS_SUCCESS, cairo_status_to_string(st));
    }

    cairo_destroy(cr); cairo_surface_destroy(surf);
    hb_buffer_destroy(buf);
    FT_Done_Face(face); FT_Done_FreeType(ftlib);
    FcFini();

    printf("\n%s (%d failure%s) -> %s\n", failures ? "NOT CLEAR" : "ALL CLEAR",
           failures, failures == 1 ? "" : "s", out);
    return failures ? 1 : 0;
}
