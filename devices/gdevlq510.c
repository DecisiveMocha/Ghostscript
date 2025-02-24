/* Copyright (C) 2001-2012 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  7 Mt. Lassen Drive - Suite A-134, San Rafael,
   CA  94903, U.S.A., +1(415)492-9861, for further information.
*/

#include <assert.h>

#include "gdevprn.h"

/* Driver for Epson LQ510 */
static dev_proc_print_page(lq510_print_page);
static dev_proc_get_params(lq510_get_params);
static dev_proc_put_params(lq510_put_params);

typedef struct _gx_device_printer_lq510 {
    gx_device_common;
    gx_prn_device_common;
    bool bidirectional;
} gx_device_printer_lq510;

static void
lq510_initialize_device_procs(gx_device *dev)
{
    gdev_prn_initialize_device_procs_mono(dev);

    set_dev_proc(dev, get_params, lq510_get_params);
    set_dev_proc(dev, put_params, lq510_put_params);
}

gx_device_printer_lq510 gs_lq510_device =
{
    prn_device_std_body(gx_device_printer_lq510, lq510_initialize_device_procs, "lq510",
    DEFAULT_WIDTH_10THS, DEFAULT_HEIGHT_10THS,
    360, 360,
    0.12, 0.53, 0.12, 0.33,	/* margins - left, bottom, right, top */
    1, lq510_print_page),
    false
};

/* ------ Internal routines ------ */

/* Forward references */
static void dot24_output_run(byte *data, int count, gp_file *prn_stream);
static void dot24_filter_bitmap(byte *data, byte *out, int count, int *output_count, bool print_even_dots);
static void dot24_print_line(byte *out_temp, byte *out, byte *out_end, bool x_high, bool y_high, bool print_even_dots, int xres, int bytes_per_pos, gp_file *prn_stream);
static void dot24_print_line_backwards(byte*out_temp, byte *out, byte *out_end, bool x_high, bool y_high, bool print_even_dots, int xres, int bytes_per_pos, gp_file *prn_stream);
static void dot24_print_block(byte *out_temp, int pos, byte *blk_start, byte *blk_end, bool x_high, bool print_even_dots, int bytes_per_pos, gp_file *prn_stream);
static void dot24_skip_lines(int lines, bool y_high, gp_file *prn_stream);

