/*
http://www.youtube.com/watch?v=JVoUgR6bhBc
 */

/*
    This file is part of darktable,
    copyright (c) 2013 tobias ellinghaus.
    copyright (c) 2018 Aurélien Pierre.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/opencl.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

//#include <gtk/gtk.h>
#include <stdlib.h>

// these are not in a state to be useful. but they look nice. too bad i couldn't map the enhanced mode with
// negative values to the wheels :(
//#define SHOW_COLOR_WHEELS

// debug
#define AUTO
#define CONTROLS
#define OPTIM

DT_MODULE_INTROSPECTION(2, dt_iop_colorbalance_params_t)

/*

  Meaning of the values:
   0 --> 100%
  -1 -->   0%
   1 --> 200%
 */

typedef enum dt_iop_colorbalance_mode_t
{
  LIFT_GAMMA_GAIN = 0,
  SLOPE_OFFSET_POWER = 1,
  LEGACY = 2
} dt_iop_colorbalance_mode_t;

typedef enum _colorbalance_channel_t
{
  CHANNEL_FACTOR = 0,
  CHANNEL_RED,
  CHANNEL_GREEN,
  CHANNEL_BLUE,
  CHANNEL_SIZE
} _colorbalance_channel_t;

typedef enum _colorbalance_levels_t
{
  LIFT = 0,
  GAMMA,
  GAIN,
  LEVELS
} _colorbalance_levels_t;

typedef enum _controls_t
{
  HSL,
  RGBL,
  BOTH
} _controls_t;

typedef struct dt_iop_colorbalance_params_t
{
  dt_iop_colorbalance_mode_t mode;
  float lift[CHANNEL_SIZE], gamma[CHANNEL_SIZE], gain[CHANNEL_SIZE];
  float saturation, contrast, grey;
} dt_iop_colorbalance_params_t;

typedef struct dt_iop_colorbalance_gui_data_t
{
  dt_pthread_mutex_t lock;
  GtkWidget *master_box;
  GtkWidget *mode;
#ifdef CONTROLS
  GtkWidget *controls;
#endif
  GtkWidget *hue_lift, *hue_gamma, *hue_gain;
  GtkWidget *sat_lift, *sat_gamma, *sat_gain;
  GtkWidget *lift_r, *lift_g, *lift_b, *lift_factor;
  GtkWidget *gamma_r, *gamma_g, *gamma_b, *gamma_factor;
  GtkWidget *gain_r, *gain_g, *gain_b, *gain_factor;
  GtkWidget *saturation, *contrast, *grey;
#ifdef AUTO
  GtkWidget *masterbox;
  GtkWidget *optim_label;
  GtkWidget *auto_luma;
  GtkWidget *auto_color;
#endif
#ifdef AUTO
  float color_patches_lift[3];
  float color_patches_gamma[3];
  float color_patches_gain[3];
  int color_patches_flags[LEVELS];
  float luma_patches[LEVELS];
  int luma_patches_flags[LEVELS];
#endif
} dt_iop_colorbalance_gui_data_t;

typedef struct dt_iop_colorbalance_data_t
{
  dt_iop_colorbalance_mode_t mode;
  float lift[CHANNEL_SIZE], gamma[CHANNEL_SIZE], gain[CHANNEL_SIZE];
  float saturation, contrast, grey;
} dt_iop_colorbalance_data_t;

typedef struct dt_iop_colorbalance_global_data_t
{
  int kernel_colorbalance;
  int kernel_colorbalance_cdl;
  int kernel_colorbalance_lgg;
} dt_iop_colorbalance_global_data_t;

const char *name()
{
  return _("color balance");
}

const char *description()
{
  return _("lift/gamma/gain controls as seen in video editors");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_colorbalance_params_v1_t
    {
      float lift[CHANNEL_SIZE], gamma[CHANNEL_SIZE], gain[CHANNEL_SIZE];
    } dt_iop_colorbalance_params_v1_t;

    dt_iop_colorbalance_params_v1_t *o = (dt_iop_colorbalance_params_v1_t *)old_params;
    dt_iop_colorbalance_params_t *n = (dt_iop_colorbalance_params_t *)new_params;
    dt_iop_colorbalance_params_t *d = (dt_iop_colorbalance_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    *n->lift = *o->lift;
    *n->gamma = *o->gamma;
    *n->gain = *o->gain;
    n->mode = LEGACY;
    return 0;
  }
  return 1;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "mode"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "controls"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "mode", GTK_WIDGET(g->mode));
  dt_accel_connect_slider_iop(self, "controls", GTK_WIDGET(g->controls));
}

static inline float CDL(float x, float slope, float offset, float power)
{
  float out;
  out = slope * x + offset;
  out = (out <= 0.0f) ? 0.0f : powf(out, power);
  return out;
}

