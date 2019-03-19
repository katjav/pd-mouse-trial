/*******************************************************************************
* Mouse event widget for Pure Data. Written by Katja Vetter. Inspired by other
* GUI classes for Pd, notably Thomas Musil's class iem_event, but not using the
* iemgui framework. Currently highly experimental, do not use in your projects.
* 
* Characteristics:
* 
* - mouse button down event when clicking widget area
* - mouse x y coordinates respective to widget during drag and hover
* - mouse x y deltas during drag
* - no mouse button up event (yet)
* - no label
* - no visible position indicator
* - fill color (integer or webcolor regular and short)
* - properties dialog implemented as abstraction
* 
* One reason for not using the iemgui framework is to avoid some outdated
* arrangements, in particular the old color definitions and the raute2dollar
* conversion. Iemgui understandably keeps these around for backward
* compatibility but for a new class this is an undesirable burden.
* 
* The reason for not using a Tk properties dialog but an abstraction instead is
* to minimize direct dependency on Tk. This should simplify an eventual port to
* another GUI framework. As a bonus the abstraction can employ all of Pd's
* powerful functionality, like resizing an object through dragging a numbox in
* its properties dialog.
* 
* Replacing some proven conventional methods with experimental approaches
* will probably introduce tons of issues, confusions and doubts. It is an
* exploration of possibilities, nothing definitive.
* 
* Pure Data is the work of Miller Puckette and others. License for this class 
* not decided yet.
* 
* 
* March 2019
* 
*******************************************************************************/


#include "m_pd.h"
#include "g_canvas.h"
#include "m_imp.h"    // for t_class definition

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#ifdef MSW
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h> // for file open
#include <fcntl.h>    // for file open
#endif

#define COLOR_SELECTED 0x0000FF     // outline color when selected (blue)
#define COLOR_NORMAL   0x000000     // standard outline color (black)
#define DEFCOLOR       0xDDDDDD     // default fill color (mouse grey)
#define DEFSIZE        64
#define DEFZOOM        1
#define IOHEIGHT       3

// mousepad is drawn as (at most) 3 rectangles; assign a number to each which
// will be used in the tag string and in 'multi-rect' arguments
#define BASE   1    // 0b001
#define INLET  2    // 0b010
#define OUTLET 4    // 0b100

#define IS_A_FLOAT(atom,index) ((atom+index)->a_type == A_FLOAT)
#define IS_A_SYMBOL(atom,index) ((atom+index)->a_type == A_SYMBOL)



// symbols with constant literal value, no need to store these in each object
static t_symbol* symEmpty;  // default name for send / receive
static t_symbol* symSize;   // selector symbols for input messages
static t_symbol* symColor;
static t_symbol* symPos;
static t_symbol* symZoom;
static t_symbol* symNames;
static t_symbol* symButton; // selector symbols for output messages
static t_symbol* symDrag;
static t_symbol* symHover;
static t_symbol* symDeltas;


// ---------- mousepad ---------------------------------------------------------


t_widgetbehavior mousepad_widgetbehavior;
static t_class *mousepad_class;


typedef struct
{
    t_object  obj;
    t_glist*  glist;                  // owning glist or 'canvas'
    int       width;                  // object rectangle width (nominal)
    int       height;                 // object rectangle height (nominal)
    int       xval;                   // mouse x relative to object (nominal)
    int       yval;                   // mouse y relative to object (nominal)
    int       pixw;                   // width expressed in true pixels
    int       pixh;                   // height expressed in true pixels
    int       zoomfactor;             // zoom factor of owning glist (1 or 2)
    int       buttonstate;            // mouse button state 1 or 0
    int       intcolor;               // fill color expressed as integer
    
    // send & receive parameters
    t_symbol* sendname;               // settable send name (expanded)
    t_symbol* receivename;            // settable receive name (expanded)
    t_symbol* sendname_unexpanded;
    t_symbol* receivename_unexpanded;
    t_symbol* objID;                  // object pointer expressed as hex symbol
    t_symbol* sendname_fixed;         // "from-mousepad-<objID>"
    t_symbol* receivename_fixed;      // "to-mousepad-<objID>"
    
    t_clock*  initclock;
    t_atom    out[3];
} t_mousepad;



