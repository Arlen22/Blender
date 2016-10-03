/*
 * Copyright 2011-2016 Blender Foundation
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

#include "util_math_matrix.h"

CCL_NAMESPACE_BEGIN

#define ccl_get_feature(pass) buffer[(pass)*pass_stride]

/* Loop over the pixels in the range [low.x, high.x) x [low.y, high.y).
 * pixel_buffer always points to the current pixel in the first pass. */
#ifdef DENOISE_TEMPORAL
#define FOR_PIXEL_WINDOW     pixel_buffer = buffer + (low.y - rect.y)*buffer_w + (low.x - rect.x); \
                             for(int t = 0; t < num_frames; t++) { \
                                 int pt = (t == 0)? 0: ((t <= prev_frames)? (t-prev_frames-1): (t - prev_frames)); \
	                             for(int py = low.y; py < high.y; py++) { \
	                                 for(int px = low.x; px < high.x; px++, pixel_buffer++) {

#define END_FOR_PIXEL_WINDOW         } \
                                     pixel_buffer += buffer_w - (high.x - low.x); \
                                 } \
                                 pixel_buffer += buffer_w * (buffer_h - (high.y - low.y)); \
                             }
#else
#define FOR_PIXEL_WINDOW     pixel_buffer = buffer + (low.y - rect.y)*buffer_w + (low.x - rect.x); \
                             for(int py = low.y; py < high.y; py++) { \
                                 for(int px = low.x; px < high.x; px++, pixel_buffer++) {

#define END_FOR_PIXEL_WINDOW     } \
                                 pixel_buffer += buffer_w - (high.x - low.x); \
                             }
#endif

ccl_device_inline void filter_get_features(int x, int y, int t, float ccl_readonly_ptr buffer, float *features, float *mean, int pass_stride)
{
	float *feature = features;
	*(feature++) = x;
	*(feature++) = y;
#ifdef DENOISE_TEMPORAL
	*(feature++) = t;
#endif
	*(feature++) = ccl_get_feature(6);
	*(feature++) = ccl_get_feature(0);
	*(feature++) = ccl_get_feature(2);
	*(feature++) = ccl_get_feature(4);
	*(feature++) = ccl_get_feature(8);
	*(feature++) = ccl_get_feature(10);
	*(feature++) = ccl_get_feature(12);
	*(feature++) = ccl_get_feature(14);
	if(mean) {
		for(int i = 0; i < DENOISE_FEATURES; i++)
			features[i] -= mean[i];
	}
#ifdef DENOISE_SECOND_ORDER_SCREEN
	features[10] = features[0]*features[0];
	features[11] = features[1]*features[1];
	features[12] = features[0]*features[1];
#endif
}

ccl_device_inline void filter_get_feature_variance(int x, int y, float ccl_readonly_ptr buffer, float *features, float *scale, int pass_stride)
{
	float *feature = features;
	*(feature++) = 0.0f;
	*(feature++) = 0.0f;
#ifdef DENOISE_TEMPORAL
	*(feature++) = 0.0f;
#endif
	*(feature++) = ccl_get_feature(7);
	*(feature++) = ccl_get_feature(1);
	*(feature++) = ccl_get_feature(3);
	*(feature++) = ccl_get_feature(5);
	*(feature++) = 0.0f;//ccl_get_feature(9);
	*(feature++) = ccl_get_feature(11);
	*(feature++) = ccl_get_feature(13);
	*(feature++) = ccl_get_feature(15);
#ifdef DENOISE_SECOND_ORDER_SCREEN
	features[10] = 0.0f;
	features[11] = 0.0f;
	features[12] = 0.0f;
#endif
	for(int i = 0; i < DENOISE_FEATURES; i++)
		features[i] *= scale[i]*scale[i];
}

ccl_device_inline void filter_get_feature_scales(int x, int y, int t, float ccl_readonly_ptr buffer, float *scales, float *mean, int pass_stride)
{
	*(scales++) = fabsf(x - *(mean++)); //X
	*(scales++) = fabsf(y - *(mean++)); //Y
#ifdef DENOISE_TEMPORAL
	*(scales++) = fabsf(t - *(mean++)); //T
#endif

	*(scales++) = fabsf(ccl_get_feature(6) - *(mean++)); //Depth

	float normalS = len_squared(make_float3(ccl_get_feature(0) - mean[0], ccl_get_feature(2) - mean[1], ccl_get_feature(4) - mean[2]));
	mean += 3;
	*(scales++) = normalS; //NormalX
	*(scales++) = normalS; //NormalY
	*(scales++) = normalS; //NormalZ

	*(scales++) = fabsf(ccl_get_feature(8) - *(mean++)); //Shadow

	float normalT = len_squared(make_float3(ccl_get_feature(10) - mean[0], ccl_get_feature(12) - mean[1], ccl_get_feature(14) - mean[2]));
	mean += 3;
	*(scales++) = normalT; //AlbedoR
	*(scales++) = normalT; //AlbedoG
	*(scales++) = normalT; //AlbedoB
}

ccl_device_inline void filter_calculate_scale(float *scale)
{
	scale[0] = 1.0f/max(scale[0], 0.01f); //X
	scale[1] = 1.0f/max(scale[1], 0.01f); //Y
	scale += 2;
#ifdef DENOISE_TEMPORAL
	scale[0] = 1.0f/max(scale[0], 0.01f); //T
	scale++;
#endif
	
	scale[0] = 1.0f/max(scale[0], 0.01f); //Depth

	scale[1] = 1.0f/max(sqrtf(scale[1]), 0.01f); //NormalX
	scale[2] = 1.0f/max(sqrtf(scale[2]), 0.01f); //NormalY
	scale[3] = 1.0f/max(sqrtf(scale[3]), 0.01f); //NormalZ
	
	scale[4] = 1.0f/max(scale[4], 0.01f); //Shadow

	scale[5] = 1.0f/max(sqrtf(scale[5]), 0.01f); //AlbedoR
	scale[6] = 1.0f/max(sqrtf(scale[6]), 0.01f); //AlbedoG
	scale[7] = 1.0f/max(sqrtf(scale[7]), 0.01f); //AlbedoB
}

ccl_device_inline float3 filter_get_pixel_color(float ccl_readonly_ptr buffer, int pass_stride)
{
	return make_float3(ccl_get_feature(16), ccl_get_feature(18), ccl_get_feature(20));
}

ccl_device_inline float filter_get_pixel_variance(float ccl_readonly_ptr buffer, int pass_stride)
{
	return average(make_float3(ccl_get_feature(17), ccl_get_feature(19), ccl_get_feature(21)));
}

ccl_device_inline float filter_fill_design_row(float *features, int rank, float *design_row, float *feature_transform, float *bandwidth_factor)
{
	design_row[0] = 1.0f;
	float weight = 1.0f;
	for(int d = 0; d < rank; d++) {
		float x = math_dot(features, feature_transform + d*DENOISE_FEATURES, DENOISE_FEATURES);
		float x2 = x*x;
		if(bandwidth_factor) x2 *= bandwidth_factor[d]*bandwidth_factor[d];
		if(x2 < 1.0f) {
			/* Pixels are weighted by Epanechnikov kernels. */
			weight *= 0.75f * (1.0f - x2);
		}
		else {
			weight = 0.0f;
			break;
		}
		design_row[1+d] = x;
		if(!bandwidth_factor) design_row[1+rank+d] = x2;
	}
	return weight;
}

ccl_device_inline bool filter_firefly_rejection(float3 pixel_color, float pixel_variance, float3 center_color, float sqrt_center_variance)
{
	float color_diff = average(fabs(pixel_color - center_color));
	float variance = sqrt_center_variance + sqrtf(pixel_variance) + 0.005f;
	return (color_diff > 3.0f*variance);
}


#ifdef __KERNEL_SSE3__

#define ccl_get_feature_sse(pass) _mm_loadu_ps(buffer + (pass)*pass_stride)

/* Loop over the pixels in the range [low.x, high.x) x [low.y, high.y), 4 at a time.
 * pixel_buffer always points to the first of the 4 current pixel in the first pass.
 * x4 and y4 contain the coordinates of the four pixels, active_pixels contains a mask that's set for all pixels within the window. */

#ifdef DENOISE_TEMPORAL
#define FOR_PIXEL_WINDOW_SSE pixel_buffer = buffer + (low.y - rect.y)*buffer_w + (low.x - rect.x); \
                             for(int t = 0; t < num_frames; t++) { \
                                 __m128 t4 = _mm_set1_ps((t == 0)? 0: ((t <= prev_frames)? (t-prev_frames-1): (t - prev_frames))); \
                                 for(int py = low.y; py < high.y; py++) { \
                                     __m128 y4 = _mm_set1_ps(py); \
                                     int px; \
                                     for(px = low.x; px < high.x; px += 4, pixel_buffer += 4) { \
                                         __m128 x4 = _mm_add_ps(_mm_set1_ps(px), _mm_set_ps(3.0f, 2.0f, 1.0f, 0.0f)); \
                                         __m128 active_pixels = _mm_cmplt_ps(x4, _mm_set1_ps(high.x));

#define END_FOR_PIXEL_WINDOW_SSE     } \
                                     pixel_buffer += buffer_w - (px - low.x); \
                                 } \
                                 pixel_buffer += buffer_w * (buffer_h - (high.y - low.y)); \
                             }
#else
#define FOR_PIXEL_WINDOW_SSE     pixel_buffer = buffer + (low.y - rect.y)*buffer_w + (low.x - rect.x); \
                                 for(int py = low.y; py < high.y; py++) { \
                                     __m128 y4 = _mm_set1_ps(py); \
                                     int px; \
                                     for(px = low.x; px < high.x; px += 4, pixel_buffer += 4) { \
                                         __m128 x4 = _mm_add_ps(_mm_set1_ps(px), _mm_set_ps(3.0f, 2.0f, 1.0f, 0.0f)); \
                                         __m128 active_pixels = _mm_cmplt_ps(x4, _mm_set1_ps(high.x));

#define END_FOR_PIXEL_WINDOW_SSE     } \
                                     pixel_buffer += buffer_w - (px - low.x); \
                                 }
#endif

ccl_device_inline void filter_get_features_sse(__m128 x, __m128 y, __m128 t, __m128 active_pixels, float ccl_readonly_ptr buffer, __m128 *features, __m128 *mean, int pass_stride)
{
	__m128 *feature = features;
	*(feature++) = x;
	*(feature++) = y;
#ifdef DENOISE_TEMPORAL
	*(feature++) = t;
#endif
	*(feature++) = ccl_get_feature_sse(6);
	*(feature++) = ccl_get_feature_sse(0);
	*(feature++) = ccl_get_feature_sse(2);
	*(feature++) = ccl_get_feature_sse(4);
	*(feature++) = ccl_get_feature_sse(8);
	*(feature++) = ccl_get_feature_sse(10);
	*(feature++) = ccl_get_feature_sse(12);
	*(feature++) = ccl_get_feature_sse(14);
	if(mean) {
		for(int i = 0; i < DENOISE_FEATURES; i++)
			features[i] = _mm_mask_ps(_mm_sub_ps(features[i], mean[i]), active_pixels);
	}
	else {
		for(int i = 0; i < DENOISE_FEATURES; i++)
			features[i] = _mm_mask_ps(features[i], active_pixels);
	}
#ifdef DENOISE_SECOND_ORDER_SCREEN
	features[10] = _mm_mul_ps(features[0], features[0]);
	features[11] = _mm_mul_ps(features[1], features[1]);
	features[12] = _mm_mul_ps(features[0], features[1]);
#endif
}

ccl_device_inline void filter_get_feature_scales_sse(__m128 x, __m128 y, __m128 t, __m128 active_pixels, float ccl_readonly_ptr buffer, __m128 *scales, __m128 *mean, int pass_stride)
{
	*(scales++) = _mm_mask_ps(_mm_fabs_ps(_mm_sub_ps(x, *(mean++))), active_pixels); //X
	*(scales++) = _mm_mask_ps(_mm_fabs_ps(_mm_sub_ps(y, *(mean++))), active_pixels); //Y
#ifdef DENOISE_TEMPORAL
	*(scales++) = _mm_mask_ps(_mm_fabs_ps(_mm_sub_ps(t, *(mean++))), active_pixels); //T
#endif

	*(scales++) = _mm_mask_ps(_mm_fabs_ps(_mm_sub_ps(ccl_get_feature_sse(6), *(mean++))), active_pixels); //Depth

	__m128 diff = _mm_sub_ps(ccl_get_feature_sse(0), mean[0]);
	__m128 scale3 = _mm_mul_ps(diff, diff);
	diff = _mm_sub_ps(ccl_get_feature_sse(2), mean[1]);
	scale3 = _mm_add_ps(scale3, _mm_mul_ps(diff, diff));
	diff = _mm_sub_ps(ccl_get_feature_sse(4), mean[2]);
	scale3 = _mm_add_ps(scale3, _mm_mul_ps(diff, diff));
	mean += 3;
	*(scales++) = _mm_mask_ps(scale3, active_pixels); //NormalX
	*(scales++) = _mm_mask_ps(scale3, active_pixels); //NormalY
	*(scales++) = _mm_mask_ps(scale3, active_pixels); //NormalZ

	*(scales++) = _mm_mask_ps(_mm_fabs_ps(_mm_sub_ps(ccl_get_feature_sse(8), *(mean++))), active_pixels); //Shadow

	diff = _mm_sub_ps(ccl_get_feature_sse(10), mean[0]);
	scale3 = _mm_mul_ps(diff, diff);
	diff = _mm_sub_ps(ccl_get_feature_sse(12), mean[1]);
	scale3 = _mm_add_ps(scale3, _mm_mul_ps(diff, diff));
	diff = _mm_sub_ps(ccl_get_feature_sse(14), mean[2]);
	scale3 = _mm_add_ps(scale3, _mm_mul_ps(diff, diff));
	mean += 3;
	*(scales++) = _mm_mask_ps(scale3, active_pixels); //AlbedoR
	*(scales++) = _mm_mask_ps(scale3, active_pixels); //AlbedoG
	*(scales++) = _mm_mask_ps(scale3, active_pixels); //AlbedoB
}

ccl_device_inline void filter_calculate_scale_sse(__m128 *scale)
{
	scale[0] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(scale[0]), _mm_set1_ps(0.01f))); //X
	scale[1] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(scale[1]), _mm_set1_ps(0.01f))); //Y
	scale += 2;
#ifdef DENOISE_TEMPORAL
	scale[0] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(scale[0]), _mm_set1_ps(0.01f))); //T
	scale++;