// see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)piece->data;
  const int ch = piece->colors;

 // these are RGB values!
    const float gamma[3] = { d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                             d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                             d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR] },
                gamma_inv[3] = { (gamma[0] != 0.0) ? 1.0 / gamma[0] : 1000000.0,
                                 (gamma[1] != 0.0) ? 1.0 / gamma[1] : 1000000.0,
                                 (gamma[2] != 0.0) ? 1.0 / gamma[2] : 1000000.0 },
                gain[3] = { d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                            d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                            d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR] },
                contrast = (d->contrast != 0.0f) ? 1.0f / d->contrast : 1000000.0f,
                grey = d->grey / 100.0f;

  // For neutral parameters, skip the computations doing x^1 or (x-a)*1 + a to save time
  const int run_contrast = (d->contrast == 1.0f) ? 0 : 1;
  const int run_saturation = (d->saturation == 1.0f) ? 0: 1;

  switch (d->mode)
  {
    case LEGACY:
    {
       // these are RGB values!
      const float lift[3] = { 2.0 - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                              2.0 - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                              2.0 - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]) };

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
        float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
        for(int i = 0; i < roi_out->width; i++)
        {
          // transform the pixel to sRGB:
          // Lab -> XYZ
          float XYZ[3] = { 0.0f };
          dt_Lab_to_XYZ(in, XYZ);

          // XYZ -> sRGB
          float rgb[3] = { 0.0f };
          dt_XYZ_to_sRGB(XYZ, rgb);

          // do the calculation in RGB space
          for(int c = 0; c < 3; c++)
          {
            // lift gamma gain
            rgb[c] = ((( rgb[c]  - 1.0f) * lift[c]) + 1.0f) * gain[c];
            rgb[c] = (rgb[c] < 0.0f) ? 0.0f : powf(rgb[c], gamma_inv[c]);
          }

          // transform the result back to Lab
          // sRGB -> XYZ
          dt_sRGB_to_XYZ(rgb, XYZ);

          // XYZ -> Lab
          dt_XYZ_to_Lab(XYZ, out);
          out[3] = in[3];

          in += ch;
          out += ch;
        }
      }

      break;
    }
    case LIFT_GAMMA_GAIN:
    {
       // these are RGB values!
      const float lift[3] = { 2.0 - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                              2.0 - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                              2.0 - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]) };

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
        float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
        for(int i = 0; i < roi_out->width; i++)
        {
          // transform the pixel to sRGB:
          // Lab -> XYZ
          float XYZ[3] = { 0.0f };
          dt_Lab_to_XYZ(in, XYZ);

          // XYZ -> sRGB
          float rgb[3] = { 0.0f };
          dt_XYZ_to_prophotorgb(XYZ, rgb);

          const float luma = XYZ[1]; // the Y channel is the relative luminance

          // do the calculation in RGB space
          for(int c = 0; c < 3; c++)
          {
            // main saturation
            if (run_saturation) rgb[c] = luma + d->saturation * (rgb[c] - luma);

            // RGB gamma correction
            rgb[c] = (rgb[c] <= 0.0f) ? 0.0f : powf(rgb[c], 1.0f/2.2f);

            // lift gamma gain
            rgb[c] = ((( rgb[c]  - 1.0f) * lift[c]) + 1.0f) * gain[c];
            rgb[c] = (rgb[c] <= 0.0f) ? 0.0f : powf(rgb[c], gamma_inv[c] * 2.2f);

            // contrast
            if (run_contrast) rgb[c] = (rgb[c] <= 0.0f) ? 0.0f : powf(rgb[c] / grey, contrast) * grey;
          }

          // transform the result back to Lab
          // sRGB -> XYZ
          dt_prophotorgb_to_XYZ(rgb, XYZ);

          // XYZ -> Lab
          dt_XYZ_to_Lab(XYZ, out);
          out[3] = in[3];

          in += ch;
          out += ch;
        }
      }

      break;

   }
    case SLOPE_OFFSET_POWER:
    {
      // these are RGB values!

      const float lift[3] = { ( d->lift[CHANNEL_RED] + d->lift[CHANNEL_FACTOR] - 2.0f),
                              ( d->lift[CHANNEL_GREEN] + d->lift[CHANNEL_FACTOR] - 2.0f),
                              ( d->lift[CHANNEL_BLUE] + d->lift[CHANNEL_FACTOR] - 2.0f)};

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
        float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
        for(int i = 0; i < roi_out->width; i++)
        {
          // transform the pixel to RGB:
          // Lab -> XYZ
          float XYZ[3];
          dt_Lab_to_XYZ(in, XYZ);

          // XYZ -> RGB
          float rgb[3];
          dt_XYZ_to_prophotorgb(XYZ, rgb);

          const float luma = XYZ[1]; // the Y channel is the RGB luminance

          // do the calculation in RGB space
          for(int c = 0; c < 3; c++)
          {
            // main saturation
            if (run_saturation) rgb[c] = luma + d->saturation * (rgb[c] - luma);

            // channel CDL
            rgb[c] = CDL(rgb[c], gain[c], lift[c], gamma_inv[c]);

            // fulcrum contrat
            if (run_contrast) rgb[c] = (rgb[c] <= 0.0f) ? 0.0f : powf(rgb[c] / grey, contrast) * grey;
          }

          // transform the result back to Lab
          // sRGB -> XYZ
          dt_prophotorgb_to_XYZ(rgb , XYZ);

          // XYZ -> Lab
          dt_XYZ_to_Lab(XYZ, out);
          out[3] = in[3];

          in += ch;
          out += ch;
        }
      }
      break;
    }
  }
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)piece->data;
  const int ch = piece->colors;
  const __m128 gamma = _mm_setr_ps(d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                                   d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                                   d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR],
                                   0.0f);
  const __m128 gamma_inv = _mm_setr_ps((gamma[0] != 0.0) ? 1.0 / gamma[0] : 1000000.0,
                                       (gamma[1] != 0.0) ? 1.0 / gamma[1] : 1000000.0,
                                       (gamma[2] != 0.0) ? 1.0 / gamma[2] : 1000000.0,
                                       0.0f);
  const __m128 gain = _mm_setr_ps(d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                                  d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                                  d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR],
                                  0.0f);

  float contrast_inv = (d->contrast != 0.0f) ? 1.0f / d->contrast : 1000000.0f;
  const __m128 contrast = _mm_setr_ps(contrast_inv, contrast_inv, contrast_inv, 0.0f);
  float grey_corr = d->grey / 100.0f;
  const __m128 grey = _mm_setr_ps(grey_corr, grey_corr, grey_corr, 0.0f);
  const __m128 saturation = _mm_setr_ps(d->saturation, d->saturation, d->saturation, 0.0f);
  const __m128 zero = _mm_setzero_ps();
  const __m128 one = _mm_set1_ps(1.0);

  // For neutral parameters, skip the computations doing x^1 or (x-a)*1 + a to save time
  const int run_contrast = (d->contrast == 1.0f) ? 0 : 1;
  const int run_saturation = (d->saturation == 1.0f) ? 0: 1;

  switch (d->mode)
  {
    case LEGACY:
    {
      // these are RGB values!
      const __m128 lift = _mm_setr_ps(2.0 - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                                      2.0 - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                                      2.0 - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]),
                                      0.0f);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
        float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
        for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
        {
          // transform the pixel to sRGB:
          // Lab -> XYZ
          __m128 XYZ = dt_Lab_to_XYZ_sse2(_mm_load_ps(in));
          // XYZ -> sRGB
          __m128 rgb = dt_XYZ_to_sRGB_sse2(XYZ);

          // do the calculation in RGB space
          // regular lift gamma gain
          rgb = ((rgb - one) * lift + one) * gain;
          rgb = _mm_max_ps(rgb, zero);
          rgb = _mm_pow_ps(rgb, gamma_inv);

          // transform the result back to Lab
          // sRGB -> XYZ
          XYZ = dt_sRGB_to_XYZ_sse2(rgb);
          // XYZ -> Lab
          _mm_store_ps(out, dt_XYZ_to_Lab_sse2(XYZ));
        }
      }

      break;
    }

    case LIFT_GAMMA_GAIN:
    {
      // these are RGB values!
      const __m128 lift = _mm_setr_ps(2.0 - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                                      2.0 - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                                      2.0 - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]),
                                      0.0f);

      const __m128 gamma_RGB = _mm_set1_ps(2.2f);
      const __m128 gamma_inv_RGB = _mm_set1_ps(1.0f/2.2f);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
        float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
        for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
        {
          // transform the pixel to sRGB:
          // Lab -> XYZ
          __m128 XYZ = dt_Lab_to_XYZ_sse2(_mm_load_ps(in));
          // XYZ -> sRGB
          __m128 rgb = dt_XYZ_to_prophotoRGB_sse2(XYZ);

          // do the calculation in RGB space

          // adjust main saturation
          if (run_saturation)
          {
            __m128 luma = _mm_set1_ps(XYZ[1]); // the Y channel is the relative luminance
            rgb = luma + saturation * (rgb - luma);
          }

          // RGB gamma adjustement
          rgb = _mm_pow_ps(_mm_max_ps(rgb, zero), gamma_inv_RGB);

          // regular lift gamma gain
          rgb = ((rgb - one) * lift + one) * gain;
          rgb = _mm_max_ps(rgb, zero);
          rgb = _mm_pow_ps(rgb, gamma_inv * gamma_RGB);

          // fulcrum contrast
          if (run_contrast)
          {
            rgb = _mm_max_ps(rgb, zero);
            rgb = _mm_pow_ps(rgb / grey, contrast) * grey;
          }

          // transform the result back to Lab
          // sRGB -> XYZ
          XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
          // XYZ -> Lab
          _mm_store_ps(out, dt_XYZ_to_Lab_sse2(XYZ));
        }
      }

      break;
    }

    case SLOPE_OFFSET_POWER:
    {
      // these are RGB values!
      const __m128 lift = _mm_setr_ps((d->lift[CHANNEL_RED] + d->lift[CHANNEL_FACTOR] - 2.f),
                                      (d->lift[CHANNEL_GREEN] + d->lift[CHANNEL_FACTOR] - 2.f),
                                      (d->lift[CHANNEL_BLUE] + d->lift[CHANNEL_FACTOR] - 2.f),
                                      0.0f);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
        float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
        for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
        {
          // transform the pixel to sRGB:
          // Lab -> XYZ
          __m128 XYZ = dt_Lab_to_XYZ_sse2(_mm_load_ps(in));
          // XYZ -> sRGB
          __m128 rgb = dt_XYZ_to_prophotoRGB_sse2(XYZ);

          // adjust main saturation
          if (run_saturation)
          {
            __m128 luma = _mm_set1_ps(XYZ[1]); // the Y channel is the relative luminance
            rgb = luma + saturation * (rgb - luma);
          }

          // slope offset
          rgb = rgb * gain + lift;

          //power
          rgb = _mm_max_ps(rgb, zero);
          rgb = _mm_pow_ps(rgb, gamma_inv);

          // fulcrum contrast
          if (run_contrast)
          {
            rgb = _mm_max_ps(rgb, zero);
            rgb = _mm_pow_ps(rgb / grey, contrast) * grey;
          }

          // transform the result back to Lab
          // sRGB -> XYZ
          XYZ = dt_prophotoRGB_to_XYZ_sse2(rgb);
          // XYZ -> Lab
          _mm_store_ps(out, dt_XYZ_to_Lab_sse2(XYZ));
        }
      }
      break;
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}
#endif

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)piece->data;
  dt_iop_colorbalance_global_data_t *gd = (dt_iop_colorbalance_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  switch (d->mode)
  {
    case LEGACY:
    {
      const float lift[4] = { 2.0f - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                              2.0f - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                              2.0f - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]), 0.0f },
                  gamma[4] = { d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                               d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                               d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR], 0.0f },
                  gamma_inv[4] = { (gamma[0] != 0.0f) ? 1.0f / gamma[0] : 1000000.0f,
                                   (gamma[1] != 0.0f) ? 1.0f / gamma[1] : 1000000.0f,
                                   (gamma[2] != 0.0f) ? 1.0f / gamma[2] : 1000000.0f, 0.0f },
                  gain[4] = { d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR], 0.0f },
                  contrast = (d->contrast != 0.0f) ? 1.0f / d->contrast : 1000000.0f,
                  grey = d->grey / 100.0f,
                  saturation = d->saturation;

      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 4, 4 * sizeof(float), (void *)&lift);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 5, 4 * sizeof(float), (void *)&gain);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 6, 4 * sizeof(float), (void *)&gamma_inv);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 7, sizeof(float), (void *)&saturation);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 8, sizeof(float), (void *)&contrast);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance, 9, sizeof(float), (void *)&grey);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorbalance, sizes);
      if(err != CL_SUCCESS) goto error;
      return TRUE;

      break;
    }
    case LIFT_GAMMA_GAIN:
    {
      const float lift[4] = { 2.0f - (d->lift[CHANNEL_RED] * d->lift[CHANNEL_FACTOR]),
                              2.0f - (d->lift[CHANNEL_GREEN] * d->lift[CHANNEL_FACTOR]),
                              2.0f - (d->lift[CHANNEL_BLUE] * d->lift[CHANNEL_FACTOR]), 0.0f },
                  gamma[4] = { d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                               d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                               d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR], 0.0f },
                  gamma_inv[4] = { (gamma[0] != 0.0f) ? 1.0f / gamma[0] : 1000000.0f,
                                   (gamma[1] != 0.0f) ? 1.0f / gamma[1] : 1000000.0f,
                                   (gamma[2] != 0.0f) ? 1.0f / gamma[2] : 1000000.0f, 0.0f },
                  gain[4] = { d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR], 0.0f },
                  contrast = (d->contrast != 0.0f) ? 1.0f / d->contrast : 1000000.0f,
                  grey = d->grey / 100.0f,
                  saturation = d->saturation;

      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_lgg, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_lgg, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_lgg, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_lgg, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_lgg, 4, 4 * sizeof(float), (void *)&lift);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_lgg, 5, 4 * sizeof(float), (void *)&gain);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_lgg, 6, 4 * sizeof(float), (void *)&gamma_inv);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_lgg, 7, sizeof(float), (void *)&saturation);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_lgg, 8, sizeof(float), (void *)&contrast);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_lgg, 9, sizeof(float), (void *)&grey);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorbalance_lgg, sizes);
      if(err != CL_SUCCESS) goto error;
      return TRUE;

      break;
    }
    case SLOPE_OFFSET_POWER:
    {
      const float lift[4] = { ( d->lift[CHANNEL_RED] + d->lift[CHANNEL_FACTOR] - 2.0f ),
                              ( d->lift[CHANNEL_GREEN] + d->lift[CHANNEL_FACTOR] - 2.0f ),
                              ( d->lift[CHANNEL_BLUE] + d->lift[CHANNEL_FACTOR] - 2.0f ),
                              0.0f },
                  gamma[4] = { d->gamma[CHANNEL_RED] * d->gamma[CHANNEL_FACTOR],
                               d->gamma[CHANNEL_GREEN] * d->gamma[CHANNEL_FACTOR],
                               d->gamma[CHANNEL_BLUE] * d->gamma[CHANNEL_FACTOR],
                               0.0f },
                  gamma_inv[4] = { (gamma[0] != 0.0f) ? 1.0f / gamma[0] : 1000000.0f,
                                   (gamma[1] != 0.0f) ? 1.0f / gamma[1] : 1000000.0f,
                                   (gamma[2] != 0.0f) ? 1.0f / gamma[2] : 1000000.0f,
                                   0.0f },
                  gain[4] = { d->gain[CHANNEL_RED] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_GREEN] * d->gain[CHANNEL_FACTOR],
                              d->gain[CHANNEL_BLUE] * d->gain[CHANNEL_FACTOR],
                              0.0f },
                  contrast = (d->contrast != 0.0f) ? 1.0f / d->contrast : 1000000.0f,
                  grey = d->grey / 100.0f,
                  saturation = d->saturation;

      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_cdl, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_cdl, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_cdl, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_cdl, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_cdl, 4, 4 * sizeof(float), (void *)&lift);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_cdl, 5, 4 * sizeof(float), (void *)&gain);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_cdl, 6, 4 * sizeof(float), (void *)&gamma_inv);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_cdl, 7, sizeof(float), (void *)&saturation);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_cdl, 8, sizeof(float), (void *)&contrast);
      dt_opencl_set_kernel_arg(devid, gd->kernel_colorbalance_cdl, 9, sizeof(float), (void *)&grey);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorbalance_cdl, sizes);
      if(err != CL_SUCCESS) goto error;
      return TRUE;

      break;
    }
  }

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorbalance] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colorbalance_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorbalance_params_t));
  module->default_enabled = 0;
  module->priority = 449; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colorbalance_params_t);
  module->gui_data = NULL;
  dt_iop_colorbalance_params_t tmp = (dt_iop_colorbalance_params_t){ SLOPE_OFFSET_POWER,
                                                                     { 1.0f, 1.0f, 1.0f, 1.0f },
                                                                     { 1.0f, 1.0f, 1.0f, 1.0f },
                                                                     { 1.0f, 1.0f, 1.0f, 1.0f },
                                                                     1.0f, 1.0f, 18.0f };

  memcpy(module->params, &tmp, sizeof(dt_iop_colorbalance_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorbalance_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_colorbalance_global_data_t *gd
      = (dt_iop_colorbalance_global_data_t *)malloc(sizeof(dt_iop_colorbalance_global_data_t));
  module->data = gd;
  gd->kernel_colorbalance = dt_opencl_create_kernel(program, "colorbalance");
  gd->kernel_colorbalance_lgg = dt_opencl_create_kernel(program, "colorbalance_lgg");
  gd->kernel_colorbalance_cdl = dt_opencl_create_kernel(program, "colorbalance_cdl");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorbalance_global_data_t *gd = (dt_iop_colorbalance_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorbalance);
  dt_opencl_free_kernel(gd->kernel_colorbalance_lgg);
  dt_opencl_free_kernel(gd->kernel_colorbalance_cdl);
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorbalance_data_t *d = (dt_iop_colorbalance_data_t *)(piece->data);
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)p1;

  d->mode = p->mode;

  switch ( d->mode )
  {
    case SLOPE_OFFSET_POWER:
    {
      // Correct the luminance in RGB parameters so we don't affect it
      float XYZ[3];

      dt_prophotorgb_to_XYZ((const float *)&p->lift[CHANNEL_RED], XYZ);
      d->lift[CHANNEL_FACTOR] = p->lift[CHANNEL_FACTOR];
      d->lift[CHANNEL_RED] = (p->lift[CHANNEL_RED] - XYZ[1]) * 2.0f + 1.f;
      d->lift[CHANNEL_GREEN] = (p->lift[CHANNEL_GREEN] - XYZ[1]) * 2.0f + 1.f;
      d->lift[CHANNEL_BLUE] = (p->lift[CHANNEL_BLUE] - XYZ[1]) * 2.0f + 1.f;

      dt_prophotorgb_to_XYZ((const float *)&p->gamma[CHANNEL_RED], XYZ);
      d->gamma[CHANNEL_FACTOR] = p->gamma[CHANNEL_FACTOR];
      d->gamma[CHANNEL_RED] = (p->gamma[CHANNEL_RED] - XYZ[1]) * 2.0f + 1.f;
      d->gamma[CHANNEL_GREEN] = (p->gamma[CHANNEL_GREEN] - XYZ[1]) * 2.0f + 1.f;
      d->gamma[CHANNEL_BLUE] = (p->gamma[CHANNEL_BLUE] - XYZ[1]) * 2.0f + 1.f;

      dt_prophotorgb_to_XYZ((const float *)&p->gain[CHANNEL_RED], XYZ);
      d->gain[CHANNEL_FACTOR] = p->gain[CHANNEL_FACTOR];
      d->gain[CHANNEL_RED] = (p->gain[CHANNEL_RED] - XYZ[1]) * 2.0f + 1.f;
      d->gain[CHANNEL_GREEN] = (p->gain[CHANNEL_GREEN] - XYZ[1]) * 2.0f + 1.f;
      d->gain[CHANNEL_BLUE] = (p->gain[CHANNEL_BLUE] - XYZ[1]) * 2.0f + 1.f;

      break;
    }

    case LEGACY:
    {
      // Luminance is not corrected in lift/gamma/gain for compatibility
      for(int i = 0; i < CHANNEL_SIZE; i++)
      {
        d->lift[i] = p->lift[i];
        d->gamma[i] = p->gamma[i];
        d->gain[i] = p->gain[i];
      }

      break;
    }

    case LIFT_GAMMA_GAIN:
    {
      // Divide the parameters by 2.2 to match the correction we got in legacy sRGB mode
      // That is because we work in a larger space
      d->lift[CHANNEL_FACTOR] = p->lift[CHANNEL_FACTOR];
      d->lift[CHANNEL_RED] = (p->lift[CHANNEL_RED] - 1.0f) / 2.2f + 1.f;
      d->lift[CHANNEL_GREEN] = (p->lift[CHANNEL_GREEN] - 1.0f) / 2.2f + 1.f;
      d->lift[CHANNEL_BLUE] = (p->lift[CHANNEL_BLUE] - 1.0f) / 2.2f + 1.f;

      d->gamma[CHANNEL_FACTOR] = p->gamma[CHANNEL_FACTOR];
      d->gamma[CHANNEL_RED] = (p->gamma[CHANNEL_RED] - 1.0f) / 2.2f + 1.f;
      d->gamma[CHANNEL_GREEN] = (p->gamma[CHANNEL_GREEN] - 1.0f) / 2.2f + 1.f;
      d->gamma[CHANNEL_BLUE] = (p->gamma[CHANNEL_BLUE] - 1.0f) / 2.2f + 1.f;

      d->gain[CHANNEL_FACTOR] = p->gain[CHANNEL_FACTOR];
      d->gain[CHANNEL_RED] = (p->gain[CHANNEL_RED] - 1.0f) / 2.2f + 1.f;
      d->gain[CHANNEL_GREEN] = (p->gain[CHANNEL_GREEN] - 1.0f) / 2.2f + 1.f;
      d->gain[CHANNEL_BLUE] = (p->gain[CHANNEL_BLUE] - 1.0f) / 2.2f + 1.f;

      break;
    }

  }


  d->grey = p->grey;
  d->saturation = p->saturation;
  d->contrast = p->contrast;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorbalance_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

static inline void update_saturation_slider_color(GtkWidget *slider, float hue)
{
  float rgb[3];
  if(hue != -1)
  {
    hsl2rgb(rgb, hue, 1.0, 0.5);
    dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
    hsl2rgb(rgb, hue, 0.0, 0.5);
    dt_bauhaus_slider_set_stop(slider, 0.0, rgb[0], rgb[1], rgb[2]);
    gtk_widget_queue_draw(GTK_WIDGET(slider));
  }
}

static inline void set_HSL_sliders(GtkWidget *hue, GtkWidget *sat, float RGB[4])
{
  /** HSL sliders are set from the RGB values at any time.
  * Only the RGB values are saved and used in the computations.
  * The HSL sliders are merely an interface.
  */
  float RGB_norm[3] = { (RGB[CHANNEL_RED] / 2.0f), (RGB[CHANNEL_GREEN]) / 2.0f, (RGB[CHANNEL_BLUE] / 2.0f) };
  float h, s, l;
  rgb2hsl(RGB_norm, &h, &s, &l);

  if (h != -1.0f)
  {
    dt_bauhaus_slider_set_soft(hue, h);
    dt_bauhaus_slider_set_soft(sat, s);
    update_saturation_slider_color(GTK_WIDGET(sat), h);
    gtk_widget_queue_draw(GTK_WIDGET(sat));
  }
  else
  {
    dt_bauhaus_slider_set_soft(hue, -1.0f);
    dt_bauhaus_slider_set_soft(sat, 0.0f);
    gtk_widget_queue_draw(GTK_WIDGET(sat));
  }
}

static inline void normalize_RGB_sliders(GtkWidget *R, GtkWidget *G, GtkWidget *B, float *p, int CHANNEL, int mode)
{
  /** This function aims at normalizing the RGB channels so the global lightness is never affected
  * by the RGB settings and so RGB sliders control only the hue and saturation.
  */

  switch (CHANNEL)
  {
    case (CHANNEL_FACTOR):
    {
      // correct all the channels
      float rgb[3] = {p[CHANNEL_RED] / 2.0f, p[CHANNEL_GREEN] / 2.0f, p[CHANNEL_BLUE] / 2.0f };
      float mean = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
      p[CHANNEL_RED] = (rgb[0] - mean) * 2.0f + 1.f;
      p[CHANNEL_GREEN] = (rgb[1] - mean) * 2.0f + 1.f;
      p[CHANNEL_BLUE] = (rgb[2] - mean) * 2.0f + 1.f;
      break;
    }
    case (CHANNEL_RED):
    {
      // correct all channels but the red
      for (int iter = 0; iter < 50; ++iter)
      {
        float rgb[3] = {p[CHANNEL_RED] / 2.0f, p[CHANNEL_GREEN] / 2.0f, p[CHANNEL_BLUE] / 2.0f };
        float mean = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
        p[CHANNEL_GREEN] = (rgb[1] - mean) * 2.0f + 1.f;
        p[CHANNEL_BLUE] = (rgb[2] - mean) * 2.0f + 1.f;
      }
      break;
    }
    case (CHANNEL_GREEN):
    {
      // correct all the channels
      for (int iter = 0; iter < 50; ++iter)
      {
        float rgb[3] = {p[CHANNEL_RED] / 2.0f, p[CHANNEL_GREEN] / 2.0f, p[CHANNEL_BLUE] / 2.0f };
        float mean = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
        p[CHANNEL_RED] = (rgb[0] - mean) * 2.0f + 1.f;
        p[CHANNEL_BLUE] = (rgb[2] - mean) * 2.0f + 1.f;
      }
      break;
    }
    case (CHANNEL_BLUE):
    {
      // correct all the channels
      for (int iter = 0; iter < 50; ++iter)
      {
        float rgb[3] = {p[CHANNEL_RED] / 2.0f, p[CHANNEL_GREEN] / 2.0f, p[CHANNEL_BLUE] / 2.0f };
        float mean = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
        p[CHANNEL_RED] = (rgb[0] - mean) * 2.0f + 1.f;
        p[CHANNEL_GREEN] = (rgb[1] - mean) * 2.0f + 1.f;
      }
      break;
    }
  }

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(R, p[CHANNEL_RED] - 1.0f);
  dt_bauhaus_slider_set_soft(G, p[CHANNEL_GREEN] - 1.0f);
  dt_bauhaus_slider_set_soft(B, p[CHANNEL_BLUE] - 1.0f);
  darktable.gui->reset = 0;
}

static inline void set_RGB_sliders(GtkWidget *R, GtkWidget *G, GtkWidget *B, float hsl[3], float *p, int mode)
{

  float rgb[3] = { 0.0f };
  hsl2rgb(rgb, hsl[0], hsl[1], hsl[2]);

  if (hsl[0] != -1)
  {
    // Adjustement from http://filmicworlds.com/blog/minimal-color-grading-tools/
    float mean = (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
    p[CHANNEL_RED] = (rgb[0] - mean) * 2.0f  + 1.0f;
    p[CHANNEL_GREEN] = (rgb[1] - mean) * 2.0f + 1.0f;
    p[CHANNEL_BLUE] = (rgb[2] - mean) * 2.0f + 1.0f;

    normalize_RGB_sliders(R, G, B, p, CHANNEL_FACTOR, mode);
  }
}


void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;

  dt_bauhaus_combobox_set(g->mode, p->mode);

  dt_bauhaus_slider_set_soft(g->grey, p->grey);
  dt_bauhaus_slider_set_soft(g->saturation, p->saturation - 1.0f);
  dt_bauhaus_slider_set_soft(g->contrast, 1.0f - p->contrast);

  dt_bauhaus_slider_set_soft(g->lift_factor, p->lift[CHANNEL_FACTOR] - 1.0f);
  dt_bauhaus_slider_set_soft(g->lift_r, p->lift[CHANNEL_RED] - 1.0f);
  dt_bauhaus_slider_set_soft(g->lift_g, p->lift[CHANNEL_GREEN] - 1.0f);
  dt_bauhaus_slider_set_soft(g->lift_b, p->lift[CHANNEL_BLUE] - 1.0f);

  dt_bauhaus_slider_set_soft(g->gamma_factor, p->gamma[CHANNEL_FACTOR] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gamma_r, p->gamma[CHANNEL_RED] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gamma_g, p->gamma[CHANNEL_GREEN] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gamma_b, p->gamma[CHANNEL_BLUE] - 1.0f);

  dt_bauhaus_slider_set_soft(g->gain_factor, p->gain[CHANNEL_FACTOR] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gain_r, p->gain[CHANNEL_RED] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gain_g, p->gain[CHANNEL_GREEN] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gain_b, p->gain[CHANNEL_BLUE] - 1.0f);

#ifdef AUTO
  if (p->mode == LEGACY || p->mode == LIFT_GAMMA_GAIN)
  {
    gtk_widget_set_visible(g->optim_label, FALSE);
    gtk_widget_set_visible(g->auto_color, FALSE);
    gtk_widget_set_visible(g->auto_luma, FALSE);
  }
  else
  {
    gtk_widget_set_visible(g->optim_label, TRUE);
    gtk_widget_set_visible(g->auto_color, TRUE);
    gtk_widget_set_visible(g->auto_luma, TRUE);
  }
#endif

  if (p->mode == LEGACY)
  {
    gtk_widget_set_visible(g->master_box, FALSE);
  }
  else
  {
    gtk_widget_set_visible(g->master_box, TRUE);
  }

  set_HSL_sliders(g->hue_lift, g->sat_lift, p->lift);
  set_HSL_sliders(g->hue_gamma, g->sat_gamma, p->gamma);
  set_HSL_sliders(g->hue_gain, g->sat_gain, p->gain);

#ifdef CONTROLS
  // default control is HSL
  gtk_widget_set_visible(g->lift_r, FALSE);
  gtk_widget_set_visible(g->lift_g, FALSE);
  gtk_widget_set_visible(g->lift_b, FALSE);
  gtk_widget_set_visible(g->gamma_r, FALSE);
  gtk_widget_set_visible(g->gamma_g, FALSE);
  gtk_widget_set_visible(g->gamma_b, FALSE);
  gtk_widget_set_visible(g->gain_r, FALSE);
  gtk_widget_set_visible(g->gain_g, FALSE);
  gtk_widget_set_visible(g->gain_b, FALSE);

  gtk_widget_set_visible(g->hue_lift, TRUE);
  gtk_widget_set_visible(g->sat_lift, TRUE);
  gtk_widget_set_visible(g->hue_gamma, TRUE);
  gtk_widget_set_visible(g->sat_gamma, TRUE);
  gtk_widget_set_visible(g->hue_gain, TRUE);
  gtk_widget_set_visible(g->sat_gain, TRUE);

  int control_mode = dt_bauhaus_combobox_get(g->controls);

  switch (control_mode)
  {
    case HSL:
    {
      break;
    }
    case RGBL:
    {
      gtk_widget_set_visible(g->lift_r, TRUE);
      gtk_widget_set_visible(g->lift_g, TRUE);
      gtk_widget_set_visible(g->lift_b, TRUE);
      gtk_widget_set_visible(g->gamma_r, TRUE);
      gtk_widget_set_visible(g->gamma_g, TRUE);
      gtk_widget_set_visible(g->gamma_b, TRUE);
      gtk_widget_set_visible(g->gain_r, TRUE);
      gtk_widget_set_visible(g->gain_g, TRUE);
      gtk_widget_set_visible(g->gain_b, TRUE);

      gtk_widget_set_visible(g->hue_lift, FALSE);
      gtk_widget_set_visible(g->sat_lift, FALSE);
      gtk_widget_set_visible(g->hue_gamma, FALSE);
      gtk_widget_set_visible(g->sat_gamma, FALSE);
      gtk_widget_set_visible(g->hue_gain, FALSE);
      gtk_widget_set_visible(g->sat_gain, FALSE);
      break;
    }
    case BOTH:
    {
      gtk_widget_set_visible(g->lift_r, TRUE);
      gtk_widget_set_visible(g->lift_g, TRUE);
      gtk_widget_set_visible(g->lift_b, TRUE);
      gtk_widget_set_visible(g->gamma_r, TRUE);
      gtk_widget_set_visible(g->gamma_g, TRUE);
      gtk_widget_set_visible(g->gamma_b, TRUE);
      gtk_widget_set_visible(g->gain_r, TRUE);
      gtk_widget_set_visible(g->gain_g, TRUE);
      gtk_widget_set_visible(g->gain_b, TRUE);

      gtk_widget_set_visible(g->hue_lift, TRUE);
      gtk_widget_set_visible(g->sat_lift, TRUE);
      gtk_widget_set_visible(g->hue_gamma, TRUE);
      gtk_widget_set_visible(g->sat_gamma, TRUE);
      gtk_widget_set_visible(g->hue_gain, TRUE);
      gtk_widget_set_visible(g->sat_gain, TRUE);
      break;
    }
  }
#endif
}

void gui_reset(dt_iop_module_t *self)
{
#ifdef CONTROLS
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  dt_bauhaus_combobox_set(g->controls, HSL);

  gtk_widget_set_visible(g->lift_r, FALSE);
  gtk_widget_set_visible(g->lift_g, FALSE);
  gtk_widget_set_visible(g->lift_b, FALSE);
  gtk_widget_set_visible(g->gamma_r, FALSE);
  gtk_widget_set_visible(g->gamma_g, FALSE);
  gtk_widget_set_visible(g->gamma_b, FALSE);
  gtk_widget_set_visible(g->gain_r, FALSE);
  gtk_widget_set_visible(g->gain_g, FALSE);
  gtk_widget_set_visible(g->gain_b, FALSE);

  gtk_widget_set_visible(g->hue_lift, TRUE);
  gtk_widget_set_visible(g->sat_lift, TRUE);
  gtk_widget_set_visible(g->hue_gamma, TRUE);
  gtk_widget_set_visible(g->sat_gamma, TRUE);
  gtk_widget_set_visible(g->hue_gain, TRUE);
  gtk_widget_set_visible(g->sat_gain, TRUE);
#endif
}

static void mode_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;

  p->mode = dt_bauhaus_combobox_get(combo);
#ifdef AUTO
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  if (p->mode == LEGACY || p->mode == LIFT_GAMMA_GAIN)
  {
    gtk_widget_set_visible(g->optim_label, FALSE);
    gtk_widget_set_visible(g->auto_color, FALSE);
    gtk_widget_set_visible(g->auto_luma, FALSE);
  }
  else
  {
    gtk_widget_set_visible(g->optim_label, TRUE);
    gtk_widget_set_visible(g->auto_color, TRUE);
    gtk_widget_set_visible(g->auto_luma, TRUE);
  }
#endif

  if (p->mode == LEGACY)
  {
    gtk_widget_set_visible(g->master_box, FALSE);
  }
  else
  {
    gtk_widget_set_visible(g->master_box, TRUE);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

#ifdef CONTROLS
static void controls_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  int control_mode = dt_bauhaus_combobox_get(combo);

  switch (control_mode)
  {
    case HSL:
    {
      gtk_widget_set_visible(g->lift_r, FALSE);
      gtk_widget_set_visible(g->lift_g, FALSE);
      gtk_widget_set_visible(g->lift_b, FALSE);
      gtk_widget_set_visible(g->gamma_r, FALSE);
      gtk_widget_set_visible(g->gamma_g, FALSE);
      gtk_widget_set_visible(g->gamma_b, FALSE);
      gtk_widget_set_visible(g->gain_r, FALSE);
      gtk_widget_set_visible(g->gain_g, FALSE);
      gtk_widget_set_visible(g->gain_b, FALSE);

      gtk_widget_set_visible(g->hue_lift, TRUE);
      gtk_widget_set_visible(g->sat_lift, TRUE);
      gtk_widget_set_visible(g->hue_gamma, TRUE);
      gtk_widget_set_visible(g->sat_gamma, TRUE);
      gtk_widget_set_visible(g->hue_gain, TRUE);
      gtk_widget_set_visible(g->sat_gain, TRUE);
      break;
    }
    case RGBL:
    {
      gtk_widget_set_visible(g->lift_r, TRUE);
      gtk_widget_set_visible(g->lift_g, TRUE);
      gtk_widget_set_visible(g->lift_b, TRUE);
      gtk_widget_set_visible(g->gamma_r, TRUE);
      gtk_widget_set_visible(g->gamma_g, TRUE);
      gtk_widget_set_visible(g->gamma_b, TRUE);
      gtk_widget_set_visible(g->gain_r, TRUE);
      gtk_widget_set_visible(g->gain_g, TRUE);
      gtk_widget_set_visible(g->gain_b, TRUE);

      gtk_widget_set_visible(g->hue_lift, FALSE);
      gtk_widget_set_visible(g->sat_lift, FALSE);
      gtk_widget_set_visible(g->hue_gamma, FALSE);
      gtk_widget_set_visible(g->sat_gamma, FALSE);
      gtk_widget_set_visible(g->hue_gain, FALSE);
      gtk_widget_set_visible(g->sat_gain, FALSE);
      break;
    }
    case BOTH:
    {
      gtk_widget_set_visible(g->lift_r, TRUE);
      gtk_widget_set_visible(g->lift_g, TRUE);
      gtk_widget_set_visible(g->lift_b, TRUE);
      gtk_widget_set_visible(g->gamma_r, TRUE);
      gtk_widget_set_visible(g->gamma_g, TRUE);
      gtk_widget_set_visible(g->gamma_b, TRUE);
      gtk_widget_set_visible(g->gain_r, TRUE);
      gtk_widget_set_visible(g->gain_g, TRUE);
      gtk_widget_set_visible(g->gain_b, TRUE);

      gtk_widget_set_visible(g->hue_lift, TRUE);
      gtk_widget_set_visible(g->sat_lift, TRUE);
      gtk_widget_set_visible(g->hue_gamma, TRUE);
      gtk_widget_set_visible(g->sat_gamma, TRUE);
      gtk_widget_set_visible(g->hue_gain, TRUE);
      gtk_widget_set_visible(g->sat_gain, TRUE);
    }
  }
}
#endif

static void hue_lift_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;

  float hsl[3] = {dt_bauhaus_slider_get(slider),
                  dt_bauhaus_slider_get(g->sat_lift),
                  0.4688f};

  update_saturation_slider_color(g->sat_lift, hsl[0]);
  set_RGB_sliders(g->lift_r, g->lift_g, g->lift_b, hsl, p->lift, p->mode);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void sat_lift_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;

  float hsl[3] = {dt_bauhaus_slider_get(g->hue_lift),
                  dt_bauhaus_slider_get(slider),
                  0.4688f};

  set_RGB_sliders(g->lift_r, g->lift_g, g->lift_b, hsl, p->lift, p->mode);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void hue_gamma_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;

  float hsl[3] = {dt_bauhaus_slider_get(slider),
                  dt_bauhaus_slider_get(g->sat_gamma),
                  0.4688f};

  update_saturation_slider_color(g->sat_gamma, hsl[0]);
  set_RGB_sliders(g->gamma_r, g->gamma_g, g->gamma_b, hsl, p->gamma, p->mode);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void sat_gamma_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;

  float hsl[3] = {dt_bauhaus_slider_get(g->hue_gamma),
                  dt_bauhaus_slider_get(slider),
                  0.4688f};

  set_RGB_sliders(g->gamma_r, g->gamma_g, g->gamma_b, hsl, p->gamma, p->mode);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void hue_gain_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;

  float hsl[3] = {dt_bauhaus_slider_get(slider),
                  dt_bauhaus_slider_get(g->sat_gain),
                  0.4688f};

  update_saturation_slider_color(g->sat_gain, hsl[0]);
  set_RGB_sliders(g->gain_r, g->gain_g, g->gain_b, hsl, p->gain, p->mode);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void sat_gain_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;

  float hsl[3] = {dt_bauhaus_slider_get(g->hue_gain),
                  dt_bauhaus_slider_get(slider),
                  0.4688f};

  set_RGB_sliders(g->gain_r, g->gain_g, g->gain_b, hsl, p->gain, p->mode);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->saturation = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void contrast_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->contrast = - dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void grey_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->grey = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lift_factor_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->lift[CHANNEL_FACTOR] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void lift_red_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->lift[CHANNEL_RED] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  normalize_RGB_sliders(g->lift_r, g->lift_g, g->lift_b, p->lift, CHANNEL_RED, p->mode);
  darktable.gui->reset = 1;
  set_HSL_sliders(g->hue_lift, g->sat_lift, p->lift);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void lift_green_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->lift[CHANNEL_GREEN] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  normalize_RGB_sliders(g->lift_r, g->lift_g, g->lift_b, p->lift, CHANNEL_GREEN, p->mode);
  darktable.gui->reset = 1;
  set_HSL_sliders(g->hue_lift, g->sat_lift, p->lift);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void lift_blue_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->lift[CHANNEL_BLUE] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  normalize_RGB_sliders(g->lift_r, g->lift_g, g->lift_b, p->lift, CHANNEL_BLUE, p->mode);
  darktable.gui->reset = 1;
  set_HSL_sliders(g->hue_lift, g->sat_lift, p->lift);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void gamma_factor_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gamma[CHANNEL_FACTOR] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gamma_red_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gamma[CHANNEL_RED] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  normalize_RGB_sliders(g->gamma_r, g->gamma_g, g->gamma_b, p->gamma, CHANNEL_RED, p->mode);
  darktable.gui->reset = 1;
  set_HSL_sliders(g->hue_gamma, g->sat_gamma, p->gamma);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gamma_green_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gamma[CHANNEL_GREEN] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  normalize_RGB_sliders(g->gamma_r, g->gamma_g, g->gamma_b, p->gamma, CHANNEL_GREEN, p->mode);
  darktable.gui->reset = 1;
  set_HSL_sliders(g->hue_gamma, g->sat_gamma, p->gamma);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gamma_blue_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gamma[CHANNEL_BLUE] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  normalize_RGB_sliders(g->gamma_r, g->gamma_g, g->gamma_b, p->gamma, CHANNEL_BLUE, p->mode);
  darktable.gui->reset = 1;
  set_HSL_sliders(g->hue_gamma, g->sat_gamma, p->gamma);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void gain_factor_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gain[CHANNEL_FACTOR] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gain_red_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gain[CHANNEL_RED] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  normalize_RGB_sliders(g->gain_r, g->gain_g, g->gain_b, p->gain, CHANNEL_RED, p->mode);
  darktable.gui->reset = 1;
  set_HSL_sliders(g->hue_gain, g->sat_gain, p->gain);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gain_green_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gain[CHANNEL_GREEN] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  normalize_RGB_sliders(g->gain_r, g->gain_g, g->gain_b, p->gain, CHANNEL_GREEN, p->mode);
  darktable.gui->reset = 1;
  set_HSL_sliders(g->hue_gain, g->sat_gain, p->gain);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void gain_blue_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  if(self->dt->gui->reset) return;
  p->gain[CHANNEL_BLUE] = dt_bauhaus_slider_get(slider) + 1.0f;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  normalize_RGB_sliders(g->gain_r, g->gain_g, g->gain_b, p->gain, CHANNEL_BLUE, p->mode);
  darktable.gui->reset = 1;
  set_HSL_sliders(g->hue_gain, g->sat_gain, p->gain);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

#if 0
static gboolean dt_iop_area_draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  float flt_bg = 0.5;
  if(gtk_widget_get_state_flags(widget) & GTK_STATE_FLAG_SELECTED) flt_bg = 0.6;
  float flt_dark = flt_bg / 1.5, flt_light = flt_bg * 1.5;

  uint32_t bg = ((255 << 24) | ((int)floor(flt_bg * 255 + 0.5) << 16) | ((int)floor(flt_bg * 255 + 0.5) << 8)
                 | (int)floor(flt_bg * 255 + 0.5));
  // bg = 0xffffffff;
  //   uint32_t dark = ((255 << 24) |
  //                  ((int)floor(flt_dark * 255 + 0.5) << 16) |
  //                  ((int)floor(flt_dark * 255 + 0.5) << 8) |
  //                  (int)floor(flt_dark * 255 + 0.5));
  uint32_t light = ((255 << 24) | ((int)floor(flt_light * 255 + 0.5) << 16)
                    | ((int)floor(flt_light * 255 + 0.5) << 8) | (int)floor(flt_light * 255 + 0.5));

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  if(width % 2 == 0) width--;
  if(height % 2 == 0) height--;
  double center_x = (float)width / 2.0, center_y = (float)height / 2.0;
  double diameter = MIN(width, height) - 4;
  double r_outside = diameter / 2.0, r_inside = r_outside * 0.87;
  double r_outside_2 = r_outside * r_outside, r_inside_2 = r_inside * r_inside;

  // clear the background
  cairo_set_source_rgb(cr, flt_bg, flt_bg, flt_bg);
  cairo_paint(cr);

  /* Create an image initialized with the ring colors */
  gint stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
  guint32 *buf = (guint32 *)malloc(sizeof(guint32) * height * stride / 4);

  for(int y = 0; y < height; y++)
  {
    guint32 *p = buf + y * width;

    double dy = -(y + 0.5 - center_y);

    for(int x = 0; x < width; x++)
    {
      double dx = x + 0.5 - center_x;
      double dist = dx * dx + dy * dy;
      if(dist < r_inside_2 || dist > r_outside_2)
      {
        uint32_t col = bg;
        if((abs(dx) < 1 && abs(dy) < 3) || (abs(dx) < 3 && abs(dy) < 1)) col = light;
        *p++ = col;
        continue;
      }

      double angle = atan2(dy, dx) - M_PI_2;
      if(angle < 0.0) angle += 2.0 * M_PI;

      double hue = angle / (2.0 * M_PI);

      float rgb[3];
      hsl2rgb(rgb, hue, 1.0, 0.5);

      *p++ = (((int)floor(rgb[0] * 255 + 0.5) << 16) | ((int)floor(rgb[1] * 255 + 0.5) << 8)
              | (int)floor(rgb[2] * 255 + 0.5));
    }
  }

  cairo_surface_t *source
      = cairo_image_surface_create_for_data((unsigned char *)buf, CAIRO_FORMAT_RGB24, width, height, stride);

  cairo_set_source_surface(cr, source, 0.0, 0.0);
  cairo_paint(cr);
  free(buf);

  // draw border
  float line_width = 1;
  cairo_set_line_width(cr, line_width);

  cairo_set_source_rgb(cr, flt_bg, flt_bg, flt_bg);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, r_outside, 0.0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, center_x, center_y, r_inside, 0.0, 2.0 * M_PI);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, flt_dark, flt_dark, flt_dark);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, r_outside, M_PI, 1.5 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, center_x, center_y, r_inside, 0.0, 0.5 * M_PI);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, flt_light, flt_light, flt_light);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, r_outside, 0.0, 0.5 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, center_x, center_y, r_inside, M_PI, 1.5 * M_PI);
  cairo_stroke(cr);

  // draw selector
  double r = 255 / 255.0, g = 155 / 255.0, b = 40 / 255.0;
  double h, s, v;

  gtk_rgb_to_hsv(r, g, b, &h, &s, &v);

  cairo_save(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.7);

  cairo_translate(cr, center_x, center_y);
  cairo_rotate(cr, h * 2.0 * M_PI - M_PI_2);

  cairo_arc(cr, r_inside * v, 0.0, 3.0, 0, 2.0 * M_PI);
  cairo_stroke(cr);

  cairo_restore(cr);

  cairo_surface_destroy(source);

  return TRUE;
}
#endif