////////////////////////////////////////////////////////////////////////////////
///////////// generalized functions ////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

// Utility functions and drawings functions that do not depend on the mousepad
// struct definition. These functions could be abstracted in a separate file
// and reused for cousin widget classes, but at the moment there isn't any.


// ---------- utility functions ------------------------------------------------

// Pd can't save numbers with enough precision for 24 bit colors. Therefore
// colors are stored as symbols representing the webcolor format, and this
// format is also accepted as argument (besides floatarg). 
// Color symbols range between "#000000" and "#FFFFFF". Each hexadecimal digit
// corresponds to a nibble in the integer representation. Shorthand webcolors
// with 3 hex digits are also recognized and converted to 24 bit notation.


// Make web color string (6 hex digits) with prefix '#' and null terminator,
// and create symbol. This might be done using sprintf() but it must be robust.
// No numbers larger than 0xFFFFFF.
t_symbol* int2hexcolor(int intcolor)
{
    char hexcolor[8];
    int nibblemask = 0xF00000; // mask starts with most significant nibble
    int i, j, nibble;

    hexcolor[0] = '#';
    
    // convert 6 nibbles to ascii characters
    for(i=1, j=5; i<7; i++, j--)
    {
        nibble = (intcolor & nibblemask) >> (4 * j);
        if(nibble < 10) hexcolor[i] = nibble + 48;  // 0 till 9
        else hexcolor[i] = nibble + 55;             // A till F
        nibblemask >>= 4;
    }
    hexcolor[7] = 0; // EOL
    
    t_symbol* colorsymbol = gensym(hexcolor);
    
    return colorsymbol;
}


// Symbol to int conversion for web color names with 3 or 6 hex digits.
// For other number of hex digits, result is unspecified but not harmful.
// hexcolor[0] is assumed '#' but not checked here.
int hexcolor2int(const char hexcolor[])
{
    int i, h, intcolor = 0;
    int intbuf[6] = {0};
    
    for(i = 0; i < 6; i++)      // ascii characters to ints
    {
        h = hexcolor[i+1];
        
        if(!h) break;           // EOL
 
        if      ((h >= 48) && (h <= 57))  intbuf[i] = h - 48;     // 0 till 9
        else if ((h >= 65) && (h <= 70))  intbuf[i] = h - 55;     // A till F
        else if ((h >= 97) && (h <= 102)) intbuf[i] = h - 87;     // a till f
    }
    
    if(i == 3) // if 3 hex digits found, 'up-sample-and-hold' to 6
    {
        intbuf[4] = intbuf[5] = intbuf[2];
        intbuf[2] = intbuf[3] = intbuf[1];
        intbuf[1] = intbuf[0];
    }
    
    for(i = 0; i < 6; i++)      // ints to single int
    {
        intcolor <<= 4;
        intcolor += intbuf[i];
    }

    return intcolor;
}


// ---------- drawing calls for tk ---------------------------------------------

// Generic functions for drawing and configuring rectangles (base, IOlets etc.)
// Argument 'obj' is the unique object ID: its pointer cast to t_int.
// Character 'part' refers to: base rectangle, inlet or outlet.
// Argument 'w' is outline width.
// Array pix[] contains coordinates x1, y1, x2, y2.

static void draw_rect(t_canvas* canv, t_int obj, char part, int pix[], int w, int isnew)
{
    if(isnew)
        sys_vgui(".x%lx.c create rectangle %d %d %d %d -width %d -tags %lx%c\n",
                canv, pix[0], pix[1], pix[2], pix[3], w, obj, part);
    else
        sys_vgui(".x%lx.c coords %lx%c %d %d %d %d\n",
                canv, obj, part, pix[0], pix[1], pix[2], pix[3]);
}


// reconfigure outline color of base rectangle when object is (de)selected
static void draw_outlinecolor(t_canvas* canv, t_int obj, char part, int color)
{
    sys_vgui(".x%lx.c itemconfigure %lx%c -outline #%06x\n",
                canv, obj, part, color);
}


static void draw_fillcolor(t_canvas* canv, t_int obj, char part, int color)
{
    sys_vgui(".x%lx.c itemconfigure %lx%c -fill #%06x\n",
                canv, obj, part, color);
}


