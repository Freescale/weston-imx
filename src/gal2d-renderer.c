/*
 * Copyright (c) 2014 Freescale Semiconductor, Inc.
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>
#include <assert.h>
#include <pthread.h>

#include "compositor.h"
#include "gal2d-renderer.h"
#include "vertex-clipping.h"
#include "HAL/gc_hal.h"
#include "HAL/gc_hal_raster.h"
#include "HAL/gc_hal_eglplatform.h"

#define galONERROR(x)  if(status < 0) printf("Error in function %s\n", __func__);

struct gal2d_output_state {
	
	int current_buffer;
	pixman_region32_t buffer_damage[2];
	NativeDisplayType display;
    gcoSURF* renderSurf;
	gctUINT32 nNumBuffers;
	int activebuffer;
	gcoSURF offscreenSurface;
	gceSURF_FORMAT format;
    pthread_mutex_t workerMutex;
    pthread_t workerId;
    gctUINT32 exitWorker;
    gctSIGNAL signal;
    gctSIGNAL busySignal;
    gcsHAL_INTERFACE iface;
    int directBlit;
    gctINT width;
    gctINT height;
};

struct gal2d_surface_state {
	float color[4];
	struct weston_buffer_reference buffer_ref;
	int pitch; /* in pixels */
    pixman_region32_t texture_damage;
    gcoSURF gco_Surface;

    struct weston_surface *surface;
    struct wl_listener surface_destroy_listener;
    struct wl_listener renderer_destroy_listener;
};

struct gal2d_renderer {
	struct weston_renderer base;
    struct wl_signal destroy_signal;
    gcoOS gcos;
	gcoHAL gcoHal;
	gco2D gcoEngine2d;
    gctPOINTER  localInfo;
};

static int
gal2d_renderer_create_surface(struct weston_surface *surface);

static inline struct gal2d_surface_state *
get_surface_state(struct weston_surface *surface)
{
	if (!surface->renderer_state)
		gal2d_renderer_create_surface(surface);
	return (struct gal2d_surface_state *)surface->renderer_state;
}

static inline struct gal2d_renderer *
get_renderer(struct weston_compositor *ec)
{
	return (struct gal2d_renderer *)ec->renderer;
}



#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) > (b)) ? (b) : (a))
/*
 * Compute the boundary vertices of the intersection of the global coordinate
 * aligned rectangle 'rect', and an arbitrary quadrilateral produced from
 * 'surf_rect' when transformed from surface coordinates into global coordinates.
 * The vertices are written to 'ex' and 'ey', and the return value is the
 * number of vertices. Vertices are produced in clockwise winding order.
 * Guarantees to produce either zero vertices, or 3-8 vertices with non-zero
 * polygon area.
 */
static int
calculate_edges(struct weston_view *ev, pixman_box32_t *rect,
		pixman_box32_t *surf_rect, float *ex, float *ey)
{

	struct clip_context ctx;
	int i, n;
	float min_x, max_x, min_y, max_y;
	struct polygon8 surf = {
		{ surf_rect->x1, surf_rect->x2, surf_rect->x2, surf_rect->x1 },
		{ surf_rect->y1, surf_rect->y1, surf_rect->y2, surf_rect->y2 },
		4
	};

	ctx.clip.x1 = rect->x1;
	ctx.clip.y1 = rect->y1;
	ctx.clip.x2 = rect->x2;
	ctx.clip.y2 = rect->y2;

	/* transform surface to screen space: */
	for (i = 0; i < surf.n; i++)
		weston_view_to_global_float(ev, surf.x[i], surf.y[i],
					    &surf.x[i], &surf.y[i]);

	/* find bounding box: */
	min_x = max_x = surf.x[0];
	min_y = max_y = surf.y[0];

	for (i = 1; i < surf.n; i++) {
		min_x = min(min_x, surf.x[i]);
		max_x = max(max_x, surf.x[i]);
		min_y = min(min_y, surf.y[i]);
		max_y = max(max_y, surf.y[i]);
	}

	/* First, simple bounding box check to discard early transformed
	 * surface rects that do not intersect with the clip region:
	 */
	if ((min_x >= ctx.clip.x2) || (max_x <= ctx.clip.x1) ||
	    (min_y >= ctx.clip.y2) || (max_y <= ctx.clip.y1))
		return 0;

	/* Simple case, bounding box edges are parallel to surface edges,
	 * there will be only four edges.  We just need to clip the surface
	 * vertices to the clip rect bounds:
	 */
	if (!ev->transform.enabled)
		return clip_simple(&ctx, &surf, ex, ey);

	/* Transformed case: use a general polygon clipping algorithm to
	 * clip the surface rectangle with each side of 'rect'.
	 * The algorithm is Sutherland-Hodgman, as explained in
	 * http://www.codeguru.com/cpp/misc/misc/graphics/article.php/c8965/Polygon-Clipping.htm
	 * but without looking at any of that code.
	 */
	n = clip_transformed(&ctx, &surf, ex, ey);

	if (n < 3)
		return 0;

	return n;
}


static inline struct gal2d_output_state *
get_output_state(struct weston_output *output)
{
	return (struct gal2d_output_state *)output->renderer_state;
}

static gctUINT32
galGetStretchFactor(gctINT32 SrcSize, gctINT32 DestSize)
{
	gctUINT stretchFactor;
	if ( (SrcSize > 0) && (DestSize > 1) )
	{
		stretchFactor = ((SrcSize - 1) << 16) / (DestSize - 1);
	}
	else
	{
		stretchFactor = 0;
	}
	return stretchFactor;
}