/* Send the page to the printer. */
static int
dot24_print_page(gx_device_printer *pdev, gp_file *prn_stream, char *init_string, int init_len, bool bidirectional)
{
    const int xres = (int)pdev->x_pixels_per_inch;
    const int yres = (int)pdev->y_pixels_per_inch;
    const bool x_high = (xres == 360);
    const bool y_high = (yres == 360);
    const uint line_size = gdev_prn_raster(pdev);
    const uint in_size = line_size * 24 * (y_high ? 2 : 1);
    byte * const in = (byte *)gs_malloc(pdev->memory, in_size, 1, "dot24_print_page (in)");
    const uint out_size = line_size * 24;
    byte * const out = (byte *)gs_malloc(pdev->memory, out_size, 1, "dot24_print_page (out)");
    byte * const out_temp = (byte *)gs_malloc(pdev->memory, out_size, 1, "dot24_print_page (out_temp)");
    const int dots_per_pos = xres / 60;
    const int bytes_per_pos = dots_per_pos * 3;
    int lnum = 0, printer_lnum = 0;
    int cycle = 0;
    bool forward = true;

    /* Check allocations */
    if (in == 0 || out == 0 || out_temp == 0)
    {
        if (out_temp)
            gs_free(pdev->memory, (char *)out_temp, out_size, 1, "dot24_print_page (out_temp)");
        if (out)
            gs_free(pdev->memory, (char *)out, out_size, 1, "dot24_print_page (out)");
        if (in)
            gs_free(pdev->memory, (char *)in, in_size, 1, "dot24_print_page (in)");
        return_error(gs_error_VMerror);
    }

    assert(xres == 180 || xres == 360);
    assert(yres == 180 || yres == 360);

    /* Initialize the printer and reset the margins. */
    gp_fwrite(init_string, init_len - 1, sizeof(char), prn_stream);
    gp_fputc((int)(pdev->width / pdev->x_pixels_per_inch * 10) + 2,
        prn_stream);

    // We have a couple of different strategies for printing depending on the resolution.
    // These are intended to distribute lines between the top and bottom lines of the
    // printhead, make sure the printhead isn't moving back and forth across the same
    // pixels repeatedly and thus making obvious smear marks, and generally produce uniform
    // output.

    if (x_high && y_high)
    {
        // 360x360 strategy:
        // Post-seek case:
        //     - 0-35 - empty
        //     - 36-47 (evens) - printing "odd" dots
        //     - 36-47 (odds) - will print next time
        // Normal case:
        //     - 0-11 (evens) - printing one set of dots
        //     - 0-11 (odds) - done
        //     - 12-23 (evens) - printing one set of dots
        //     - 12-23 (odds) - one set of dots printed
        //     - 24-35 (evens) - printing one set of dots
        //     - 24-35 (odds) - one set of dots printed
        //     - 36-47 (evens) - printing one set of dots
        //     - 36-47 (odds) - printing next time
        //
        // We alternate between advancing 11 and 13 lines,
        // and we also print "even" dots for two cycles, then print "odd" dots for two cycles.

        lnum = -36;
    }
    else if (x_high)
    {
        // 360x180 strategy:
        // Post-seek case:
        //     - 0-11 - empty
        //     - 12-23 - printing "odd" dots
        // Normal case:
        //     - 0-11 - printing "even" dots
        //     - 12-23 - printing "odd" dots
        //
        // We advance 12 lines every time.

        lnum = -12;
    }
    else if (y_high)
    {
        // 180x360 strategy:
        // Post-seek case:
        //     - 0-22 (evens) - empty
        //     - 1-23 (odds) - empty
        //     - 24-47 (evens) - printing
        //     - 24-47 (odds) - printed next time
        // Normal case:
        //     - 0-22 (evens) - printing
        //     - 1-23 (odds) - printed last time
        //     - 24-46 (evens) - printing
        //     - 25-47 (odds) - will print next time
        //
        // We advance 25 lines every time.

        lnum = -24;
    }
    else
    {
        lnum = -12;
    }

    /* Print lines of graphics */
    while (lnum < pdev->height)
    {
        byte *in_end = NULL;
        byte *out_end = NULL;
        byte *inp = NULL;
        int lcnt = 0;

        memset(in, 0, in_size);

        // Copy a full block of scan lines
        if (y_high)
        {
            inp = in;
            // Even lines first
            for (lcnt = 0; lcnt < 24; lcnt++, inp += line_size)
            {
                const int line = lnum + lcnt * 2;
                if (line < 0 || !gdev_prn_copy_scan_lines(pdev, line, inp, line_size))
                {
                    memset(inp, 0, line_size);
                }
            }

            // Odd lines go to the end of the buffer. We aren't going to print
            // them now, but we need them if we decide to seek.
            assert(inp == in + line_size * 24);

            for (lcnt = 0; lcnt < 24; lcnt++, inp += line_size)
            {
                const int line = lnum + lcnt * 2 + 1;
                if (line < 0 || !gdev_prn_copy_scan_lines(pdev, line, inp, line_size))
                {
                    memset(inp, 0, line_size);
                }
            }
            assert(inp == in + in_size);
        }
        else
        {
            inp = in;
            for (lcnt = 0; lcnt < 24; lcnt++, inp += line_size)
            {
                const int line = lnum + lcnt;
                if (line < 0 || !gdev_prn_copy_scan_lines(pdev, line, inp, line_size))
                {
                    memset(inp, 0, line_size);
                }
            }
            assert(inp == in + in_size);
        }

        // Seek if the block starts with empty lines
        if (in[0] == 0 && !memcmp((char *)in, (char *)in + 1, line_size - 1))
        {
            int empty_lines = 0;
            int empty_lines_needed = 0;

            if (!y_high)
            {
                empty_lines = 1;
                empty_lines_needed = (x_high ? 12 : 12);

                while (empty_lines < 24 && !memcmp((char*)in, (char*)in + line_size * empty_lines, line_size))
                {
                    empty_lines++;
                }
            }
            else
            {
                empty_lines_needed = (x_high ? 36 : 24);

                for (empty_lines = 0; empty_lines < 48; empty_lines++)
                {
                    // we put the odd lines in the bottom half of the input buffer, so we need some
                    // contortions to iterate through them here
                    const char* target_line = (const char*)(in + (line_size * ((empty_lines / 2) + (empty_lines % 2 == 0 ? 0 : 24))));
                    if (memcmp((char*)in, target_line, line_size))
                    {
                        break;
                    }

                    assert(empty_lines != 0 || (byte*)target_line == (byte*)in);
                }
            }

            if (empty_lines > empty_lines_needed)
            {
                lnum += (empty_lines - empty_lines_needed);
                continue;
            }
        }

        if (lnum < 0 || lnum < printer_lnum)
        {
            // We can't actually put the printhead at a negative position.
            // Instead, fiddle the buffers so it looks like it is.
            assert(printer_lnum < 4);

            if (lnum % 2 != printer_lnum % 2)
            {
                dot24_skip_lines(1, y_high, prn_stream);
                printer_lnum++;
            }

            assert((printer_lnum - lnum) % 2 == 0);

            for (int real_line = 0; real_line < 24; real_line++)
            {
                const int document_line = lnum + real_line * 2;
                const int printhead_line = (document_line - printer_lnum) / 2;

                assert((document_line - printer_lnum) % 2 == 0);
                assert(printhead_line < real_line);

                if (printhead_line >= 0 && printhead_line < 24)
                {
                    memcpy(in + (printhead_line * line_size), in + (real_line * line_size), line_size);
                    assert(printhead_line * line_size + line_size <= in_size);
                }

                memset(in + (real_line * line_size), 0, line_size);
                assert(real_line * line_size + line_size <= in_size);
            }
        }
        else if (printer_lnum != lnum)
        {
            assert(printer_lnum < lnum);

            dot24_skip_lines(lnum - printer_lnum, y_high, prn_stream);
            printer_lnum = lnum;
        }

        out_end = out;
        inp = in;
        in_end = in + line_size;

        for (; inp < in_end; inp++, out_end += 24)
        {
            memflip8x8(inp, line_size, out_end, 3);
            memflip8x8(inp + line_size * 8, line_size, out_end + 1, 3);
            memflip8x8(inp + line_size * 16, line_size, out_end + 2, 3);
        }

        assert(out_end <= out + out_size);

        if (forward)
        {
            dot24_print_line(out_temp, out, out_end, x_high, y_high, !x_high || cycle < 2, xres, bytes_per_pos, prn_stream);
        }
        else
        {
            dot24_print_line_backwards(out_temp, out, out_end, x_high, y_high, !x_high || cycle < 2, xres, bytes_per_pos, prn_stream);
        }

        if (bidirectional)
        {
            forward = !forward;
        }

        if (x_high && y_high)
        {
            lnum += (lnum % 2 == 0 ? 11 : 13);
        }
        else if (x_high)
        {
            lnum += 12;
        }
        else if (y_high)
        {
            lnum += 25;
        }
        else
        {
            lnum += 12;
        }

        cycle = (cycle + 1) % 4;
    }

    /* Eject the page and reinitialize the printer */
    gp_fputs("\f\033@", prn_stream);
    gp_fflush(prn_stream);

    gs_free(pdev->memory, (char *)out_temp, out_size, 1, "dot24_print_page (out_temp)");
    gs_free(pdev->memory, (char *)out, out_size, 1, "dot24_print_page (out)");
    gs_free(pdev->memory, (char *)in, in_size, 1, "dot24_print_page (in)");

    return 0;
}