static void draw_erase(t_canvas* canv, t_int obj, char part)
{
    sys_vgui(".x%lx.c delete %lx%c\n", canv, obj, part);
}


////////////////////////////////////////////////////////////////////////////////
////////////// mousepad specific functions /////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

// In their current form, the functions below depend on the mousepad struct
// definition. Part of it might be generalized for reuse in cousin widget
// classes if similarity between classes justifies a shared struct. Currently
// there is no use case, therefore I wouldn't know yet which part to extract.
// Note that IEM GUIs, despite their shared struct and function pointers, define
// most methods individually because of the difference between widgets.


// ----------- pixel calculator ------------------------------------------------

// Calculate pixel coordinates of rectangles that must be (re)drawn.
// Functions text_*pix() (in g_graph.c) will compensate parent offset.

static void mousepad_draw(t_mousepad *mp, int isnew, int rects)
{
    if((!isnew) & (!glist_isvisible(mp->glist))) return;
    
    t_canvas* canv = glist_getcanvas(mp->glist);
    int zoom       = mp->zoomfactor;
    int xpos       = text_xpix(&mp->obj, mp->glist);
    int ypos       = text_ypix(&mp->obj, mp->glist);
    int width      = mp->pixw;          // pixw and pixh do match zoom factor...
    int height     = mp->pixh;
    int iowidth    = IOWIDTH * zoom;    // ...but IOWIDTH and IOHEIGHT not
    int ioheight   = IOHEIGHT * zoom;
    
    if(!rects)  // figure out what to draw if not specified
    {
        rects = BASE;
        if(mp->sendname == symEmpty) rects |= INLET;
        if(mp->receivename == symEmpty) rects |= OUTLET;
    }
    
    if(rects & BASE)
    {
        int base[4];
        base[0] = xpos;
        base[1] = ypos;
        base[2] = xpos + width;
        base[3] = ypos + height;
        draw_rect(canv, (t_int)mp, BASE, base, zoom, isnew);
    }
    
    if(rects & INLET)
    {
        int inlet[4];
        inlet[0] = xpos;
        inlet[1] = ypos;
        inlet[2] = xpos + iowidth;
        inlet[3] = ypos + ioheight;
        draw_rect(canv, (t_int)mp, INLET, inlet, zoom, isnew);
    }
    
    if(rects & OUTLET)
    {
        int outlet[4];
        outlet[0] = xpos;
        outlet[1] = ypos + height - ioheight;
        outlet[2] = xpos + iowidth;
        outlet[3] = ypos + height;
        draw_rect(canv, (t_int)mp, OUTLET, outlet, zoom, isnew);
    }
    
    if(isnew) draw_fillcolor(canv, (t_int)mp, BASE, mp->intcolor);
    else canvas_fixlinesfor(mp->glist, (t_text*)mp);
}


// called by mousepad_send() and mousepad_receive() if send/receivable changes
static void mousepad_change_io(t_mousepad *mp, int change, int iolet)
{
    if(!glist_isvisible(mp->glist)) return;
    
    t_canvas* canv = glist_getcanvas(mp->glist);
    
    if(change == 1) mousepad_draw(mp, 1, iolet);
    else if(change == -1) draw_erase(canv, (t_int)mp, iolet);
}


// ----------- t_widgetbehaviour callback functions ---------------------------- 


void mousepad_vis(t_gobj *z, t_glist *glist, int vis)
{
    t_mousepad *mp = (t_mousepad*)z;
    t_canvas *canv = glist_getcanvas(glist);
    
    if(vis) 
    {
        const int isnew = 1;
        mousepad_draw(mp, isnew, 0);
    }
    
    else
    {
        draw_erase(canv, (t_int)mp, BASE);
        if(mp->sendname == symEmpty)    draw_erase(canv, (t_int)mp, INLET);
        if(mp->receivename == symEmpty) draw_erase(canv, (t_int)mp, OUTLET);
        sys_unqueuegui(z);
    }
}


void mousepad_displace(t_gobj *z, t_glist *glist, int dx, int dy)
{
    const int isnew = 0;
    t_mousepad *mp = (t_mousepad *)z;
    
    mp->obj.te_xpix += dx;
    mp->obj.te_ypix += dy;
    
    mousepad_draw(mp, isnew, 0);
}