static gceSTATUS
galGetStretchFactors(
	IN gcsRECT_PTR SrcRect,
	IN gcsRECT_PTR DestRect,
	OUT gctUINT32 * HorFactor,
	OUT gctUINT32 * VerFactor
	)
{
	if (HorFactor != gcvNULL)
	{
        gctINT32 src, dest;

        /* Compute width of rectangles. */
        gcmVERIFY_OK(gcsRECT_Width(SrcRect, &src));
        gcmVERIFY_OK(gcsRECT_Width(DestRect, &dest));

        /* Compute and return horizontal stretch factor. */
		*HorFactor = galGetStretchFactor(src, dest);
	}

	if (VerFactor != gcvNULL)
	{
		gctINT32 src, dest;

		/* Compute height of rectangles. */
		gcmVERIFY_OK(gcsRECT_Height(SrcRect, &src));
		gcmVERIFY_OK(gcsRECT_Height(DestRect, &dest));

		/* Compute and return vertical stretch factor. */
		*VerFactor = galGetStretchFactor(src, dest);
	}
    /* Success. */
    return gcvSTATUS_OK;
}

static gceSTATUS
gal2d_getSurfaceFormat(halDISPLAY_INFO info, gceSURF_FORMAT * Format)
{
	/* Get the color format. */
    switch (info.greenLength)
    {
    case 4:
        if (info.blueOffset == 0)
        {
            *Format = (info.alphaLength == 0) ? gcvSURF_X4R4G4B4 : gcvSURF_A4R4G4B4;
        }
        else
        {
            *Format = (info.alphaLength == 0) ? gcvSURF_X4B4G4R4 : gcvSURF_A4B4G4R4;
        }
        break;

    case 5:
        if (info.blueOffset == 0)
        {
            *Format = (info.alphaLength == 0) ? gcvSURF_X1R5G5B5 : gcvSURF_A1R5G5B5;
        }
        else
        {
            *Format = (info.alphaLength == 0) ? gcvSURF_X1B5G5R5 : gcvSURF_A1B5G5R5;
        }
        break;

    case 6:
        *Format = gcvSURF_R5G6B5;
        break;

    case 8:
        if (info.blueOffset == 0)
        {
            *Format = (info.alphaLength == 0) ? gcvSURF_X8R8G8B8 : gcvSURF_A8R8G8B8;
        }
        else
        {
            *Format = (info.alphaLength == 0) ? gcvSURF_X8B8G8R8 : gcvSURF_A8B8G8R8;
        }
        break;

    default:
        /* Unsupported color depth. */
        return gcvSTATUS_INVALID_ARGUMENT;
    }
	/* Success. */
    return gcvSTATUS_OK;
}

static gceSTATUS galIsYUVFormat(IN gceSURF_FORMAT Format)
{
    switch (Format)
    {
    case gcvSURF_YUY2:
    case gcvSURF_UYVY:
    case gcvSURF_I420:
    case gcvSURF_YV12:
    case gcvSURF_NV16:
    case gcvSURF_NV12:
    case gcvSURF_NV61:
    case gcvSURF_NV21:

        return gcvSTATUS_TRUE;

    default:
        return gcvSTATUS_FALSE;
    }
}

static gceSTATUS galQueryUVStride(
    IN gceSURF_FORMAT Format,
    IN gctUINT32 yStride,
    OUT gctUINT32_PTR uStride,
    OUT gctUINT32_PTR vStride
    )
{
    switch (Format)
    {
    case gcvSURF_YUY2:
    case gcvSURF_UYVY:
        *uStride = *vStride = 0;
        break;

    case gcvSURF_I420:
    case gcvSURF_YV12:
        *uStride = *vStride = yStride / 2;
        break;

    case gcvSURF_NV16:
    case gcvSURF_NV12:
    case gcvSURF_NV61:
    case gcvSURF_NV21:

        *uStride = yStride;
        *vStride = 0;
        break;

    default:
        return gcvSTATUS_NOT_SUPPORTED;
    }

    return gcvSTATUS_OK;
}

static int
make_current(struct gal2d_renderer *gr, gcoSURF surface)
{    
	gceSTATUS status = gcvSTATUS_OK;
	gctUINT width = 0;
	gctUINT height = 0;
	gctINT stride = 0;
	gctUINT32 physical;
	gctPOINTER va =0;

	if(!surface)
		goto OnError;
    

	gcmONERROR(gcoSURF_GetAlignedSize(surface, &width, &height, &stride));
    
	gcmONERROR(gcoSURF_Lock(surface, &physical, (gctPOINTER *)&va));

	gcmONERROR(gco2D_SetTargetEx(gr->gcoEngine2d, physical, stride,
									gcvSURF_0_DEGREE, width, height));
                                   
	gcmONERROR(gcoSURF_Unlock(surface, (gctPOINTER *)&va));
    
OnError:
    galONERROR(status);
	return status;
}

static gceSTATUS
gal2d_clear(struct weston_output *base)
{
    struct gal2d_renderer *gr = get_renderer(base->compositor);
	struct gal2d_output_state *go = get_output_state(base);    
	gceSTATUS status = gcvSTATUS_OK;
	
	gctINT stride = 0;
	gctUINT width = 0, height = 0;
	gcsRECT dstRect = {0};
	gcmONERROR(gcoSURF_GetAlignedSize(go->renderSurf[go->activebuffer],
					&width, &height, &stride));
	dstRect.right = width;
	dstRect.bottom = height;
	gcmONERROR(gco2D_SetSource(gr->gcoEngine2d, &dstRect));
	gcmONERROR(gco2D_SetClipping(gr->gcoEngine2d, &dstRect));
	gcmONERROR(gco2D_Clear(gr->gcoEngine2d, 1, &dstRect, 0xff0000ff, 0xCC, 0xCC, go->format));
    gcmONERROR(gcoHAL_Commit(gr->gcoHal, gcvTRUE));

OnError:
	galONERROR(status);
    
	return status;
}