void dot24_skip_lines(int lines, bool y_high, gp_file *prn_stream)
{
    if (!y_high)
    {
        // printer is always initialized such that \n corresponds to a 1/360 inch
        // page feed
        lines *= 2;
    }

    /* Vertical tab to the appropriate position. */
    while ((lines >> 1) > 255)
    {
        gp_fputs("\x1bJ\xff", prn_stream);
        lines -= 255 * 2;
    }

    if (lines)
    {
        if (lines >> 1)
            gp_fprintf(prn_stream, "\x1bJ%c", lines >> 1);
        if (lines & 1)
            gp_fprintf(prn_stream, "\x1b+%c\n\x1b+%c", 1, 0);
    }
}

void dot24_print_line(byte *out_temp, byte *in_start, byte *in_end, bool x_high, bool y_high, bool print_even_dots, int xres, int bytes_per_pos, gp_file *prn_stream)
{
    const byte *orig_in = in_start;
    byte *blk_start;
    byte *blk_end;

    while (in_end - 3 >= in_start && in_end[-1] == 0
        && in_end[-2] == 0 && in_end[-3] == 0)
        in_end -= 3;

    for (; in_start < in_end;)
    {
        /* Skip to the first position that isn't zero */
        for (blk_start = in_start; blk_start < in_end; blk_start += bytes_per_pos)
        {
            bool zero = true;
            for (int i = 0; i < bytes_per_pos && blk_start + i < in_end; i++)
            {
                if (blk_start[i] != 0)
                {
                    zero = false;
                    break;
                }
            }
            if (!zero)
            {
                break;
            }
        }

        if (blk_start >= in_end)
        {
            // done!
            break;
        }

        /* Seek until we find a zero gap that's at least (xres * 3) / 2 (0.5 inch) */
        for (blk_end = blk_start + bytes_per_pos; blk_end < in_end; blk_end += bytes_per_pos)
        {
            bool zero = true;
            const int blk_gap_len = (xres * 3) / 2; // xres is the DPI and there are 3 bytes per column (24 pins)
            for (int i = 0; i < blk_gap_len && blk_end + i < in_end; i++)
            {
                if (blk_end[i] != 0)
                {
                    zero = false;
                    break;
                }
            }
            if (zero)
            {
                break;
            }
        }

        if (blk_end > in_end)
        {
            blk_end = in_end;
        }

        assert((blk_start - orig_in) % bytes_per_pos == 0);

        dot24_print_block(out_temp, (blk_start - orig_in) / bytes_per_pos, blk_start, blk_end, x_high, print_even_dots, bytes_per_pos, prn_stream);

        in_start = blk_end;
    }
}