void mousepad_select(t_gobj *z, t_glist *glist, int selected)
{
    t_mousepad *mp = (t_mousepad *)z;
    int outlinecolor = COLOR_NORMAL;
    if(selected) outlinecolor = COLOR_SELECTED;
    
    if(glist_isvisible(glist))
    {
        t_canvas* canv = glist_getcanvas(glist);
        draw_outlinecolor(canv, (t_int)mp, BASE, outlinecolor);
    }
}


void mousepad_delete(t_gobj *z, t_glist *glist)
{
    canvas_deletelinesfor(glist, (t_text*)z);
}

// Functions text_*pix() (in g_graph.c) compensate for parent offset.
static void mousepad_getrect(t_gobj *z, t_glist *glist,
                                int *xp1, int *yp1, int *xp2, int *yp2)
{
    t_mousepad *mp = (t_mousepad *)z;
    
    *xp1 = text_xpix(&mp->obj, glist);
    *yp1 = text_ypix(&mp->obj, glist);
    
    *xp2 = *xp1 + mp->pixw;
    *yp2 = *yp1 + mp->pixh;
}


// --------- other callback functions ------------------------------------------


static void mousepad_save(t_gobj *z, t_binbuf *b)
{
    t_mousepad *mp = (t_mousepad *)z;
    
    // normalize position to zoom factor 1
    int xpos   = (int)mp->obj.te_xpix / mp->zoomfactor;
    int ypos   = (int)mp->obj.te_ypix / mp->zoomfactor;
    
    binbuf_addv(b, "ssiisiisss", gensym("#X"), gensym("obj"),
        xpos, ypos,
        atom_getsymbol(binbuf_getvec(mp->obj.te_binbuf)),
        mp->width, mp->height, 
        mp->sendname_unexpanded, mp->receivename_unexpanded,
        int2hexcolor(mp->intcolor)); // store color as symbol
    binbuf_addv(b, ";");
}


static void mousepad_motion(t_mousepad *mp, t_floatarg dx, t_floatarg dy)
{
    int deltax = (t_int)dx;
    int deltay = (t_int)dy;
    
    if ((deltax | deltay) == 0) return; // do not send output if nothing changed
    
    int sendable = (mp->sendname != symEmpty);
    
    mp->xval += deltax;
    mp->yval += deltay;
    
    // xy relative to gui
    SETFLOAT(mp->out,   (t_float)(mp->xval / mp->zoomfactor));
    SETFLOAT(mp->out+1, (t_float)(mp->yval / mp->zoomfactor));
    outlet_anything(mp->obj.ob_outlet, symDrag, 2, mp->out);
    if(sendable && mp->sendname->s_thing)
        typedmess(mp->sendname->s_thing, symDrag, 2, mp->out);
    
    // xy deltas
    SETFLOAT(mp->out,   (t_float)(deltax / mp->zoomfactor));
    SETFLOAT(mp->out+1, (t_float)(deltay / mp->zoomfactor));
    outlet_anything(mp->obj.ob_outlet, symDeltas, 2, mp->out);
    if(sendable && mp->sendname->s_thing)
        typedmess(mp->sendname->s_thing, symDeltas, 2, mp->out);
}


// This function is called when the mouse hovers over the canvas or when a
// mouse click on the gui area happens.
static int mousepad_click(t_gobj *z, struct _glist *glist, int xpix, int ypix, 
                            int shift, int alt, int dbl, int buttonstate)
{
    t_mousepad* mp = (t_mousepad *)z;
    int sendable   = (mp->sendname != symEmpty);
    int xpos       = text_xpix(&mp->obj, glist);
    int ypos       = text_ypix(&mp->obj, glist);
  
    if(buttonstate != mp->buttonstate)
    {
        SETFLOAT(mp->out, (t_float)buttonstate);
        SETFLOAT(mp->out+1, (t_float)shift);
        SETFLOAT(mp->out+2, (t_float)(alt?1:0));
        outlet_anything(mp->obj.ob_outlet, symButton, 3, mp->out);
        if(sendable && mp->sendname->s_thing)
            typedmess(mp->sendname->s_thing, symButton, 3, mp->out);
        mp->buttonstate = buttonstate;
    }
  
    mp->xval = xpix - xpos;
    mp->yval = ypix - ypos;
    SETFLOAT(mp->out, (t_float)(mp->xval / mp->zoomfactor));
    SETFLOAT(mp->out+1, (t_float)(mp->yval / mp->zoomfactor));
    
    // if mouse click, pass motion function pointer and send drag coords
    if(buttonstate)
    {
        glist_grab(mp->glist, &mp->obj.te_g, (t_glistmotionfn)mousepad_motion, 
            0, (t_float)xpix, (t_float)ypix);
        outlet_anything(mp->obj.ob_outlet, symDrag, 2, mp->out);
        if(sendable && mp->sendname->s_thing)
            typedmess(mp->sendname->s_thing, symDrag, 2, mp->out);
    }
    
    // if mouse up, send hover coords
    else
    {
        outlet_anything(mp->obj.ob_outlet, symHover, 2, mp->out);
        if(sendable && mp->sendname->s_thing)
            typedmess(mp->sendname->s_thing, symHover, 2, mp->out);
    }
    
    return (1);
}