static void autogrey_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
    dt_control_queue_redraw();
  }
  else
  {
    dt_dev_reprocess_all(self->dev);
    dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
    dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

    if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
    {
      dt_control_log(_("wait for the preview to be updated."));
      return;
    }

    float XYZ[3] = { 0.0f };
    float rgb[3] = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);
    dt_XYZ_to_prophotorgb((const float *)XYZ, rgb);

    const float lift[3] = { ( p->lift[CHANNEL_RED] + p->lift[CHANNEL_FACTOR] - 2.0f ),
                              ( p->lift[CHANNEL_GREEN] + p->lift[CHANNEL_FACTOR] - 2.0f ),
                              ( p->lift[CHANNEL_BLUE] + p->lift[CHANNEL_FACTOR] - 2.0f ) },
                gamma[3] = { p->gamma[CHANNEL_RED] * p->gamma[CHANNEL_FACTOR],
                             p->gamma[CHANNEL_GREEN] * p->gamma[CHANNEL_FACTOR],
                             p->gamma[CHANNEL_BLUE] * p->gamma[CHANNEL_FACTOR] },
                gamma_inv[3] = { (gamma[0] != 0.0f) ? 1.0f / gamma[0] : 1000000.0f,
                                 (gamma[1] != 0.0f) ? 1.0f / gamma[1] : 1000000.0f,
                                 (gamma[2] != 0.0f) ? 1.0f / gamma[2] : 1000000.0f },
                gain[3] = { p->gain[CHANNEL_RED] * p->gain[CHANNEL_FACTOR],
                            p->gain[CHANNEL_GREEN] * p->gain[CHANNEL_FACTOR],
                            p->gain[CHANNEL_BLUE] * p->gain[CHANNEL_FACTOR] };

    for(int c = 0; c < 3; c++)
    {
      rgb[c] = CDL(rgb[c], gain[c], lift[c], gamma_inv[c]);
      rgb[c] = CLAMP(rgb[c], 0.0f, 1.0f);
    }

    dt_prophotorgb_to_XYZ((const float *)rgb, XYZ);

    p->grey = XYZ[1] * 100.0f;

    dt_bauhaus_slider_set_soft(g->grey, p->grey);

    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