void dot24_print_line_backwards(byte * out_temp, byte *in_start, byte *in_end, bool x_high, bool y_high, bool print_even_dots, int xres, int bytes_per_pos, gp_file *prn_stream)
{
    const byte *orig_in = in_start;
    byte *blk_start;
    byte *blk_end;

    while (in_end - 3 >= in_start && in_end[-1] == 0
        && in_end[-2] == 0 && in_end[-3] == 0)
        in_end -= 3;

    for (; in_start < in_end;)
    {
        /* Skip to the last position that isn't zero */
        for (blk_end = in_end; blk_end > in_start; blk_end -= bytes_per_pos)
        {
            bool zero = true;
            for (int i = -1; i >= -bytes_per_pos && blk_end + i >= in_start; i--)
            {
                if (blk_end[i] != 0)
                {
                    zero = false;
                    break;
                }
            }
            if (!zero)
            {
                break;
            }

            // some trickery because in_end isn't guaranteed to fall in a multiple of bytes_per_pos
            if ((blk_end - orig_in) % bytes_per_pos != 0)
            {
                blk_end += bytes_per_pos - (blk_end - orig_in) % bytes_per_pos;
            }

            assert((blk_end - orig_in) % bytes_per_pos == 0);
        }

        if (blk_end <= in_start)
        {
            // done!
            break;
        }

        blk_start = blk_end - (blk_end - orig_in) % bytes_per_pos;

        if (blk_start == blk_end)
        {
            blk_start -= bytes_per_pos;
        }

        assert((blk_start - orig_in) % bytes_per_pos == 0);

        /* Seek until we find a zero gap that's at least (xres * 3) / 2 (0.5 inch) */
        for (; blk_start >= in_start; blk_start -= bytes_per_pos)
        {
            bool zero = true;
            const int blk_gap_len = (xres * 3) / 2; // xres is the DPI and there are 3 bytes per column (24 pins)
            for (int i = -1; i >= -blk_gap_len && blk_start + i >= in_start; i--)
            {
                if (blk_start[i] != 0)
                {
                    zero = false;
                    break;
                }
            }
            if (zero)
            {
                break;
            }
        }

        if (blk_start < in_start)
        {
            blk_start = in_start;
        }

        assert((blk_start - orig_in) % bytes_per_pos == 0);

        dot24_print_block(out_temp, (blk_start - orig_in) / bytes_per_pos, blk_start, blk_end, x_high, print_even_dots, bytes_per_pos, prn_stream);

        in_end = blk_start;
    }
}