#endif
	
	scale[0] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(scale[0]), _mm_set1_ps(0.01f))); //Depth

	scale[1] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(_mm_sqrt_ps(scale[1])), _mm_set1_ps(0.01f))); //NormalX
	scale[2] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(_mm_sqrt_ps(scale[2])), _mm_set1_ps(0.01f))); //NormalY
	scale[3] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(_mm_sqrt_ps(scale[3])), _mm_set1_ps(0.01f))); //NormalZ
	
	scale[4] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(scale[4]), _mm_set1_ps(0.01f))); //Shadow

	scale[5] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(_mm_sqrt_ps(scale[5])), _mm_set1_ps(0.01f))); //AlbedoR
	scale[6] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(_mm_sqrt_ps(scale[6])), _mm_set1_ps(0.01f))); //AlbedoG
	scale[7] = _mm_rcp_ps(_mm_max_ps(_mm_hmax_ps(_mm_sqrt_ps(scale[7])), _mm_set1_ps(0.01f))); //AlbedoB
}

ccl_device_inline void filter_get_feature_variance_sse(__m128 x, __m128 y, __m128 active_pixels, float ccl_readonly_ptr buffer, __m128 *features, __m128 *scale, int pass_stride)
{
	__m128 *feature = features;
	*(feature++) = _mm_setzero_ps();
	*(feature++) = _mm_setzero_ps();
#ifdef DENOISE_TEMPORAL
	*(feature++) = _mm_setzero_ps();
#endif
	*(feature++) = _mm_mask_ps(ccl_get_feature_sse( 7), active_pixels);
	*(feature++) = _mm_mask_ps(ccl_get_feature_sse( 1), active_pixels);
	*(feature++) = _mm_mask_ps(ccl_get_feature_sse( 3), active_pixels);
	*(feature++) = _mm_mask_ps(ccl_get_feature_sse( 5), active_pixels);
	*(feature++) = _mm_setzero_ps();//_mm_mask_ps(ccl_get_feature_sse( 9), active_pixels);
	*(feature++) = _mm_mask_ps(ccl_get_feature_sse(11), active_pixels);
	*(feature++) = _mm_mask_ps(ccl_get_feature_sse(13), active_pixels);
	*(feature++) = _mm_mask_ps(ccl_get_feature_sse(15), active_pixels);
#ifdef DENOISE_SECOND_ORDER_SCREEN
	features[10] = _mm_setzero_ps();
	features[11] = _mm_setzero_ps();
	features[12] = _mm_setzero_ps();
#endif
	for(int i = 0; i < DENOISE_FEATURES; i++)
		features[i] = _mm_mul_ps(features[i], _mm_mul_ps(scale[i], scale[i]));
}