static gcoSURF getSurfaceFromShm(struct weston_surface *es, struct weston_buffer *buffer)
{	
    struct gal2d_renderer *gr = get_renderer(es->compositor);
	
	gcoSURF surface = 0;
	gceSURF_FORMAT format;
	gcePOOL pool = gcvPOOL_DEFAULT;

	if (wl_shm_buffer_get_format(buffer->shm_buffer) == WL_SHM_FORMAT_XRGB8888)
		format = gcvSURF_X8R8G8B8;
	else
		format = gcvSURF_A8R8G8B8;

	if(buffer->width == ((buffer->width + 0x7) & ~0x7))
	{
		pool = gcvPOOL_USER;
	}

	gcmVERIFY_OK(gcoSURF_Construct(gr->gcoHal,
						  (gctUINT) buffer->width,
						  (gctUINT) buffer->height,
						  1, gcvSURF_BITMAP,
						  format, pool, &surface));

	if(pool == gcvPOOL_USER)
	{
		gcmVERIFY_OK(gcoSURF_MapUserSurface(surface, 1,
					(gctPOINTER)wl_shm_buffer_get_data(buffer->shm_buffer), gcvINVALID_ADDRESS));
	}

	return surface;
}

static int
gal2dBindBuffer(struct weston_surface* es)
{
    struct gal2d_surface_state *gs = get_surface_state(es);
	gceSTATUS status = gcvSTATUS_OK;
	gcoSURF surface = gs->gco_Surface;	
    struct weston_buffer *buffer = gs->buffer_ref.buffer;
	gcePOOL pool = gcvPOOL_DEFAULT;
    
	gcmVERIFY_OK(gcoSURF_QueryVidMemNode(surface, gcvNULL,
						&pool, gcvNULL));

	if(pool != gcvPOOL_USER)
	{
		gctUINT alignedWidth;
		gctPOINTER logical = (gctPOINTER)wl_shm_buffer_get_data(buffer->shm_buffer);
		gctPOINTER va =0;


		gcmVERIFY_OK(gcoSURF_GetAlignedSize(surface, &alignedWidth, gcvNULL, gcvNULL));
		gcmVERIFY_OK(gcoSURF_Lock(surface, gcvNULL, (gctPOINTER *)&va));
      
		if(alignedWidth == (unsigned int)buffer->width)
		{
			int size = wl_shm_buffer_get_stride(buffer->shm_buffer)*buffer->height;
			memcpy(va, logical, size);
		}
		else
		{
			int i, j;
			for (i = 0; i < buffer->height; i++)
			{
				for (j = 0; j < buffer->width; j++)
				{
					gctUINT dstOff = i * alignedWidth + j;
					gctUINT srcOff = (i * buffer->width + j);

					memcpy(va + dstOff * 4, logical + srcOff * 4, 4);
				}
			}
		}
		gcmVERIFY_OK(gcoSURF_Unlock(surface, (gctPOINTER)va));
	}

	return status;
}

static void
gal2d_flip_surface(struct weston_output *output)
{
	struct gal2d_output_state *go = get_output_state(output);

	if(go->nNumBuffers > 1)
	{
		gctUINT Offset;
		gctINT X;
		gctINT Y;
        
		gcmVERIFY_OK(gcoOS_GetDisplayBackbuffer(go->display, gcvNULL,
									gcvNULL, gcvNULL, &Offset, &X, &Y));

		gcmVERIFY_OK(gcoOS_SetDisplayVirtual(go->display, gcvNULL,
									Offset, X, Y));		
	}
}
static void *gal2d_output_worker(void *arg) 
{  
    struct weston_output *output = (struct weston_output *)arg;
    struct gal2d_output_state *go = get_output_state(output);
    
    while(1)
    {
        if(gcoOS_WaitSignal(gcvNULL, go->signal, gcvINFINITE) == gcvSTATUS_OK )
        {
            gal2d_flip_surface(output);
            gcoOS_Signal(gcvNULL,go->busySignal, gcvTRUE);
        }
        pthread_mutex_lock(&go->workerMutex);
        if(go->exitWorker == 1)
        {
            pthread_mutex_unlock(&go->workerMutex);
            break;
        }
        pthread_mutex_unlock(&go->workerMutex);
    }    
    return 0;
}

static int
update_surface(struct weston_output *output)
{
    struct gal2d_renderer *gr = get_renderer(output->compositor);
	struct gal2d_output_state *go = get_output_state(output);
    gceSTATUS status = gcvSTATUS_OK;

    if(go->nNumBuffers == 1)
	{
        if(!go->directBlit && go->offscreenSurface)
        {        
            make_current(gr, go->renderSurf[go->activebuffer]);

            gctUINT srcWidth = 0;
            gctUINT srcHeight = 0;
            gctINT srcStride = 0;
            gceSURF_FORMAT srcFormat;;
            gcsRECT dstRect = {0};
            gcoSURF srcSurface = go->offscreenSurface;
            gctUINT32 physical;
            gctPOINTER va =0;

            gcmONERROR(gcoSURF_GetAlignedSize(srcSurface, &srcWidth, &srcHeight, &srcStride));
            gcmONERROR(gcoSURF_GetFormat(srcSurface, gcvNULL, &srcFormat));
            gcmONERROR(gcoSURF_Lock(srcSurface, &physical, (gctPOINTER *)&va));
            gcmONERROR(gco2D_SetColorSource(gr->gcoEngine2d, physical, srcStride, srcFormat,
                                gcvFALSE, srcWidth, gcvFALSE, gcvSURF_OPAQUE, 0));

            dstRect.left 	= 0;
            dstRect.top		= 0;
            dstRect.right 	= srcWidth;
            dstRect.bottom 	= srcHeight;

            gcmONERROR(gco2D_SetSource(gr->gcoEngine2d, &dstRect));
            gcmONERROR(gco2D_SetClipping(gr->gcoEngine2d, &dstRect));
            gcmONERROR(gco2D_Blit(gr->gcoEngine2d, 1, &dstRect, 0xCC, 0xCC, go->format));
            gcmONERROR(gcoSURF_Unlock(srcSurface, (gctPOINTER *)&va));
        }
		gcmONERROR(gcoHAL_Commit(gr->gcoHal, gcvFALSE));		
	}
    else if(go->nNumBuffers > 1)
    {
        gcoHAL_ScheduleEvent(gr->gcoHal, &go->iface);
        gcmVERIFY_OK(gcoHAL_Commit(gr->gcoHal, gcvFALSE));        
    }    
OnError:
	galONERROR(status);
	return status;
 }