void dot24_print_block(byte *out_temp, int pos, byte *blk_start, byte *blk_end, bool x_high, bool print_even_dots, int bytes_per_pos, gp_file *prn_stream)
{
    byte *seg_start = NULL;
    byte *seg_end = NULL;
    int seg_rel_pos = 0;
    const int bytes_per_rel_pos = (bytes_per_pos / 3) / (x_high ? 2 : 1);

    assert(bytes_per_rel_pos == 3);

    // we're going to be using relative seeks inside the loop, so start out with an absolute seek to the start of the block
    gp_fprintf(prn_stream, "\033$%c%c", pos % 256, pos / 256);

    if (!print_even_dots)
    {
        // skip 1/360 to get to dot 1
        gp_fprintf(prn_stream, "\x1b*\x28%c%c%c%c%c", 1, 0, 0, 0, 0);
    }

    // buffer up what we're supposed to be printing
    if (x_high)
    {
        int count_to_print = 0;
        dot24_filter_bitmap(blk_start, out_temp, blk_end - blk_start, &count_to_print, print_even_dots);

        blk_start = out_temp;
        blk_end = blk_start + count_to_print;
    }
    else
    {
        memcpy(out_temp, blk_start, blk_end - blk_start);
        blk_end = out_temp + (blk_end - blk_start);
        blk_start = out_temp;
        
    }

    for (; blk_start < blk_end;)
    {
        for (seg_start = blk_start; seg_start < blk_end; seg_start += bytes_per_rel_pos)
        {
            bool zero = true;
            for (int i = 0; i < bytes_per_rel_pos && seg_start + i < blk_end; i++)
            {
                if (seg_start[i] != 0)
                {
                    zero = false;
                    break;
                }
            }
            if (!zero)
            {
                break;
            }
        }

        if (seg_start >= blk_end)
        {
            // done!
            break;
        }

        for (seg_end = seg_start; seg_end < blk_end; seg_end += bytes_per_rel_pos)
        {
            bool zero = true;
            const int seg_gap_len = 4;

            for (int i = 0; i < bytes_per_rel_pos * seg_gap_len && seg_end + i < blk_end; i++)
            {
                if (seg_end[i] != 0)
                {
                    zero = false;
                    break;
                }
            }
            if (zero)
            {
                break;
            }
        }

        if (seg_end >= blk_end)
        {
            seg_end = blk_end;
        }

        assert(seg_start != seg_end);

        // go to the start of the segment
        seg_rel_pos = (seg_start - blk_start) / bytes_per_rel_pos;
        assert((seg_start - blk_start) % bytes_per_rel_pos == 0);

        if (seg_rel_pos != 0)
        {
            gp_fprintf(prn_stream, "\033\\%c%c", seg_rel_pos % 256, seg_rel_pos / 256);
        }

        // print
        dot24_output_run(seg_start, seg_end - seg_start, prn_stream);

        blk_start = seg_end;
    }

    gp_fputc('\r', prn_stream);
}

/* Output a single graphics command. */
static void
dot24_output_run(byte *data, int count, gp_file *prn_stream)
{
    int xcount = count / 3;

    if (count == 0)
    {
        return;
    }

    gp_fputc(033, prn_stream);
    gp_fputc('*', prn_stream);
    gp_fputc(39, prn_stream);
    gp_fputc(xcount & 0xff, prn_stream);
    gp_fputc(xcount >> 8, prn_stream);
    gp_fwrite(data, 1, count, prn_stream);
}

static void
dot24_filter_bitmap(byte *data, byte *out, int count, int *output_count, bool print_even_dots)
{
    int i = 0;
    int count_to_print = 0;
    assert(count % 3 == 0);

    for (i = (print_even_dots ? 0 : 3); i < count; i += 6)
    {
        out[(i / 6) * 3 + 0] = data[i + 0];
        out[(i / 6) * 3 + 1] = data[i + 1];
        out[(i / 6) * 3 + 2] = data[i + 2];
        count_to_print += 3;
    }

    *output_count = count_to_print;
}

static int
lq510_print_page(gx_device_printer *pdev, gp_file *prn_stream)
{
    bool bidirectional = ((gx_device_printer_lq510*)pdev)->bidirectional;
    char lq510_init_string[] = "\033@\033P\033l\000\r\033\053\000\033U\000\033x\001\033Q";

    assert(pdev->params_size == sizeof(gx_device_printer_lq510));
    assert(lq510_init_string[12] == 'U');
    lq510_init_string[13] = bidirectional ? 0 : 1; // flag is actually for unidirectional

    return dot24_print_page(pdev, prn_stream, lq510_init_string, sizeof(lq510_init_string), bidirectional);
}

static int
lq510_get_params(gx_device *pdev, gs_param_list *plist)
{
    gx_device_printer_lq510 *lq510 = (gx_device_printer_lq510*)pdev;
    int code = gdev_prn_get_params(pdev, plist);

    assert(pdev->params_size == sizeof(gx_device_printer_lq510));

    if (code < 0 || (code = param_write_bool(plist, "Bidirectional", &lq510->bidirectional)) < 0)
    {
        return code;
    }

    return code;
}

static int
lq510_put_params(gx_device *pdev, gs_param_list *plist)
{
    gx_device_printer_lq510 *lq510 = (gx_device_printer_lq510*)pdev;
    int code = 0;
    int ecode = 0;
    const char *param_name;
    bool bidirectional = lq510->bidirectional;

    assert(pdev->params_size == sizeof(gx_device_printer_lq510));

    switch (code = param_read_bool(plist, (param_name = "Bidirectional"), &bidirectional)) {
    default:
        ecode = code;
        param_signal_error(plist, param_name, ecode);
    case 0:
    case 1:
        break;
    }

    if (ecode < 0)
        return ecode;

    code = gdev_prn_put_params(pdev, plist);
    if (code < 0)
        return code;

    lq510->bidirectional = bidirectional;

    return code;
}