// As long as class mousepad is an external, field 'gl_zoom' in the glist cannot
// be accessed directly since this will give undesired effects when using with
// non-zooming Pd versions. Therefore wait until Pd calls with a zoom
// message, then store value in the mousepad object struct.
static void mousepad_zoom(t_mousepad *mp, t_floatarg zoomfactor)
{
    mp->pixw       = mp->width * (int)zoomfactor;
    mp->pixh       = mp->height * (int)zoomfactor;
    mp->zoomfactor = zoomfactor;
    int sendable   = (mp->sendname != symEmpty);
    
    // push message to listeners as this can be considered an event
    SETFLOAT(mp->out, (zoomfactor));
    outlet_anything(mp->obj.ob_outlet, symZoom, 1, mp->out);
    if(sendable && mp->sendname->s_thing)
        typedmess(mp->sendname->s_thing, symZoom, 1, mp->out);
    if(mp->sendname_fixed->s_thing)
        typedmess(mp->sendname_fixed->s_thing, symZoom, 1, mp->out);
}


// --------- settings ----------------------------------------------------------

// methods exposed to user and properties dialog


// parameters for which user and properties patch can request values
static void mousepad_get(t_mousepad *mp, t_symbol *selector)
{
    int argc     = 0;
    int sendable = (mp->sendname != symEmpty);
    
    if(selector == symSize)
    {
        SETFLOAT(mp->out,   (t_float)(mp->width));
        SETFLOAT(mp->out+1, (t_float)(mp->height));
        argc = 2;
    }
    
    else if(selector == symNames)
    {
        SETSYMBOL(mp->out, mp->sendname_unexpanded);
        SETSYMBOL(mp->out+1, mp->receivename_unexpanded);
        argc = 2;
    }
    
    else if(selector == symColor)
    {
        SETFLOAT(mp->out, mp->intcolor);
        argc = 1;
    }
    
    else if(selector == symPos) // nominal object position on canvas
    {
        int xpos = text_xpix(&mp->obj, mp->glist) / mp->zoomfactor;
        int ypos = text_ypix(&mp->obj, mp->glist) / mp->zoomfactor;
        SETFLOAT(mp->out,   (t_float)xpos);
        SETFLOAT(mp->out+1, (t_float)ypos);
        argc = 2;
    }
    
    else if(selector == symZoom)
    {
        SETFLOAT(mp->out,   (t_float)(mp->zoomfactor));
        argc = 1;
    }
    
    if(argc)
    {
        outlet_anything(mp->obj.ob_outlet, selector, argc, mp->out);
        if(sendable && mp->sendname->s_thing)
            typedmess(mp->sendname->s_thing, selector, argc, mp->out);
        if(mp->sendname_fixed->s_thing)
            typedmess(mp->sendname_fixed->s_thing, selector, argc, mp->out);
    }
}


static void mousepad_status(t_mousepad *mp)
{
    t_symbol* color = int2hexcolor(mp->intcolor);
    
    post("mousepad width: %d", mp->width);
    post("mousepad height: %d", mp->height);
    post("mousepad send name: %s", mp->sendname_unexpanded->s_name);
    post("mousepad receive name: %s", mp->receivename_unexpanded->s_name);
    post("mousepad color is %s", color->s_name);
    post("object ID is %s", mp->objID->s_name);
}