ccl_device_inline void filter_get_pixel_color_sse(float ccl_readonly_ptr buffer, __m128 active_pixels, __m128 *color, int pass_stride)
{
	color[0] = _mm_mask_ps(ccl_get_feature_sse(16), active_pixels);
	color[1] = _mm_mask_ps(ccl_get_feature_sse(18), active_pixels);
	color[2] = _mm_mask_ps(ccl_get_feature_sse(20), active_pixels);
}

ccl_device_inline __m128 filter_get_pixel_variance_sse(float ccl_readonly_ptr buffer, __m128 active_pixels, int pass_stride)
{
	return _mm_mask_ps(_mm_mul_ps(_mm_set1_ps(1.0f/3.0f), _mm_add_ps(_mm_add_ps(ccl_get_feature_sse(17), ccl_get_feature_sse(19)), ccl_get_feature_sse(21))), active_pixels);
}

ccl_device_inline __m128 filter_fill_design_row_sse(__m128 *features, __m128 active_pixels, int rank, __m128 *design_row, __m128 *feature_transform, __m128 *bandwidth_factor)
{
	__m128 weight = _mm_mask_ps(_mm_set1_ps(1.0f), active_pixels);
	design_row[0] = weight;
	for(int d = 0; d < rank; d++) {
		__m128 x = math_dot_sse(features, feature_transform + d*DENOISE_FEATURES, DENOISE_FEATURES);
		__m128 x2 = _mm_mul_ps(x, x);
		if(bandwidth_factor) x2 = _mm_mul_ps(x2, _mm_mul_ps(bandwidth_factor[d], bandwidth_factor[d]));
		weight = _mm_mask_ps(_mm_mul_ps(weight, _mm_mul_ps(_mm_set1_ps(0.75f), _mm_sub_ps(_mm_set1_ps(1.0f), x2))), _mm_and_ps(_mm_cmplt_ps(x2, _mm_set1_ps(1.0f)), active_pixels));
		design_row[1+d] = x;
		if(!bandwidth_factor) design_row[1+rank+d] = x2;
	}
	return weight;
}