#ifdef AUTO
static void lift_neutralize_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
    dt_control_queue_redraw();
  }
  else
  {
    dt_dev_reprocess_all(self->dev);
    dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
    dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

    if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
    {
      dt_control_log(_("wait for the preview to be updated."));
      return;
    }

    const float gain[3] = { p->gain[CHANNEL_RED],
                            p->gain[CHANNEL_GREEN],
                            p->gain[CHANNEL_BLUE] };

    float XYZ[3] = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);
    float RGB[3] = { 0.0f };
    dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

    // Save the patch color for the optimization
#ifdef OPTIM
    for (int c = 0; c < 3; ++c) g->color_patches_lift[c] = RGB[c];
    g->color_patches_flags[LIFT] = 1;
#endif

    // Compute the RGB values after the CDL factors
    for (int c = 0; c < 3; ++c) RGB[c] = CDL( RGB[c], \
                                              p->gain[CHANNEL_FACTOR], \
                                              p->lift[CHANNEL_FACTOR] - 1.0f, \
                                              1.0f / p->gamma[CHANNEL_FACTOR] );

    // Compute the average grey
    float grey = 0.0;
    for (int c = 0; c < 3; ++c) grey += RGB[c] / 3.0f;

    // Get the parameter
    for (int c = 0; c < 3; ++c) RGB[c] = grey - RGB[c] * gain[c];

    // Correct the luminance
    dt_prophotorgb_to_XYZ(RGB, XYZ);
    for (int c = 0; c < 3; ++c) RGB[c] = (RGB[c] - XYZ[1]) + 1.0f;

    p->lift[CHANNEL_RED] = RGB[0];
    p->lift[CHANNEL_GREEN] = RGB[1];
    p->lift[CHANNEL_BLUE] = RGB[2];

    darktable.gui->reset = 1;
    dt_bauhaus_slider_set_soft(g->lift_r, p->lift[CHANNEL_RED] - 1.0f);
    dt_bauhaus_slider_set_soft(g->lift_g, p->lift[CHANNEL_GREEN] - 1.0f);
    dt_bauhaus_slider_set_soft(g->lift_b, p->lift[CHANNEL_BLUE] - 1.0f);
    set_HSL_sliders(g->hue_lift, g->sat_lift, p->lift);
    darktable.gui->reset = 0;

    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static void gamma_neutralize_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
    dt_control_queue_redraw();
  }
  else
  {
    dt_dev_reprocess_all(self->dev);
    dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
    dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

    if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
    {
      dt_control_log(_("wait for the preview to be updated."));
      return;
    }

    float XYZ[3] = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);
    float RGB[3] = { 0.0f };
    dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

    // Save the patch color for the optimization