// functionally equivalent to mousepad_displace but different arguments
static void mousepad_delta(t_mousepad *mp, t_floatarg dx, t_floatarg dy)
{
    const int isnew  = 0;
    mp->obj.te_xpix += (int)dx * mp->zoomfactor;
    mp->obj.te_ypix += (int)dy * mp->zoomfactor;

    mousepad_draw(mp, isnew, 0);
}


static void mousepad_pos(t_mousepad *mp, t_floatarg xpos, t_floatarg ypos)
{
    const int isnew = 0;
    mp->obj.te_xpix = (int)xpos * mp->zoomfactor;
    mp->obj.te_ypix = (int)ypos * mp->zoomfactor;
    
    mousepad_draw(mp, isnew, 0);
}


static void mousepad_color(t_mousepad *mp, t_symbol *s, int argc, t_atom *argv)
{
    if(!argc) return;
    
    int intcolor = DEFCOLOR;
    
    if(IS_A_FLOAT(argv, 0)) 
    {
        intcolor = (int)atom_getfloatarg(0, 1, argv);
        intcolor &= 0xFFFFFF;
    }
    
    else if(IS_A_SYMBOL(argv, 0))
    {
        t_symbol* hexcolor;
        hexcolor = atom_getsymbolarg(0, 1, argv);
        if(hexcolor->s_name[0] == '#')
            intcolor = hexcolor2int(hexcolor->s_name);
    }

    if(glist_isvisible(mp->glist))
    {
        t_canvas* canv = glist_getcanvas(mp->glist);
        draw_fillcolor(canv, (t_int)mp, BASE, intcolor);
    }

    mp->intcolor = intcolor;
}


// Nominal width and height. If only one argument is given, then height = width.
static void mousepad_size(t_mousepad *mp, int argc, t_atom *argv)
{
    int width = DEFSIZE, height = DEFSIZE;
    
    if(argc >= 1)
    {
        if(IS_A_FLOAT(argv, 0))       width = (int)argv[0].a_w.w_float;
        if(argc == 1) height = width;
        else if(IS_A_FLOAT(argv, 1)) height = (int)argv[1].a_w.w_float;
    }
    
    if(width < 1) width = 1;
    if(height < 1) height = 1;
    
    mp->width = width;
    mp->height = height;
    
    // width and height expressed in screen pixels
    mp->pixw = width  * mp->zoomfactor;
    mp->pixh = height * mp->zoomfactor;
}


static void mousepad_resize(t_mousepad *mp, t_symbol *s, int argc, t_atom *argv)
{
    const int isnew = 0;
    
    mousepad_size(mp, argc, argv);
    mousepad_draw(mp, isnew, 0);
}


// Set dirty flag in top level parent. We only want to set the dirty flag if
// a setting is changed through the properties dialog (and not through a direct
// user call). Therefore the properties dialog must call this function.
static void mousepad_dirty(t_mousepad* mp)
{
    canvas_dirty(mp->glist, 1);
}


// --------- send and receive names --------------------------------------------

// If the argument is an empty symbol (global variable 's_'), "empty" is set
// explicitly. A dollar sign found in the name will be expanded. The unexpanded
// name is stored for later when parameters will be saved.


static void mousepad_send(t_mousepad *mp, t_symbol *sendname)
{
    int was_sendable   = (mp->sendname != symEmpty);
    int is_sendable    = 0;
    t_glist* glist     = mp->glist;
    
    if(sendname == &s_) sendname = symEmpty;            // &s_: global symbol ""
    if(sendname != symEmpty) is_sendable = 1;
    
    mp->sendname_unexpanded = sendname;
    mp->sendname = canvas_realizedollar(glist, sendname); // g_canvas.c
    
    int change = was_sendable - is_sendable;
    if(change) mousepad_change_io(mp, change, INLET);     // draw or erase inlet
}