static int
is_view_visible(struct weston_view *view)
{
	/* Return false, if surface is guaranteed to be totally obscured. */
	int ret;
	pixman_region32_t unocc;

	pixman_region32_init(&unocc);
	pixman_region32_subtract(&unocc, &view->transform.boundingbox,
				 &view->clip);
	ret = pixman_region32_not_empty(&unocc);
	pixman_region32_fini(&unocc);

	return ret;
}
 
static int
use_output(struct weston_output *output)
{
    struct weston_compositor *compositor = output->compositor;
	struct weston_view *view;
    struct gal2d_output_state *go = get_output_state(output);	
	struct gal2d_renderer *gr = get_renderer(output->compositor);    
    gceSTATUS status = gcvSTATUS_OK;

    gcoSURF surface;
    int visibleViews=0;
    int fullscreenViews=0;
    
    surface = go->renderSurf[go->activebuffer];
    if(go->nNumBuffers == 1)
    {
        wl_list_for_each_reverse(view, &compositor->view_list, link)
    		if (view->plane == &compositor->primary_plane && is_view_visible(view))
            {   
                visibleViews++;
                if(view->surface->width == go->width && view->surface->height == go->height)
                {
                    pixman_box32_t *bb_rects;
                    int nbb=0;
                    bb_rects = pixman_region32_rectangles(&view->transform.boundingbox, &nbb);
                    if(nbb == 1)
                        if(bb_rects[0].x1 == 0 && bb_rects[0].y1 ==0)
                            fullscreenViews++;
                }
            }
    
        go->directBlit = ((visibleViews == 1) || (fullscreenViews > 1));

        if(!go->directBlit)
        {
             surface = go->offscreenSurface;
        }
    }
    make_current(gr, surface); 
    return status;
}

static int
gal2d_renderer_read_pixels(struct weston_output *output,
			       pixman_format_code_t format, void *pixels,
			       uint32_t x, uint32_t y,
			       uint32_t width, uint32_t height)
{
	return 0;
}

static int gal2d_int_from_double(double d)
{
	return wl_fixed_to_int(wl_fixed_from_double(d));
}