#ifdef OPTIM
    for (int c = 0; c < 3; ++c) g->color_patches_gamma[c] = RGB[c];
    g->color_patches_flags[GAMMA] = 1;
#endif

    // Compute the RGB values after the CDL factors
    for (int c = 0; c < 3; ++c) RGB[c] = CDL( RGB[c], \
                                              p->gain[CHANNEL_FACTOR], \
                                              p->lift[CHANNEL_FACTOR] - 1.0f, \
                                              1.0f / p->gamma[CHANNEL_FACTOR] );

    // Compute the average grey
    float grey = 0.0;
    for (int c = 0; c < 3; ++c) grey += RGB[c] / 3.0f;

    // Get the parameter
    for (int c = 0; c < 3; ++c) RGB[c] = logf(RGB[c]) / logf(grey);

    // Correct the luminance
    dt_prophotorgb_to_XYZ(RGB, XYZ);
    for (int c = 0; c < 3; ++c) RGB[c] = (RGB[c] - XYZ[1]) + 1.0f;

    p->gamma[CHANNEL_RED] = RGB[0];
    p->gamma[CHANNEL_GREEN] = RGB[1];
    p->gamma[CHANNEL_BLUE] = RGB[2];

    darktable.gui->reset = 1;
    dt_bauhaus_slider_set_soft(g->gamma_r, p->gamma[CHANNEL_RED] - 1.0f);
    dt_bauhaus_slider_set_soft(g->gamma_g, p->gamma[CHANNEL_GREEN] - 1.0f);
    dt_bauhaus_slider_set_soft(g->gamma_b, p->gamma[CHANNEL_BLUE] - 1.0f);
    set_HSL_sliders(g->hue_gamma, g->sat_gamma, p->gamma);
    darktable.gui->reset = 0;

    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static void gain_neutralize_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
    dt_control_queue_redraw();
  }
  else
  {
    dt_dev_reprocess_all(self->dev);
    dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
    dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

    if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
    {
      dt_control_log(_("wait for the preview to be updated."));
      return;
    }

    float XYZ[3] = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);
    float RGB[3] = { 0.0f };
    dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

    // Save the patch color for the optimization