static void mousepad_receive(t_mousepad *mp, t_symbol *receivename)
{
    int was_receivable = (mp->receivename != symEmpty);
    int is_receivable  = 0;
    t_glist* glist     = mp->glist;
    t_pd* object       = (t_pd*)mp;     // both resolve to '&x->obj.ob_pd'
    
    // if previously receivable, unbind old receive name
    if(was_receivable) pd_unbind(object, mp->receivename);
    
    // new name, bind if not empty
    if(receivename == &s_) receivename = symEmpty;      // &s_: global symbol ""
    mp->receivename_unexpanded = receivename;
    receivename = canvas_realizedollar(glist, receivename);
    if(receivename != symEmpty) is_receivable = 1;
    if(is_receivable) pd_bind(object, receivename);
    mp->receivename = receivename;
    
    int change = was_receivable - is_receivable;
    if(change) mousepad_change_io(mp, change, OUTLET);   // draw or erase outlet
}


// -------- creation, init, deletion, setup ------------------------------------

// Try to fetch unexpanded send- and receive names from binbuf. Binbuf is
// not accessible from within mousepad_new(), therefore the unexpanded names are
// initialized "empty" and a callback is used to update them from the binbuf. In
// the meantime names may be updated via loadbanged messages, in which case they
// are no longer defined "empty" and initialization from binbuf must be skipped.
// Mousepad does not support the '#' shorthand notation for unexpanded dollar
// symbols.


static void mousepad_init_unexpanded(t_mousepad* mp)
{
    t_atom* binbufvec = binbuf_getvec(mp->obj.ob_binbuf);
    
    // if at least 5 atoms in binbuf, assume the names are there
    if(binbuf_getnatom(mp->obj.ob_binbuf) > 4)
    {
        if(mp->sendname_unexpanded == symEmpty)
        {
            char send[80];
            atom_string(binbufvec + 3, send, 80);
            mp->sendname_unexpanded = gensym(send);
        }
        
        if(mp->receivename_unexpanded == symEmpty)
        {
            char receive[80];
            atom_string(binbufvec + 4, receive, 80);
            mp->receivename_unexpanded = gensym(receive);
        }
    }
}


// set up fixed send & receive channels for communication with property dialog
static void mousepad_fixed_sendreceive(t_mousepad* mp)
{
    char objID[40], sendname[40], receivename[40];
    
    sprintf(objID, "%#lX", (t_int)mp);
    sprintf(sendname, "from-mousepad-%#lX", (t_int)mp);
    sprintf(receivename, "to-mousepad-%#lX", (t_int)mp);
    
    mp->objID = gensym(objID);
    mp->sendname_fixed = gensym(sendname);
    mp->receivename_fixed = gensym(receivename);
    
    pd_bind(&mp->obj.ob_pd, mp->receivename_fixed);
}


// arguments are optional but their order is fixed:
// [width height send receive color]
static void *mousepad_new(t_symbol *s, int argc, t_atom *argv)
{
    t_mousepad *mp = (t_mousepad *)pd_new(mousepad_class);
    mp->glist = (t_glist *)canvas_getcurrent();
    outlet_new(&mp->obj, &s_list);

    mp->intcolor     = DEFCOLOR;
    mp->zoomfactor   = DEFZOOM;
    mp->xval         = 0;
    mp->yval         = 0;
    mp->buttonstate  = 0;
    mp->sendname     = symEmpty;
    mp->receivename  = symEmpty;
    
    // process instantiation arguments
    mousepad_size(mp, argc, argv); // argument index 0 and 1
    mousepad_send(mp, atom_getsymbolarg(2, argc, argv));
    mousepad_receive(mp, atom_getsymbolarg(3, argc, argv));
    if(argc >= 5) mousepad_color(mp, s, 1, argv + 4);
    
    // overwrite unexpanded names with defaults, we really can't know them yet
    mp->sendname_unexpanded = symEmpty;
    mp->receivename_unexpanded = symEmpty;
    
    // setup callback method to initialize unexpanded symbols
    mp->initclock = clock_new(mp, (t_method)mousepad_init_unexpanded);
    clock_delay(mp->initclock, 0);
    
    mousepad_fixed_sendreceive(mp);
    
    return (mp);
}


// TODO: close properties dialog (but we don't know the pointer to it)
static void mousepad_free(t_mousepad *mp)
{
    pd_unbind(&mp->obj.ob_pd, mp->receivename_fixed);
    if(mp->receivename != symEmpty) pd_unbind(&mp->obj.ob_pd, mp->receivename);
}