static void
repaint_region(struct weston_view *ev, struct weston_output *output, struct gal2d_output_state *go, pixman_region32_t *region,
		pixman_region32_t *surf_region){

    struct gal2d_renderer *gr = get_renderer(ev->surface->compositor);
    struct gal2d_surface_state *gs = get_surface_state(ev->surface);

	pixman_box32_t *rects, *surf_rects, *bb_rects;
	int i, j, nrects, nsurf, nbb=0;
	gceSTATUS status = gcvSTATUS_OK;
	gcoSURF srcSurface = gs->gco_Surface;
	gcsRECT srcRect = {0};
	gcsRECT dstrect = {0};
	gctUINT32 horFactor, verFactor;
	int useStretch =1;
	int useFilterBlit = 0;
	gctUINT srcWidth = 0;
	gctUINT srcHeight = 0;
	gctUINT32 srcStride[3];
	gceSURF_FORMAT srcFormat;;
	gctUINT32 srcPhyAddr[3];
	gctUINT32 dstPhyAddr[3];
	gctUINT dstWidth = 0;
	gctUINT dstHeight = 0;
	gctUINT32 dstStrides[3];
	gcoSURF dstsurface;
	int geoWidth = ev->surface->width;
	int geoheight = ev->surface->height;

	bb_rects = pixman_region32_rectangles(&ev->transform.boundingbox, &nbb);

	if(!srcSurface || nbb <= 0)
		goto OnError;
	rects = pixman_region32_rectangles(region, &nrects);
	surf_rects = pixman_region32_rectangles(surf_region, &nsurf);

	gcmVERIFY_OK(gcoSURF_GetAlignedSize(srcSurface, &srcWidth, &srcHeight, (gctINT *)&srcStride[0]));

	gcmVERIFY_OK(gcoSURF_GetFormat(srcSurface, gcvNULL, &srcFormat));

	if(galIsYUVFormat(srcFormat) == gcvSTATUS_TRUE)
	{
		useFilterBlit = 1;
	}

	gcmVERIFY_OK(gcoSURF_Lock(srcSurface, &srcPhyAddr[0], gcvNULL));

	gcmVERIFY_OK(gcoSURF_Unlock(srcSurface, gcvNULL));

	srcRect.left = ev->geometry.x < 0.0 ? gal2d_int_from_double(fabsf(ev->geometry.x)) : 0;
	srcRect.top = 0; /*es->geometry.y < 0.0 ? gal2d_int_from_double(fabsf(es->geometry.y)) : 0;*/
	srcRect.right = ev->surface->width;
	srcRect.bottom = ev->surface->height;

	if(useFilterBlit)
	{
		dstsurface = go->nNumBuffers > 1 ?
						go->renderSurf[go->activebuffer] :
						go->offscreenSurface;
		gcmVERIFY_OK(gcoSURF_GetAlignedSize(dstsurface, &dstWidth, &dstHeight, (gctINT *)&dstStrides));
		gcmVERIFY_OK(gcoSURF_Lock(dstsurface, &dstPhyAddr[0], gcvNULL));
		gcmVERIFY_OK(gcoSURF_Unlock(dstsurface, gcvNULL));
	}
	else
	{
		gcmVERIFY_OK(gco2D_SetColorSourceEx(gr->gcoEngine2d, srcPhyAddr[0], srcStride[0], srcFormat,
						gcvFALSE, srcWidth, srcHeight, gcvFALSE, gcvSURF_OPAQUE, 0));
		gcmVERIFY_OK(gco2D_SetSource(gr->gcoEngine2d, &srcRect));
	}

	for (i = 0; i < nrects; i++)
	{
		pixman_box32_t *rect = &rects[i];
		gctFLOAT min_x, max_x, min_y, max_y;

		dstrect.left = (bb_rects[0].x1 < 0) ? rect->x1 : bb_rects[0].x1;
		dstrect.top = (bb_rects[0].y1 < 0) ? rect->y1 : bb_rects[0].y1;
		dstrect.right = bb_rects[0].x2;
		dstrect.bottom = bb_rects[0].y2;

		if(dstrect.right < 0 || dstrect.bottom < 0)
		{
			break;
		}

		for (j = 0; j < nsurf; j++)
		{
			pixman_box32_t *surf_rect = &surf_rects[j];
			gctFLOAT ex[8], ey[8];          /* edge points in screen space */
			int n;
			gcsRECT clipRect = {0};
			int m=0;
			n = calculate_edges(ev, rect, surf_rect, ex, ey);
			if (n < 3)
				continue;

			min_x = max_x = ex[0];
			min_y = max_y = ey[0];
			for (m = 1; m < n; m++)
			{
				min_x = min(min_x, ex[m]);
				max_x = max(max_x, ex[m]);
				min_y = min(min_y, ey[m]);
				max_y = max(max_y, ey[m]);
			}

			clipRect.left = gal2d_int_from_double(min_x);
			clipRect.top = gal2d_int_from_double(min_y);
			clipRect.right = gal2d_int_from_double(max_x);
			clipRect.bottom = gal2d_int_from_double(max_y);

			if(output->x > 0)
			{
				dstrect.left = dstrect.left - output->x;
				dstrect.right = dstrect.right - output->x;
				clipRect.left = clipRect.left - output->x;
				clipRect.right = clipRect.right - output->x;
			}

			dstrect.left = (dstrect.left < 0) ? 0 : dstrect.left;
			
			status = gco2D_SetClipping(gr->gcoEngine2d, &clipRect);
			if(status < 0)
			{
				weston_log("Error in gco2D_SetClipping %s\n", __func__);
				goto OnError;
			}

			if(useFilterBlit)
			{
				gctINT          srcStrideNum;
				gctINT          srcAddressNum;
				gcmVERIFY_OK(galQueryUVStride(srcFormat, srcStride[0],
						&srcStride[1], &srcStride[2]));

				switch (srcFormat)
				{
				case gcvSURF_YUY2:
				case gcvSURF_UYVY:
					srcStrideNum = srcAddressNum = 1;
					break;

				case gcvSURF_I420:
				case gcvSURF_YV12:
					srcStrideNum = srcAddressNum = 3;
					break;

				case gcvSURF_NV16:
				case gcvSURF_NV12:
				case gcvSURF_NV61:
				case gcvSURF_NV21:
					srcStrideNum = srcAddressNum = 2;
					break;

				default:
					gcmONERROR(gcvSTATUS_NOT_SUPPORTED);
				}
				gco2D_FilterBlitEx2(gr->gcoEngine2d,
					srcPhyAddr, srcAddressNum,
					srcStride, srcStrideNum,
					gcvLINEAR, srcFormat, gcvSURF_0_DEGREE,
					geoWidth, geoheight, &srcRect,
					dstPhyAddr, 1,
					dstStrides, 1,
					gcvLINEAR, go->format, gcvSURF_0_DEGREE,
					dstWidth, dstHeight,
					&dstrect, gcvNULL);
			}
			else
			{
				if(useStretch)
					gcmVERIFY_OK(galGetStretchFactors(&srcRect, &dstrect, &horFactor, &verFactor));

				if(verFactor == 65536 && horFactor == 65536)
				{
					gcmVERIFY_OK(gco2D_Blit(gr->gcoEngine2d, 1, &dstrect,
											0xCC, 0xCC, go->format));
				}
				else
				{
					/* Program the stretch factors. */
					gcmVERIFY_OK(gco2D_SetStretchFactors(gr->gcoEngine2d, horFactor, verFactor));

					gcmVERIFY_OK(gco2D_StretchBlit(gr->gcoEngine2d, 1, &dstrect,
							0xCC, 0xCC, go->format));
				}
			}

			if(status < 0)
			{
				printf("cr l=%d r=%d t=%d b=%d w=%d h=%d\n",
					clipRect.left, clipRect.right, clipRect.top ,clipRect.bottom,
					clipRect.right - clipRect.left, clipRect.bottom -clipRect.top);
				printf("dr l=%d r=%d t=%d b=%d w=%d h=%d\n",
						dstrect.left, dstrect.right, dstrect.top ,dstrect.bottom,
						dstrect.right - dstrect.left, dstrect.bottom -dstrect.top);
				printf("horFactor=%d, verFactor=%d\n",horFactor, verFactor);

				goto OnError;
			}
		}
	}

OnError:
	galONERROR(status);
}

