/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Templated common implementation part of all CPU kernels.
 *
 * The idea is that particular .cpp files sets needed optimization flags and
 * simply includes this file without worry of copying actual implementation over.
 */

#include "kernel_compat_cpu.h"
#include "kernel_math.h"
#include "kernel_types.h"
#include "kernel_globals.h"
#include "kernel_cpu_image.h"
#include "kernel_film.h"
#include "kernel_path.h"
#include "kernel_path_branched.h"
#include "kernel_bake.h"
#include "kernel_filter.h"

CCL_NAMESPACE_BEGIN

/* Path Tracing */

void KERNEL_FUNCTION_FULL_NAME(path_trace)(KernelGlobals *kg,
                                           float *buffer,
                                           unsigned int *rng_state,
                                           int sample,
                                           int x, int y,
                                           int offset,
                                           int stride)
{
#ifdef __BRANCHED_PATH__
	if(kernel_data.integrator.branched) {
		kernel_branched_path_trace(kg,
		                           buffer,
		                           rng_state,
		                           sample,
		                           x, y,
		                           offset,
		                           stride);
	}
	else
#endif
	{
		kernel_path_trace(kg, buffer, rng_state, sample, x, y, offset, stride);
	}
}

/* Film */

void KERNEL_FUNCTION_FULL_NAME(convert_to_byte)(KernelGlobals *kg,
                                                uchar4 *rgba,
                                                float *buffer,
                                                float sample_scale,
                                                int x, int y,
                                                int offset,
                                                int stride)
{
	kernel_film_convert_to_byte(kg,
	                            rgba,
	                            buffer,
	                            sample_scale,
	                            x, y,
	                            offset,
	                            stride);
}

void KERNEL_FUNCTION_FULL_NAME(convert_to_half_float)(KernelGlobals *kg,
                                                      uchar4 *rgba,
                                                      float *buffer,
                                                      float sample_scale,
                                                      int x, int y,
                                                      int offset,
                                                      int stride)
{
	kernel_film_convert_to_half_float(kg,
	                                  rgba,
	                                  buffer,
	                                  sample_scale,
	                                  x, y,
	                                  offset,
	                                  stride);
}

/* Shader Evaluate */

void KERNEL_FUNCTION_FULL_NAME(shader)(KernelGlobals *kg,
                                       uint4 *input,
                                       float4 *output,
                                       float *output_luma,
                                       int type,
                                       int filter,
                                       int i,
                                       int offset,
                                       int sample)
{
	if(type >= SHADER_EVAL_BAKE) {
		kernel_assert(output_luma == NULL);
#ifdef __BAKING__
		kernel_bake_evaluate(kg,
		                     input,
		                     output,
		                     (ShaderEvalType)type,
		                     filter,
		                     i,
		                     offset,
		                     sample);
#endif
	}
	else {
		kernel_shader_evaluate(kg,
		                       input,
		                       output,
		                       output_luma,
		                       (ShaderEvalType)type,
		                       i,
		                       sample);
	}
}

/* Denoise filter */

void KERNEL_FUNCTION_FULL_NAME(filter_divide_shadow)(KernelGlobals *kg,
                                                     int sample,
                                                     float** buffers,
                                                     int x,
                                                     int y,
                                                     int *tile_x,
                                                     int *tile_y,
                                                     int *offset,
                                                     int *stride,
                                                     float *unfiltered, float *sampleVariance, float *sampleVarianceV, float *bufferVariance,
                                                     int4 prefilter_rect)
{
	kernel_filter_divide_shadow(kg, sample, buffers, x, y, tile_x, tile_y, offset, stride, unfiltered, sampleVariance, sampleVarianceV, bufferVariance, prefilter_rect);
}

void KERNEL_FUNCTION_FULL_NAME(filter_get_feature)(KernelGlobals *kg,
                                                   int sample,
                                                   float** buffers,
                                                   int m_offset,
                                                   int v_offset,
                                                   int x,
                                                   int y,
                                                   int *tile_x,
                                                   int *tile_y,
                                                   int *offset,
                                                   int *stride,
                                                   float *mean, float *variance,
                                                   int4 prefilter_rect)
{
	kernel_filter_get_feature(kg, sample, buffers, m_offset, v_offset, x, y, tile_x, tile_y, offset, stride, mean, variance, prefilter_rect);
}

void KERNEL_FUNCTION_FULL_NAME(filter_non_local_means)(int x, int y,
                                                       float *noisyImage,
                                                       float *weightImage,
                                                       float *variance,
                                                       float *filteredImage,
                                                       int4 rect,
                                                       int r, int f,
                                                       float a, float k_2)
{
	kernel_filter_non_local_means(x, y, noisyImage, weightImage, variance, filteredImage, rect, r, f, a, k_2);
}

void KERNEL_FUNCTION_FULL_NAME(filter_combine_halves)(int x, int y,
                                                      float *mean,
                                                      float *variance,
                                                      float *a,
                                                      float *b,
                                                      int4 prefilter_rect,
                                                      int r)
{
	kernel_filter_combine_halves(x, y, mean, variance, a, b, prefilter_rect, r);
}

void KERNEL_FUNCTION_FULL_NAME(filter_estimate_params)(KernelGlobals *kg,
                                                       int sample,
                                                       float* buffer,
                                                       int x,
                                                       int y,
                                                       void *storage,
                                                       int4 rect)
{
	kernel_filter_estimate_params(kg, sample, buffer, x, y, (FilterStorage*) storage, rect);
}

void KERNEL_FUNCTION_FULL_NAME(filter_final_pass)(KernelGlobals *kg,
                                                  int sample,
                                                  float* buffer,
                                                  int x,
                                                  int y,
                                                  int offset,
                                                  int stride,
                                                  float *buffers,
                                                  void *storage,
                                                  int4 filter_area,
                                                  int4 rect)
{
	kernel_filter_final_pass(kg, sample, buffer, x, y, offset, stride, buffers, (FilterStorage*) storage, filter_area, rect);
}

void KERNEL_FUNCTION_FULL_NAME(filter_old_1)(KernelGlobals *kg,
                                             float *denoise_data,
                                             int x, int y,
                                             int samples,
                                             int halfWindow,
                                             float bandwidthFactor,
                                             float *storage,
                                             int4 rect)
{
  kernel_filter1_pixel(kg, denoise_data, x, y, samples, halfWindow, bandwidthFactor, storage, rect);
}

void KERNEL_FUNCTION_FULL_NAME(filter_old_2)(KernelGlobals *kg,
                                             float* buffer,
                                             float *denoise_data,
                                             int x, int y,
                                             int offset, int stride,
                                             int samples,
                                             int halfWindow,
                                             float bandwidthFactor,
                                             float *storage,
                                             int4 rect,
                                             int4 tile)
{
  kernel_filter2_pixel(kg, buffer, denoise_data, x, y, offset, stride, samples, halfWindow, bandwidthFactor, storage, rect, tile);
}

CCL_NAMESPACE_END