#ifdef OPTIM
    for (int c = 0; c < 3; c++) g->color_patches_gain[c] = RGB[c];
    g->color_patches_flags[GAIN] = 1;
#endif

    // Compute the RGB values after the CDL factors
    for (int c = 0; c < 3; ++c) RGB[c] = CDL( RGB[c], \
                                              p->gain[CHANNEL_FACTOR], \
                                              p->lift[CHANNEL_FACTOR] - 1.0f, \
                                              1.0f / p->gamma[CHANNEL_FACTOR] );

    // Compute the average grey
    float grey = 0.0;
    for (int c = 0; c < 3; ++c) grey += RGB[c] / 3.0f;

    // Get the parameter
    for (int c = 0; c < 3; ++c) RGB[c] = grey / MAX(RGB[c], 0.000001f);

    // Correct the luminance
    dt_prophotorgb_to_XYZ(RGB, XYZ);
    for (int c = 0; c < 3; ++c) RGB[c] = (RGB[c] - XYZ[1]) + 1.0f;

    p->gain[CHANNEL_RED] = RGB[0];
    p->gain[CHANNEL_GREEN] = RGB[1];
    p->gain[CHANNEL_BLUE] = RGB[2];

    darktable.gui->reset = 1;
    dt_bauhaus_slider_set_soft(g->gain_r, p->gain[CHANNEL_RED] - 1.0f);
    dt_bauhaus_slider_set_soft(g->gain_g, p->gain[CHANNEL_GREEN] - 1.0f);
    dt_bauhaus_slider_set_soft(g->gain_b, p->gain[CHANNEL_BLUE] - 1.0f);
    set_HSL_sliders(g->hue_gain, g->sat_gain, p->gain);
    darktable.gui->reset = 0;

    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static void optimize_color_pressed_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  if(g->color_patches_flags[0] != 1 || g->color_patches_flags[1] != 1 || g->color_patches_flags[2] != 1)
  {
    dt_control_log(_("you need to select 3 color samples first"));
    return;
  }

  // Build the CDL-corrected samples (after the factors)
  float samples_lift[3] = { 0.f };
  float samples_gamma[3] = { 0.f };
  float samples_gain[3] = { 0.f };

  for (int c = 0; c < 3; ++c)
  {
    samples_lift[c] = CDL(g->color_patches_lift[c], p->gain[CHANNEL_FACTOR], p->lift[CHANNEL_FACTOR] - 1.0f, 1.0f / p->gamma[CHANNEL_FACTOR]);
    samples_gamma[c] = CDL(g->color_patches_gamma[c], p->gain[CHANNEL_FACTOR], p->lift[CHANNEL_FACTOR] - 1.0f, 1.0f / p->gamma[CHANNEL_FACTOR]);
    samples_gain[c] = CDL(g->color_patches_gain[c], p->gain[CHANNEL_FACTOR], p->lift[CHANNEL_FACTOR] - 1.0f, 1.0f / p->gamma[CHANNEL_FACTOR]);
  }

  float XYZ[3] = { 0.f };

  // Get the average patches RGB value (= neutral grey equivalents) after the CDL factors
  float greys[3] = { 0.0 };

  for (int c = 0; c < 3; ++c)
  {
    greys[0] += samples_lift[c] / 3.0f;
    greys[1] += samples_gamma[c] / 3.0f;
    greys[2] += samples_gain[c] / 3.0f;
  }

  // Get the current params
  float RGB_lift[3] = { p->lift[CHANNEL_RED] - 1.0f, p->lift[CHANNEL_GREEN] - 1.0f, p->lift[CHANNEL_BLUE] - 1.0f };
  float RGB_gamma[3] = { p->gamma[CHANNEL_RED], p->gamma[CHANNEL_GREEN], p->gamma[CHANNEL_BLUE] };
  float RGB_gain[3] = { p->gain[CHANNEL_RED], p->gain[CHANNEL_GREEN], p->gain[CHANNEL_BLUE] };

  /** Optimization loop :
  * We try to find the CDL curves that neutralize the 3 input color patches, while not affecting the overall lightness.
  * But this is a non-linear overconstrained problem with tainted inputs, so the best we can do is a numerical optimization.
  * To do so, we compute each parameter of each RGB curve from the input color and the 2 other parameters.
  * Then, we loop over the previous optimization until the difference between 2 updates is insignificant.
  * This would need a proper stopping criterion based on convergence analysis, but it would be overkill here since
  * it should converge usually in 20 iterations, and maximum in 100.
  * Also, the convergence has not been proven formally.
  * For better color accuracy, we compute on luminance corrected RGB values (after the main factors corrections).
  * To avoid divergence, we constrain the parameters between +- 0.25 around the neutral value.
  * Experimentally, nothing good happens out of these bounds.
  */
  for (int runs = 0 ; runs < 100 ; ++runs)
  {
    // compute RGB slope/gain
    for (int c = 0; c < 3; ++c) RGB_gain[c] = CLAMP((powf(MAX(greys[GAIN], 0.000001f), RGB_gamma[c]) - RGB_lift[c]) / MAX(samples_gain[c], 0.000001f), 0.75f, 1.25f);
    // compute RGB offset/lift
    for (int c = 0; c < 3; ++c) RGB_lift[c] = CLAMP(powf(MAX(greys[LIFT], 0.000001f), RGB_gamma[c]) - MAX(samples_lift[c], 0.000001f)  * RGB_gain[c], -0.25f, 0.25f);
    // compute  power/gamma
    for (int c = 0; c < 3; ++c) RGB_gamma[c] = CLAMP(logf(MAX(RGB_gain[c] * samples_gamma[c] + RGB_lift[c], 0.000001f)) / logf(MAX(greys[GAMMA], 0.000001f)), 0.25f, 1.25f);
  }

  // correct the luminance on RGB parameters
  dt_prophotorgb_to_XYZ((const float *)RGB_lift, (float *)XYZ);
  for (int c = 0; c < 3; ++c) RGB_lift[c] = (RGB_lift[c] - XYZ[1]) + 1.0f;
  dt_prophotorgb_to_XYZ((const float *)RGB_gamma, (float *)XYZ);
  for (int c = 0; c < 3; ++c) RGB_gamma[c] = (RGB_gamma[c] - XYZ[1]) + 1.0f;
  dt_prophotorgb_to_XYZ((const float *)RGB_lift, (float *)XYZ);
  for (int c = 0; c < 3; ++c) RGB_gain[c] = (RGB_gain[c] - XYZ[1]) + 1.0f;

  // save
  p->lift[CHANNEL_RED] = RGB_lift[0];
  p->lift[CHANNEL_GREEN] = RGB_lift[1];
  p->lift[CHANNEL_BLUE] = RGB_lift[2];
  p->gamma[CHANNEL_RED] = RGB_gamma[0];
  p->gamma[CHANNEL_GREEN] = RGB_gamma[1];
  p->gamma[CHANNEL_BLUE] = RGB_gamma[2];
  p->gain[CHANNEL_RED] = RGB_gain[0];
  p->gain[CHANNEL_GREEN] = RGB_gain[1];
  p->gain[CHANNEL_BLUE] = RGB_gain[2];

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->lift_r, p->lift[CHANNEL_RED] - 1.0f);
  dt_bauhaus_slider_set_soft(g->lift_g, p->lift[CHANNEL_GREEN] - 1.0f);
  dt_bauhaus_slider_set_soft(g->lift_b, p->lift[CHANNEL_BLUE] - 1.0f);
  set_HSL_sliders(g->hue_lift, g->sat_lift, p->lift);

  dt_bauhaus_slider_set_soft(g->gamma_r, p->gamma[CHANNEL_RED] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gamma_g, p->gamma[CHANNEL_GREEN] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gamma_b, p->gamma[CHANNEL_BLUE] - 1.0f);
  set_HSL_sliders(g->hue_gamma, g->sat_gamma, p->gamma);

  dt_bauhaus_slider_set_soft(g->gain_r, p->gain[CHANNEL_RED] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gain_g, p->gain[CHANNEL_GREEN] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gain_b, p->gain[CHANNEL_BLUE] - 1.0f);
  set_HSL_sliders(g->hue_gain, g->sat_gain, p->gain);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lift_auto_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
    dt_control_queue_redraw();
    dt_dev_reprocess_all(self->dev);
  }
  else
  {
    dt_dev_reprocess_all(self->dev);
    dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
    dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

    if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
    {
      dt_control_log(_("wait for the preview to be updated."));
      return;
    }

    float XYZ[3] = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color_min, XYZ);