static void
draw_view(struct weston_view *ev, struct weston_output *output,
	     pixman_region32_t *damage) /* in global coordinates */
{
	struct weston_compositor *ec = ev->surface->compositor;	
	struct gal2d_output_state *go = get_output_state(output);
	/* repaint bounding region in global coordinates: */
	pixman_region32_t repaint;
	/* non-opaque region in surface coordinates: */
	pixman_region32_t surface_blend;
	pixman_region32_t *buffer_damage;

    pixman_region32_init(&repaint);
	pixman_region32_intersect(&repaint,
				  &ev->transform.boundingbox, damage);
	pixman_region32_subtract(&repaint, &repaint, &ev->clip);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	buffer_damage = &go->buffer_damage[go->current_buffer];
	pixman_region32_subtract(buffer_damage, buffer_damage, &repaint);

	/* blended region is whole surface minus opaque region: */
	pixman_region32_init_rect(&surface_blend, 0, 0,
				  ev->surface->width, ev->surface->height);
	pixman_region32_subtract(&surface_blend, &surface_blend, &ev->surface->opaque);

    struct gal2d_renderer *gr = get_renderer(ec);
    
	if (pixman_region32_not_empty(&ev->surface->opaque)) {

		repaint_region(ev, output, go, &repaint, &ev->surface->opaque);
	}

	if (pixman_region32_not_empty(&surface_blend)) {
    
        gco2D_EnableAlphaBlend(gr->gcoEngine2d,
            ev->alpha * 0xFF, ev->alpha * 0xFF,
            gcvSURF_PIXEL_ALPHA_STRAIGHT, gcvSURF_PIXEL_ALPHA_STRAIGHT,
            gcvSURF_GLOBAL_ALPHA_SCALE, gcvSURF_GLOBAL_ALPHA_SCALE,
            gcvSURF_BLEND_STRAIGHT, gcvSURF_BLEND_INVERSED,
            gcvSURF_COLOR_STRAIGHT, gcvSURF_COLOR_STRAIGHT);
            
		repaint_region(ev, output, go, &repaint, &surface_blend);
	}

    gco2D_DisableAlphaBlend(gr->gcoEngine2d);
	pixman_region32_fini(&surface_blend);

out:
	pixman_region32_fini(&repaint);

}

static void
repaint_views(struct weston_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *compositor = output->compositor;
	struct weston_view *view;
	struct gal2d_output_state *go = get_output_state(output);
 	
    if(go->nNumBuffers > 1)
    {
        /*500ms is more than enough to process a frame */
        gcoOS_WaitSignal(gcvNULL, go->busySignal, 500);
    }
    go->activebuffer = (go->activebuffer+1) % go->nNumBuffers;
    
	wl_list_for_each_reverse(view, &compositor->view_list, link)
		if (view->plane == &compositor->primary_plane)
			draw_view(view, output, damage);
}

static void
gal2d_renderer_repaint_output(struct weston_output *output,
			     pixman_region32_t *output_damage)
{
    struct gal2d_output_state *go = get_output_state(output);	
 	gctUINT32 i;

	if (use_output(output) < 0)
		return;
        
	for (i = 0; i < 2; i++)
		pixman_region32_union(&go->buffer_damage[i],
				      &go->buffer_damage[i],
				      output_damage);

	pixman_region32_union(output_damage, output_damage,
			      &go->buffer_damage[go->current_buffer]);

	repaint_views(output, output_damage);

	pixman_region32_copy(&output->previous_damage, output_damage);
	wl_signal_emit(&output->frame_signal, output);
    
    update_surface(output);

	go->current_buffer ^= 1;
}

static void
gal2d_renderer_attach_egl(struct weston_surface *es, struct weston_buffer *buffer)
{
    gcsWL_VIV_BUFFER *vivBuffer = wl_resource_get_user_data(buffer->resource);
    gctUINT width = 0;
    gctUINT height = 0;
    gctINT stride = 0;
    gceSURF_FORMAT format;
    gcoSURF srcSurf = vivBuffer->surface;
    gctUINT32 physical;
    gctPOINTER va =0;
    gceSTATUS status = gcvSTATUS_OK;
    struct gal2d_surface_state *gs = get_surface_state(es);
        
    if(gs->gco_Surface == gcvNULL)
    {
        /** Construct a wrapper. */
        gcmONERROR(gcoSURF_ConstructWrapper(gcvNULL, &gs->gco_Surface));
    }

    gcmONERROR(gcoSURF_GetAlignedSize(srcSurf, &width, &height, &stride));
    gcmONERROR(gcoSURF_GetFormat(srcSurf, gcvNULL, &format));
    gcmONERROR(gcoSURF_Lock(srcSurf, &physical, (gctPOINTER *)&va));  

    /* Set the buffer. */
    gcmONERROR(gcoSURF_SetBuffer(gs->gco_Surface,
                               gcvSURF_BITMAP_NO_VIDMEM,
                               format,
                               stride,
                               (gctPOINTER) va,
                               (gctUINT32) physical));

    /* Set the window. */
    gcmONERROR(gcoSURF_SetWindow(gs->gco_Surface, 0, 0, width, height));
    
    buffer->width = vivBuffer->width;
    buffer->height = vivBuffer->height;
    
  OnError:
    galONERROR(status);
}