// Open the properties dialog which is an abstraction expected to live in the
// same directory as the external binary. The approach is similar to loading
// a help patch, except that an argument for object ID is passed here.
// Function glob_evalfile() is from m_binbuf.c up till Pd 0.48, but moved to
// g_canvas.c  in 0.49.
// TODO: check whether dir name as stored in class may need expansion ("~/" etc)

void canvas_popabstraction(t_canvas *x); // defined in g_canvas.c


static void mousepad_properties(t_gobj *z, t_glist *owner)
{
    int fd = -1;
    
    t_symbol* dir = z->g_pd->c_externdir; // from class
    t_symbol* file = gensym("mousepad-properties.pd");
    char path[MAXPDSTRING];
    
    int dsize = strlen(dir->s_name);
    int fsize = strlen(file->s_name);
    
    if((dsize + fsize) > (MAXPDSTRING - 4)) return;
    
    // assemble full path
    strcpy(path, dir->s_name);
    if (dir->s_name[dsize - 1] != '/') strcat(path, "/");
    strcat(path, file->s_name);
    
    fd = sys_open(path, O_RDONLY);
    
    if(fd < 0)
    {
        post("could not find mousepad-properties.pd");
        return;
    }
    
    sys_close(fd);
    
    // instantiate mousepad-properties.pd and pass object ID as argument
    t_mousepad* mp = (t_mousepad*)z;
    t_atom objID;
    SETSYMBOL(&objID, mp->objID);

    canvas_setargs(1, &objID);
    glob_evalfile(0, file, dir);  // pops a toplevel window
    canvas_setargs(0, 0);
}


void mousepad_setup(void)
{
    mousepad_class = class_new(gensym("mousepad"), (t_newmethod)mousepad_new,
        (t_method)mousepad_free, sizeof(t_mousepad), 0, A_GIMME, 0);
    class_addmethod(mousepad_class, (t_method)mousepad_motion,
        gensym("motion"), A_FLOAT, A_FLOAT, 0);
    class_addmethod(mousepad_class, (t_method)mousepad_resize,
        gensym("size"), A_GIMME, 0);
    class_addmethod(mousepad_class, (t_method)mousepad_color,
        gensym("color"), A_GIMME, 0);
    class_addmethod(mousepad_class, (t_method)mousepad_delta,
        gensym("delta"), A_FLOAT, A_FLOAT, 0);
    class_addmethod(mousepad_class, (t_method)mousepad_pos,
        gensym("pos"), A_FLOAT, A_FLOAT, 0);
    class_addmethod(mousepad_class, (t_method)mousepad_send,
        gensym("send"), A_DEFSYM, 0);
    class_addmethod(mousepad_class, (t_method)mousepad_receive,
        gensym("receive"), A_DEFSYM, 0);
    class_addmethod(mousepad_class, (t_method)mousepad_status,
        gensym("status"), 0);
    class_addmethod(mousepad_class, (t_method)mousepad_get,
        gensym("get"), A_DEFSYM, 0);
    class_addmethod(mousepad_class, (t_method)mousepad_dirty,
        gensym("dirty"), 0);
    class_addmethod(mousepad_class, (t_method)mousepad_zoom,
        gensym("zoom"), A_CANT, 0);
        
    mousepad_widgetbehavior.w_getrectfn    = mousepad_getrect;
    mousepad_widgetbehavior.w_displacefn   = mousepad_displace;
    mousepad_widgetbehavior.w_selectfn     = mousepad_select;
    mousepad_widgetbehavior.w_activatefn   = NULL;
    mousepad_widgetbehavior.w_deletefn     = mousepad_delete;
    mousepad_widgetbehavior.w_visfn        = mousepad_vis;
    mousepad_widgetbehavior.w_clickfn      = mousepad_click;
    
    class_setwidget(mousepad_class, &mousepad_widgetbehavior);
    class_setsavefn(mousepad_class, mousepad_save);
    class_setpropertiesfn(mousepad_class, mousepad_properties);
    
    symSize         = gensym("size");
    symColor        = gensym("color");
    symNames        = gensym("names");
    symButton       = gensym("button");
    symDrag         = gensym("drag");
    symHover        = gensym("hover");
    symDeltas       = gensym("deltas");
    symEmpty        = gensym("empty");
    symPos          = gensym("pos");
    symZoom         = gensym("zoom");
}