#ifdef OPTIM
    g->luma_patches[LIFT] = XYZ[1];
    g->luma_patches_flags[LIFT] = 1;
#endif

    float RGB[3] = { 0.0f };
    dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

    p->lift[CHANNEL_FACTOR] = - p->gain[CHANNEL_FACTOR] * XYZ[1] + 1.0f;

    dt_bauhaus_slider_set_soft(g->lift_factor, p->lift[CHANNEL_FACTOR] - 1.0f);

    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static void gamma_auto_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
    dt_control_queue_redraw();
  }
  else
  {
    dt_dev_reprocess_all(self->dev);
    dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
    dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

    if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
    {
      dt_control_log(_("wait for the preview to be updated."));
      return;
    }

    float XYZ[3] = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color, XYZ);

#ifdef OPTIM
    g->luma_patches[GAMMA] = XYZ[1];
    g->luma_patches_flags[GAMMA] = 1;
#endif

    float RGB[3] = { 0.0f };
    dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

    p->gamma[CHANNEL_FACTOR] =  logf(MAX(p->gain[CHANNEL_FACTOR] * XYZ[1] + p->lift[CHANNEL_FACTOR] - 1.0f, 0.000001f)) / logf(0.1800f);

    dt_bauhaus_slider_set_soft(g->gamma_factor, p->gamma[CHANNEL_FACTOR] - 1.0f);

    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static void gain_auto_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    dt_iop_request_focus(self);
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
    dt_control_queue_redraw();
  }
  else
  {
    dt_dev_reprocess_all(self->dev);
    dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
    dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

    if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
    {
      dt_control_log(_("wait for the preview to be updated."));
      return;
    }

    float XYZ[3] = { 0.0f };
    dt_Lab_to_XYZ((const float *)self->picked_color_max, XYZ);

#ifdef OPTIM
    g->luma_patches[GAIN] = XYZ[1];
    g->luma_patches_flags[GAIN] = 1;
#endif

    float RGB[3] = { 0.0f };
    dt_XYZ_to_prophotorgb((const float *)XYZ, RGB);

    p->gain[CHANNEL_FACTOR] = p->lift[CHANNEL_FACTOR] / (XYZ[1]);

    dt_bauhaus_slider_set_soft(g->gain_factor, p->gain[CHANNEL_FACTOR] - 1.0f);

    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static void optimize_luma_pressed_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;

  if(g->luma_patches_flags[0] != 1 || g->luma_patches_flags[1] != 1 || g->luma_patches_flags[2] != 1)
  {
    dt_control_log(_("you need to select 3 luma samples first"));
    return;
  }

  /** Optimization loop :
  * We try to find the CDL curves that neutralize the 3 input luma patches
  */
  for (int runs = 0 ; runs < 100 ; ++runs)
  {
    p->gain[CHANNEL_FACTOR] = CLAMP(p->lift[CHANNEL_FACTOR] / g->luma_patches[GAIN], 0.0f, 2.0f);
    p->lift[CHANNEL_FACTOR] = CLAMP(-p->gain[CHANNEL_FACTOR] * g->luma_patches[LIFT] + 1.0f, 0.0f, 2.0f);
    p->gamma[CHANNEL_FACTOR] = CLAMP(logf(MAX(p->gain[CHANNEL_FACTOR] * g->luma_patches[GAMMA] + p->lift[CHANNEL_FACTOR] - 1.0f, 0.000001f)) / logf(0.1800f), 0.0f, 2.0f);
  }

  dt_bauhaus_slider_set_soft(g->lift_factor, p->lift[CHANNEL_FACTOR] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gamma_factor, p->gamma[CHANNEL_FACTOR] - 1.0f);
  dt_bauhaus_slider_set_soft(g->gain_factor, p->gain[CHANNEL_FACTOR] - 1.0f);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

static void draw_hue_slider(GtkWidget *slider)
{
  dt_bauhaus_slider_set_stop(slider, 0.0f, 1.0f, 0.0f, 0.0f);
  dt_bauhaus_slider_set_stop(slider, 0.166f, 1.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(slider, 0.322f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(slider, 0.498f, 0.0f, 1.0f, 1.0f);
  dt_bauhaus_slider_set_stop(slider, 0.664f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(slider, 0.830f, 1.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(slider, 1.0f, 1.0f, 0.0f, 0.0f);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorbalance_gui_data_t));
  dt_iop_colorbalance_gui_data_t *g = (dt_iop_colorbalance_gui_data_t *)self->gui_data;
  dt_iop_colorbalance_params_t *p = (dt_iop_colorbalance_params_t *)self->params;

  g->mode = NULL;

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // mode choice
  g->mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mode, NULL, _("mode"));
  dt_bauhaus_combobox_add(g->mode, _("lift, gamma, gain (ProPhotoRGB)"));
  dt_bauhaus_combobox_add(g->mode, _("slope, offset, power (ProPhotoRGB)"));
  dt_bauhaus_combobox_add(g->mode, _("lift, gamma, gain (sRGB)"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->mode), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->mode, _("color-grading mapping method"));
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_callback), self);


  // control choice
#ifdef CONTROLS
  g->controls = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->controls, NULL, _("color control sliders"));
  dt_bauhaus_combobox_add(g->controls, _("HSL"));
  dt_bauhaus_combobox_add(g->controls, _("RGBL"));
  dt_bauhaus_combobox_add(g->controls, _("both"));
  dt_bauhaus_combobox_set_default(g->controls, HSL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->controls), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->controls, _("color-grading mapping method"));
  g_signal_connect(G_OBJECT(g->controls), "value-changed", G_CALLBACK(controls_callback), self);