static void
gal2d_renderer_flush_damage(struct weston_surface *surface)
{
	struct gal2d_surface_state *gs = get_surface_state(surface);
	struct weston_buffer *buffer = gs->buffer_ref.buffer;
    struct weston_view *view;
	int texture_used;
	pixman_region32_union(&gs->texture_damage,
			      &gs->texture_damage, &surface->damage);

	if (!buffer)
		return;

	texture_used = 0;
	wl_list_for_each(view, &surface->views, surface_link) {
		if (view->plane == &surface->compositor->primary_plane) {
			texture_used = 1;
			break;
		}
	}
	if (!texture_used)
		return;

	if (!pixman_region32_not_empty(&gs->texture_damage))
		goto done;

    if(wl_shm_buffer_get(buffer->resource))
	{
		if(gs->gco_Surface==NULL)
		{
			gs->gco_Surface = getSurfaceFromShm(surface, buffer);
		}
		gal2dBindBuffer(surface);
	}
	else
        gal2d_renderer_attach_egl(surface, buffer);

done:
	pixman_region32_fini(&gs->texture_damage);
	pixman_region32_init(&gs->texture_damage);

	weston_buffer_reference(&gs->buffer_ref, NULL);
}

static void
gal2d_renderer_attach(struct weston_surface *es, struct weston_buffer *buffer)
{
	struct gal2d_surface_state *gs = get_surface_state(es);
	struct wl_shm_buffer *shm_buffer;
	weston_buffer_reference(&gs->buffer_ref, buffer);

	if(buffer==NULL)
		return;

	shm_buffer = wl_shm_buffer_get(buffer->resource);

	if(shm_buffer)
	{
		buffer->width = wl_shm_buffer_get_width(shm_buffer);
		buffer->height = wl_shm_buffer_get_height(shm_buffer);
		buffer->shm_buffer = shm_buffer;

		if(gs->gco_Surface)
		{
			gcoSURF_Destroy(gs->gco_Surface);
            gs->gco_Surface = getSurfaceFromShm(es, buffer);
		}
	}
	else
		gal2d_renderer_attach_egl(es, buffer);
}

static void
surface_state_destroy(struct gal2d_surface_state *gs, struct gal2d_renderer *gr)
{
	if(gs->gco_Surface)
    {
        gcoSURF_Destroy(gs->gco_Surface);
    }
    wl_list_remove(&gs->surface_destroy_listener.link);
	wl_list_remove(&gs->renderer_destroy_listener.link);
	if(gs->surface)
		gs->surface->renderer_state = NULL;

	weston_buffer_reference(&gs->buffer_ref, NULL);
	free(gs);
}

static void
surface_state_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct gal2d_surface_state *gs;
	struct gal2d_renderer *gr;

	gs = container_of(listener, struct gal2d_surface_state,
			  surface_destroy_listener);

	gr = get_renderer(gs->surface->compositor);
	surface_state_destroy(gs, gr);
}

static void
surface_state_handle_renderer_destroy(struct wl_listener *listener, void *data)
{
	struct gal2d_surface_state *gs;
	struct gal2d_renderer *gr;

	gr = data;

	gs = container_of(listener, struct gal2d_surface_state,
			  renderer_destroy_listener);

	surface_state_destroy(gs, gr);
}


static int
gal2d_renderer_create_surface(struct weston_surface *surface)
{
    struct gal2d_surface_state *gs;
    struct gal2d_renderer *gr = get_renderer(surface->compositor);
    
	gs = calloc(1, sizeof *gs);
	if (!gs)
		return -1;

	/* A buffer is never attached to solid color surfaces, yet
	 * they still go through texcoord computations. Do not divide
	 * by zero there.
	 */
	gs->pitch = 1;

    gs->surface = surface;
    
	pixman_region32_init(&gs->texture_damage);
	surface->renderer_state = gs;

	gs->surface_destroy_listener.notify =
		surface_state_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &gs->surface_destroy_listener);

	gs->renderer_destroy_listener.notify =
		surface_state_handle_renderer_destroy;
	wl_signal_add(&gr->destroy_signal,
		      &gs->renderer_destroy_listener);

	if (surface->buffer_ref.buffer) {
		gal2d_renderer_attach(surface, surface->buffer_ref.buffer);
		gal2d_renderer_flush_damage(surface);
	}
    
    return 0;
}

static void
gal2d_renderer_surface_set_color(struct weston_surface *surface,
		 float red, float green, float blue, float alpha)
{
    struct gal2d_surface_state *gs = get_surface_state(surface);

	gs->color[0] = red;
	gs->color[1] = green;
	gs->color[2] = blue;
	gs->color[3] = alpha;
}


static void
gal2d_renderer_output_destroy(struct weston_output *output)
{
    struct gal2d_output_state *go = get_output_state(output);
    gctUINT32 i;

    for (i = 0; i < 2; i++)
    {
        pixman_region32_fini(&go->buffer_damage[i]);
    }
    if(go->nNumBuffers <= 1 )
	{
		if(go->offscreenSurface)
			gcmVERIFY_OK(gcoSURF_Destroy(go->offscreenSurface));
	}
    else
    {
        gcoOS_Signal(gcvNULL,go->signal, gcvTRUE);
        pthread_mutex_lock(&go->workerMutex);
        go->exitWorker = 1;
        pthread_mutex_unlock(&go->workerMutex);
        pthread_join(go->workerId, NULL);
    }
    
	for(i=0; i < go->nNumBuffers; i++)
	{
		gcmVERIFY_OK(gcoSURF_Destroy(go->renderSurf[i]));
	}
	free(go->renderSurf);
	go->renderSurf = gcvNULL;

	free(go);
}