ccl_device_inline __m128 filter_firefly_rejection_sse(__m128 *pixel_color, __m128 pixel_variance, __m128 *center_color, __m128 sqrt_center_variance)
{
	__m128 color_diff = _mm_mul_ps(_mm_set1_ps(1.0f/9.0f), _mm_add_ps(_mm_add_ps(_mm_fabs_ps(_mm_sub_ps(pixel_color[0], center_color[0])), _mm_fabs_ps(_mm_sub_ps(pixel_color[1], center_color[1]))), _mm_fabs_ps(_mm_sub_ps(pixel_color[2], center_color[2]))));
	__m128 variance = _mm_add_ps(_mm_add_ps(sqrt_center_variance, _mm_sqrt_ps(pixel_variance)), _mm_set1_ps(0.005f));
	return _mm_cmple_ps(color_diff, variance);;
}
#endif

#ifdef __KERNEL_CUDA__
ccl_device_inline float filter_fill_design_row_cuda(float *features, int rank, float *design_row, float ccl_readonly_ptr feature_transform, int transform_stride, float *bandwidth_factor)
{
	design_row[0] = 1.0f;
	float weight = 1.0f;
	for(int d = 0; d < rank; d++) {
		float x = math_dot_cuda(features, feature_transform + d*DENOISE_FEATURES*transform_stride, transform_stride, DENOISE_FEATURES);
		float x2 = x*x;
		if(bandwidth_factor) x2 *= bandwidth_factor[d]*bandwidth_factor[d];
		if(x2 < 1.0f) {
			/* Pixels are weighted by Epanechnikov kernels. */
			weight *= 0.75f * (1.0f - x2);
		}
		else {
			weight = 0.0f;
			break;
		}
		design_row[1+d] = x;
		if(!bandwidth_factor) design_row[1+rank+d] = x2;
	}
	return weight;
}
#endif

CCL_NAMESPACE_END