#endif

  // master
  g->master_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->master_box), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(g->master_box), dt_ui_section_label_new(_("master")), FALSE, FALSE, 2);

  g->saturation = dt_bauhaus_slider_new_with_range_and_feedback(self, -0.5, 0.5, 0.0005, p->saturation - 1.0f, 4, 0);
  dt_bauhaus_slider_enable_soft_boundaries(g->saturation, -1.0f, 1.0f);
  dt_bauhaus_widget_set_label(g->saturation, NULL, _("saturation"));
  gtk_box_pack_start(GTK_BOX(g->master_box), g->saturation, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->saturation, _("saturation"));
  g_signal_connect(G_OBJECT(g->saturation), "value-changed", G_CALLBACK(saturation_callback), self);

  g->grey = dt_bauhaus_slider_new_with_range(self, 0.1, 100., 0.5, p->grey, 1);
  dt_bauhaus_widget_set_label(g->grey, NULL, _("grey fulcrum"));
  gtk_box_pack_start(GTK_BOX(g->master_box), g->grey, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->grey, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey, _("adjust to match a neutral tone"));
  g_signal_connect(G_OBJECT(g->grey), "value-changed", G_CALLBACK(grey_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->grey, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);
  g_signal_connect(G_OBJECT(g->grey), "quad-pressed", G_CALLBACK(autogrey_callback), self);

  g->contrast = dt_bauhaus_slider_new_with_range_and_feedback(self, -0.5, 0.5, 0.005, p->contrast - 1.0f, 4, 0);
  dt_bauhaus_slider_enable_soft_boundaries(g->contrast, -0.99, 0.99);
  dt_bauhaus_widget_set_label(g->contrast, NULL, _("contrast"));
  gtk_box_pack_start(GTK_BOX(g->master_box), g->contrast, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->contrast, _("contrast"));
  g_signal_connect(G_OBJECT(g->contrast), "value-changed", G_CALLBACK(contrast_callback), self);


  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

#if 0//def SHOW_COLOR_WHEELS
  GtkWidget *area = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  gtk_box_pack_start(GTK_BOX(hbox), area, TRUE, TRUE, 0);

  //   gtk_widget_add_events(g->area,
  //                         GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK |
  //                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(dt_iop_area_draw), self);
  //   g_signal_connect (G_OBJECT (area), "button-press-event",
  //                     G_CALLBACK (dt_iop_colorbalance_button_press), self);
  //   g_signal_connect (G_OBJECT (area), "motion-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_motion_notify), self);
  //   g_signal_connect (G_OBJECT (area), "leave-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_leave_notify), self);

  area = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  gtk_box_pack_start(GTK_BOX(hbox), area, TRUE, TRUE, 0);

  //   gtk_widget_add_events(g->area,
  //                         GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK |
  //                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(dt_iop_area_draw), self);
  //   g_signal_connect (G_OBJECT (area), "button-press-event",
  //                     G_CALLBACK (dt_iop_colorbalance_button_press), self);
  //   g_signal_connect (G_OBJECT (area), "motion-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_motion_notify), self);
  //   g_signal_connect (G_OBJECT (area), "leave-notify-event",
  //                     G_CALLBACK (dt_iop_colorbalance_leave_notify), self);

  area = dtgtk_drawing_area_new_with_aspect_ratio(1.0);
  gtk_box_pack_start(GTK_BOX(hbox), area, TRUE, TRUE, 0);

  //   gtk_widget_add_events(g->area,
  //                         GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK |
  //                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(dt_iop_area_draw), self);
//   g_signal_connect (G_OBJECT (area), "button-press-event",
//                     G_CALLBACK (dt_iop_colorbalance_button_press), self);
//   g_signal_connect (G_OBJECT (area), "motion-notify-event",
//                     G_CALLBACK (dt_iop_colorbalance_motion_notify), self);
//   g_signal_connect (G_OBJECT (area), "leave-notify-event",
//                     G_CALLBACK (dt_iop_colorbalance_leave_notify), self);
#endif

#define ADD_FACTOR(which) \
  g->which##_factor = dt_bauhaus_slider_new_with_range_and_feedback(self, -0.5, 0.5, 0.0005, p->which[CHANNEL_FACTOR] - 1.0f, 4, 0);\
  dt_bauhaus_slider_enable_soft_boundaries(g->which##_factor, -1.0, 1.0); \
  dt_bauhaus_slider_set_stop(g->which##_factor, 0.0, 0.0, 0.0, 0.0);\
  dt_bauhaus_slider_set_stop(g->which##_factor, 1.0, 1.0, 1.0, 1.0);\
  gtk_widget_set_tooltip_text(g->which##_factor, _("factor of " #which));\
  dt_bauhaus_widget_set_label(g->which##_factor, _(#which), _("factor"));\
  g_signal_connect(G_OBJECT(g->which##_factor), "value-changed", G_CALLBACK(which##_factor_callback), self);\
  gtk_box_pack_start(GTK_BOX(self->widget), g->which##_factor, TRUE, TRUE, 0);\
  dt_bauhaus_widget_set_quad_paint(g->which##_factor, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);\
  g_signal_connect(G_OBJECT(g->which##_factor), "quad-pressed", G_CALLBACK(which##_auto_callback), self);

#define ADD_CHANNEL(which, c, n, N) \
  g->which##_##c = dt_bauhaus_slider_new_with_range_and_feedback(self, -0.5, 0.5, 0.0005, p->which[CHANNEL_##N] - 1.0f, 4, 0);\
  dt_bauhaus_slider_enable_soft_boundaries(g->which##_##c, -1.0, 1.0); \
  gtk_widget_set_tooltip_text(g->which##_##c, _("factor of " #n " for " #which));\
  dt_bauhaus_widget_set_label(g->which##_##c, _(#which), _(#n));\
  g_signal_connect(G_OBJECT(g->which##_##c), "value-changed", G_CALLBACK(which##_##n##_callback), self);\
  gtk_box_pack_start(GTK_BOX(self->widget), g->which##_##c, TRUE, TRUE, 0);

  /* lift */
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("shadows : lift / offset")), FALSE, FALSE, 5);

  static const char *lift_messages[] = { N_("factor of lift"), N_("lift") };
  (void)lift_messages;
  ADD_FACTOR(lift)

  g->hue_lift = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 1.0f, 0.0005f, 0.0f, 4, 0);
  dt_bauhaus_widget_set_label(g->hue_lift, NULL, _("hue"));
  draw_hue_slider(g->hue_lift);
  gtk_widget_set_tooltip_text(g->hue_lift, _("select the hue"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->hue_lift, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->hue_lift), "value-changed", G_CALLBACK(hue_lift_callback), self);
#ifdef AUTO
  dt_bauhaus_widget_set_quad_paint(g->hue_lift, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);
  g_signal_connect(G_OBJECT(g->hue_lift), "quad-pressed", G_CALLBACK(lift_neutralize_callback), self);
#endif

  g->sat_lift = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 0.25f, 0.0005f, 0.0f, 4, 0);
  dt_bauhaus_slider_enable_soft_boundaries(g->sat_lift, 0.0f, 1.0f);
  dt_bauhaus_widget_set_label(g->sat_lift, NULL, _("saturation"));
  dt_bauhaus_slider_set_stop(g->sat_lift, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(g->sat_lift, 1.0f, 1.0f, 1.0f, 1.0f);
  gtk_widget_set_tooltip_text(g->sat_lift, _("select the saturation"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->sat_lift, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->sat_lift), "value-changed", G_CALLBACK(sat_lift_callback), self);


  static const char *lift_red_messages[] = { N_("factor of red for lift"), N_("red") };
  (void)lift_red_messages;
  ADD_CHANNEL(lift, r, red, RED)
  dt_bauhaus_slider_set_stop(g->lift_r, 0.0, 0.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->lift_r, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->lift_r, 1.0, 1.0, 0.0, 0.0);

  static const char *lift_green_messages[] = { N_("factor of green for lift"), N_("green") };
  (void)lift_green_messages;
  ADD_CHANNEL(lift, g, green, GREEN)
  dt_bauhaus_slider_set_stop(g->lift_g, 0.0, 1.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->lift_g, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->lift_g, 1.0, 0.0, 1.0, 0.0);

  static const char *lift_blue_messages[] = { N_("factor of blue for lift"), N_("blue") };
  (void)lift_blue_messages;
  ADD_CHANNEL(lift, b, blue, BLUE)
  dt_bauhaus_slider_set_stop(g->lift_b, 0.0, 1.0, 1.0, 0.0);
  dt_bauhaus_slider_set_stop(g->lift_b, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->lift_b, 1.0, 0.0, 0.0, 1.0);

  /* gamma */
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("mid-tones : gamma / power")), FALSE, FALSE, 5);

  static const char *gamma_messages[] = { N_("factor of gamma"), N_("gamma") };
  (void)gamma_messages;
  ADD_FACTOR(gamma)

  g->hue_gamma = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 1.0f, 0.0005f, 0.0f, 4, 0);
  dt_bauhaus_widget_set_label(g->hue_gamma, NULL, _("hue"));
  draw_hue_slider(g->hue_gamma);
  gtk_widget_set_tooltip_text(g->hue_gamma, _("select the hue"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->hue_gamma, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->hue_gamma), "value-changed", G_CALLBACK(hue_gamma_callback), self);
#ifdef AUTO
  dt_bauhaus_widget_set_quad_paint(g->hue_gamma, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);
  g_signal_connect(G_OBJECT(g->hue_gamma), "quad-pressed", G_CALLBACK(gamma_neutralize_callback), self);
#endif
  g->sat_gamma = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 0.25f, 0.0005f, 0.0f, 4, 0);
  dt_bauhaus_slider_enable_soft_boundaries(g->sat_gamma, 0.0f, 1.0f);
  dt_bauhaus_widget_set_label(g->sat_gamma, NULL, _("saturation"));
  dt_bauhaus_slider_set_stop(g->sat_gamma, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(g->sat_gamma, 1.0f, 1.0f, 1.0f, 1.0f);
  gtk_widget_set_tooltip_text(g->sat_gamma, _("select the saturation"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->sat_gamma, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->sat_gamma), "value-changed", G_CALLBACK(sat_gamma_callback), self);

  static const char *gamma_red_messages[] = { N_("factor of red for gamma") };
  (void)gamma_red_messages;
  ADD_CHANNEL(gamma, r, red, RED)
  dt_bauhaus_slider_set_stop(g->gamma_r, 0.0, 0.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gamma_r, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gamma_r, 1.0, 1.0, 0.0, 0.0);

  static const char *gamma_green_messages[] = { N_("factor of green for gamma") };
  (void)gamma_green_messages;
  ADD_CHANNEL(gamma, g, green, GREEN)
  dt_bauhaus_slider_set_stop(g->gamma_g, 0.0, 1.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gamma_g, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gamma_g, 1.0, 0.0, 1.0, 0.0);

  static const char *gamma_blue_messages[] = { N_("factor of blue for gamma") };
  (void)gamma_blue_messages;
  ADD_CHANNEL(gamma, b, blue, BLUE)
  dt_bauhaus_slider_set_stop(g->gamma_b, 0.0, 1.0, 1.0, 0.0);
  dt_bauhaus_slider_set_stop(g->gamma_b, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gamma_b, 1.0, 0.0, 0.0, 1.0);

  /* gain */
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("highlights : gain / slope")), FALSE, FALSE, 5);

  static const char *gain_messages[] = { N_("factor of gain"), N_("gain") };
  (void)gain_messages;
  ADD_FACTOR(gain)

  g->hue_gain = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 1.0f, 0.0005f, 0.0f, 4, 0);
  dt_bauhaus_widget_set_label(g->hue_gain, NULL, _("hue"));
  draw_hue_slider(g->hue_gain);
  gtk_widget_set_tooltip_text(g->hue_gain, _("select the hue"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->hue_gain, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->hue_gain), "value-changed", G_CALLBACK(hue_gain_callback), self);
#ifdef AUTO
  dt_bauhaus_widget_set_quad_paint(g->hue_gain, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);
  g_signal_connect(G_OBJECT(g->hue_gain), "quad-pressed", G_CALLBACK(gain_neutralize_callback), self);
#endif
  g->sat_gain = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 0.25f, 0.0005f, 0.0f, 4, 0);
  dt_bauhaus_slider_enable_soft_boundaries(g->sat_gain, 0.0f, 1.0f);
  dt_bauhaus_widget_set_label(g->sat_gain, NULL, _("saturation"));
  dt_bauhaus_slider_set_stop(g->sat_gain, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(g->sat_gain, 1.0f, 1.0f, 1.0f, 1.0f);
  gtk_widget_set_tooltip_text(g->sat_gain, _("select the saturation"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->sat_gain, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->sat_gain), "value-changed", G_CALLBACK(sat_gain_callback), self);

  static const char *gain_red_messages[] = { N_("factor of red for gain") };
  (void)gain_red_messages;
  ADD_CHANNEL(gain, r, red, RED)
  dt_bauhaus_slider_set_stop(g->gain_r, 0.0, 0.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gain_r, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gain_r, 1.0, 1.0, 0.0, 0.0);

  static const char *gain_green_messages[] = { N_("factor of green for gain") };
  (void)gain_green_messages;
  ADD_CHANNEL(gain, g, green, GREEN)
  dt_bauhaus_slider_set_stop(g->gain_g, 0.0, 1.0, 0.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gain_g, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gain_g, 1.0, 0.0, 1.0, 0.0);

  static const char *gain_blue_messages[] = { N_("factor of blue for gain") };
  (void)gain_blue_messages;
  ADD_CHANNEL(gain, b, blue, BLUE)
  dt_bauhaus_slider_set_stop(g->gain_b, 0.0, 1.0, 1.0, 0.0);
  dt_bauhaus_slider_set_stop(g->gain_b, 0.5, 1.0, 1.0, 1.0);
  dt_bauhaus_slider_set_stop(g->gain_b, 1.0, 0.0, 0.0, 1.0);

#ifdef AUTO
  g->optim_label =  dt_ui_section_label_new(_("auto optimizers"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->optim_label, FALSE, FALSE, 5);

  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);

  g->auto_luma = gtk_button_new_with_label(_("optimize luma"));
  gtk_widget_set_tooltip_text(g->auto_luma, _("fit the whole histogram and center the average luma"));
  gtk_box_pack_start(GTK_BOX(box), g->auto_luma, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->auto_luma), "clicked", G_CALLBACK(optimize_luma_pressed_callback), self);

  g->auto_color = gtk_button_new_with_label(_("neutralize colors"));
  gtk_widget_set_tooltip_text(g->auto_color, _("optimize the RGB curves to remove color casts"));
  gtk_box_pack_start(GTK_BOX(box), g->auto_color, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->auto_color), "clicked", G_CALLBACK(optimize_color_pressed_callback), self);
#endif

#ifdef CONTROLS
  // default control is HSL
  gtk_widget_set_visible(g->lift_r, FALSE);
  gtk_widget_set_visible(g->lift_g, FALSE);
  gtk_widget_set_visible(g->lift_b, FALSE);
  gtk_widget_set_visible(g->gamma_r, FALSE);
  gtk_widget_set_visible(g->gamma_g, FALSE);
  gtk_widget_set_visible(g->gamma_b, FALSE);
  gtk_widget_set_visible(g->gain_r, FALSE);
  gtk_widget_set_visible(g->gain_g, FALSE);
  gtk_widget_set_visible(g->gain_b, FALSE);

  gtk_widget_set_visible(g->hue_lift, TRUE);
  gtk_widget_set_visible(g->sat_lift, TRUE);
  gtk_widget_set_visible(g->hue_gamma, TRUE);
  gtk_widget_set_visible(g->sat_gamma, TRUE);
  gtk_widget_set_visible(g->hue_gain, TRUE);
  gtk_widget_set_visible(g->sat_gain, TRUE);
#endif
#undef ADD_FACTOR
#undef ADD_CHANNEL
}

void gui_cleanup(dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

/** additional, optional callbacks to capture darkroom center events. */
// int mouse_moved(dt_iop_module_t *self, double x, double y, double pressure, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
// uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