static void
gal2d_renderer_destroy(struct weston_compositor *ec)
{
    struct gal2d_renderer *gr = get_renderer(ec);

    wl_signal_emit(&gr->destroy_signal, gr);
	free(ec->renderer);
	ec->renderer = NULL;
}


static int
gal2d_renderer_create(struct weston_compositor *ec)
{
    struct gal2d_renderer *gr;
    gceSTATUS status = gcvSTATUS_OK;
	gr = malloc(sizeof *gr);
	if (gr == NULL)
		return -1;

	gr->base.read_pixels = gal2d_renderer_read_pixels;
	gr->base.repaint_output = gal2d_renderer_repaint_output;
	gr->base.flush_damage = gal2d_renderer_flush_damage;
	gr->base.attach = gal2d_renderer_attach;
	gr->base.surface_set_color = gal2d_renderer_surface_set_color;
	gr->base.destroy = gal2d_renderer_destroy;
    
    /* Construct the gcoOS object. */
	gcmONERROR(gcoOS_Construct(gcvNULL, &gr->gcos));

	/* Construct the gcoHAL object. */
	gcmONERROR(gcoHAL_Construct(gcvNULL, gr->gcos, &gr->gcoHal));
	gcmONERROR(gcoHAL_Get2DEngine(gr->gcoHal, &gr->gcoEngine2d));
	gcmONERROR(gcoHAL_SetHardwareType(gr->gcoHal, gcvHARDWARE_2D));
    
	ec->renderer = &gr->base; 
        wl_signal_init(&gr->destroy_signal);
OnError:
    galONERROR(status);
    
    /* Return the status. */    
    return status;
	
}

static int
gal2d_renderer_output_create(struct weston_output *output, NativeDisplayType display,
				    NativeWindowType window)

 {
    struct gal2d_renderer *gr = get_renderer(output->compositor);
	struct gal2d_output_state *go = calloc(1, sizeof *go);
    halDISPLAY_INFO info;
    gctUINT32 backOffset = 0;    
    gceSTATUS status = gcvSTATUS_OK;
	gctUINT32 i;

    if (!go)
		return -1;

    output->renderer_state = go;
    go->display = display;
    gcmONERROR(gcoOS_InitLocalDisplayInfo(go->display, &gr->localInfo));

    /* Get display information. */
	gcmONERROR(gcoOS_GetDisplayInfoEx2(
					go->display, gcvNULL, gr->localInfo,
					sizeof(info), &info));
	go->nNumBuffers = info.multiBuffer;

    weston_log("Number of buffers=%d\n",go->nNumBuffers);

	gcmONERROR(gal2d_getSurfaceFormat(info, &go->format));    
	backOffset = (gctUINT32)(info.stride * info.height );

	go->activebuffer = 0;

	go->renderSurf = malloc(sizeof(gcoSURF) * go->nNumBuffers);
	gcoOS_GetDisplayVirtual(go->display, &go->width, &go->height);
    gcoOS_SetSwapInterval(go->display, 1);
   
    /*Needed only for multi Buffer  */
    if(go->nNumBuffers > 1)
    {
        gcmVERIFY_OK(gcoOS_CreateSignal(gcvNULL, gcvFALSE,
                &go->signal));
        gcmVERIFY_OK(gcoOS_CreateSignal(gcvNULL, gcvFALSE,
                &go->busySignal));
                
        go->iface.command            = gcvHAL_SIGNAL;
        go->iface.u.Signal.signal    = gcmPTR_TO_UINT64(go->signal);
        go->iface.u.Signal.auxSignal = 0;
        go->iface.u.Signal.process = gcmPTR_TO_UINT64(gcoOS_GetCurrentProcessID());
        go->iface.u.Signal.fromWhere = gcvKERNEL_PIXEL;
       
        go->exitWorker = 0;        
        pthread_create(&go->workerId, NULL, gal2d_output_worker, output);    
        pthread_mutex_init(&go->workerMutex, gcvNULL);
    }
	for(i=0; i < go->nNumBuffers; i++)
	{
        gcmONERROR(gcoSURF_Construct(gr->gcoHal, info.width, info.height, 1, 
            gcvSURF_BITMAP, go->format, gcvPOOL_USER, &go->renderSurf[i]));
        
        gcoSURF_MapUserSurface(go->renderSurf[i], 0,info.logical + (i * backOffset),
						info.physical + (i * backOffset));
		
		//Clear surfaces
		make_current(gr, go->renderSurf[go->activebuffer]);
		gal2d_clear(output);
		gal2d_flip_surface(output);
	}
	if(go->nNumBuffers <= 1)
		go->activebuffer = 0;
	else
		go->activebuffer = 1;
    
	if(go->nNumBuffers <= 1 )
	{
		gcmVERIFY_OK(gcoSURF_Construct(gr->gcoHal,
							  (gctUINT) info.width,
							  (gctUINT) info.height,
							  1,
							  gcvSURF_BITMAP,
							  go->format,
							  gcvPOOL_DEFAULT,
							  &go->offscreenSurface));
		make_current(gr, go->offscreenSurface);
		gal2d_clear(output);
	}
    else
    {
        gcoOS_Signal(gcvNULL,go->busySignal, gcvTRUE);
    }

	for (i = 0; i < 2; i++)
		pixman_region32_init(&go->buffer_damage[i]);
OnError:
    galONERROR(status);
    /* Return the status. */
    return status;  
 }

 WL_EXPORT struct gal2d_renderer_interface gal2d_renderer_interface = {
	.create = gal2d_renderer_create,
	.output_create = gal2d_renderer_output_create,
	.output_destroy = gal2d_renderer_output_destroy,
};
