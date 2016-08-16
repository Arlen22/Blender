/*
 * Adapted from Open Shading Language with this license:
 *
 * Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
 * All Rights Reserved.
 *
 * Modifications Copyright 2011, Blender Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the name of Sony Pictures Imageworks nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __BSDF_DISNEY_SPECULAR_H__
#define __BSDF_DISNEY_SPECULAR_H__

CCL_NAMESPACE_BEGIN

typedef ccl_addr_space struct DisneySpecularBsdf {
	SHADER_CLOSURE_BASE;

	float specular, specularTint, roughness, metallic, anisotropic, alpha_x, alpha_y, rough_g;
	float3 N, T;
	float3 baseColor, cspec0;
} DisneySpecularBsdf;

ccl_device int bsdf_disney_specular_setup(DisneySpecularBsdf *bsdf)
{
	float m_cdlum = 0.3f * bsdf->baseColor.x + 0.6f * bsdf->baseColor.y + 0.1f * bsdf->baseColor.z; // luminance approx.

	float3 m_ctint = m_cdlum > 0.0f ? bsdf->baseColor / m_cdlum : make_float3(1.0f, 1.0f, 1.0f); // normalize lum. to isolate hue+sat

	float3 tmp_col = make_float3(1.0f, 1.0f, 1.0f) * (1.0f - bsdf->specularTint) + m_ctint * bsdf->specularTint; // lerp(make_float3(1.0f, 1.0f, 1.0f), m_ctint, sc->data2/*specularTint*/);
	bsdf->cspec0 = (bsdf->specular * 0.08f * tmp_col) * (1.0f - bsdf->metallic) + bsdf->baseColor * bsdf->metallic; // lerp(sc->data1/*specular*/ * 0.08f * tmp_col, sc->color0/*baseColor*/, sc->data0/*metallic*/);

	float aspect = safe_sqrtf(1.0f - bsdf->anisotropic * 0.9f);
	float r2 = sqr(bsdf->roughness);
	
	/* ax */
	bsdf->alpha_x = fmaxf(0.001f, r2 / aspect);

	/* ay */
	bsdf->alpha_y = fmaxf(0.001f, r2 * aspect);

	/* rough_g */
	bsdf->rough_g = sqr(bsdf->roughness * 0.5f + 0.5f);

    bsdf->type = CLOSURE_BSDF_DISNEY_SPECULAR_ID;
    return SD_BSDF|SD_BSDF_HAS_EVAL;
}

ccl_device float3 bsdf_disney_specular_eval_reflect(const ShaderClosure *sc, const float3 I,
    const float3 omega_in, float *pdf)
{
	const DisneySpecularBsdf *bsdf = (const DisneySpecularBsdf *)sc;

	float3 N = bsdf->N;

	if (fmaxf(bsdf->alpha_x, bsdf->alpha_y) <= 1e-4f)
		return make_float3(0.0f, 0.0f, 0.0f);

	float cosNO = dot(N, I);
	float cosNI = dot(N, omega_in);

	if (cosNI > 0 && cosNO > 0) {
		/* get half vector */
		float3 m = normalize(omega_in + I);
		float alpha2 = bsdf->alpha_x * bsdf->alpha_y;
		float D, G1o, G1i;

		if (bsdf->alpha_x == bsdf->alpha_y) {
			/* isotropic
			 * eq. 20: (F*G*D)/(4*in*on)
			 * eq. 33: first we calculate D(m) */
            float cosThetaM = dot(N, m);
            float cosThetaM2 = cosThetaM * cosThetaM;
            float cosThetaM4 = cosThetaM2 * cosThetaM2;
            float tanThetaM2 = (1 - cosThetaM2) / cosThetaM2;
            D = alpha2 / (M_PI_F * cosThetaM4 * (alpha2 + tanThetaM2) * (alpha2 + tanThetaM2));

			/* eq. 34: now calculate G1(i,m) and G1(o,m) */
            G1o = 2 / (1 + safe_sqrtf(1 + alpha2 * (1 - cosNO * cosNO) / (cosNO * cosNO)));
            G1i = 2 / (1 + safe_sqrtf(1 + alpha2 * (1 - cosNI * cosNI) / (cosNI * cosNI)));
		}
		else {
			/* anisotropic */
            float3 X, Y, Z = N;
            make_orthonormals_tangent(Z, bsdf->T, &X, &Y);

            // distribution
            float3 local_m = make_float3(dot(X, m), dot(Y, m), dot(Z, m));
            float slope_x = -local_m.x/(local_m.z*bsdf->alpha_x);
            float slope_y = -local_m.y/(local_m.z*bsdf->alpha_y);
            float slope_len = 1 + slope_x*slope_x + slope_y*slope_y;

            float cosThetaM = local_m.z;
            float cosThetaM2 = cosThetaM * cosThetaM;
            float cosThetaM4 = cosThetaM2 * cosThetaM2;

            D = 1 / ((slope_len * slope_len) * M_PI_F * alpha2 * cosThetaM4);

			/* G1(i,m) and G1(o,m) */
            float tanThetaO2 = (1 - cosNO * cosNO) / (cosNO * cosNO);
            float cosPhiO = dot(I, X);
            float sinPhiO = dot(I, Y);

            float alphaO2 = (cosPhiO*cosPhiO)*(bsdf->alpha_x*bsdf->alpha_x) + (sinPhiO*sinPhiO)*(bsdf->alpha_y*bsdf->alpha_y);
            alphaO2 /= cosPhiO*cosPhiO + sinPhiO*sinPhiO;

            G1o = 2 / (1 + safe_sqrtf(1 + alphaO2 * tanThetaO2));

            float tanThetaI2 = (1 - cosNI * cosNI) / (cosNI * cosNI);
            float cosPhiI = dot(omega_in, X);
            float sinPhiI = dot(omega_in, Y);

			float alphaI2 = (cosPhiI*cosPhiI)*(bsdf->alpha_x*bsdf->alpha_x) + (sinPhiI*sinPhiI)*(bsdf->alpha_y*bsdf->alpha_y);
            alphaI2 /= cosPhiI*cosPhiI + sinPhiI*sinPhiI;

            G1i = 2 / (1 + safe_sqrtf(1 + alphaI2 * tanThetaI2));
		}

		float G = G1o * G1i;

		/* eq. 20 */
		float common = D * 0.25f / cosNO;

        float FH = schlick_fresnel(dot(omega_in, m));
		float3 F = bsdf->cspec0 * (1.0f - FH) + make_float3(1.0f, 1.0f, 1.0f) * FH; // lerp(sc->custom_color0, make_float3(1.0f, 1.0f, 1.0f), FH);

		float3 out = F * G * common;

		/* eq. 2 in distribution of visible normals sampling
		 * pm = Dw = G1o * dot(m, I) * D / dot(N, I); */

		/* eq. 38 - but see also:
		 * eq. 17 in http://www.graphics.cornell.edu/~bjw/wardnotes.pdf
		 * pdf = pm * 0.25 / dot(m, I); */
		*pdf = G1o * common;

		return out;
	}

	return make_float3(0.0f, 0.0f, 0.0f);
}

ccl_device float3 bsdf_disney_specular_eval_transmit(const ShaderClosure *sc, const float3 I,
	const float3 omega_in, float *pdf)
{
    return make_float3(0.0f, 0.0f, 0.0f);
}

ccl_device int bsdf_disney_specular_sample(const ShaderClosure *sc,
    float3 Ng, float3 I, float3 dIdx, float3 dIdy, float randu, float randv,
    float3 *eval, float3 *omega_in, float3 *domega_in_dx,
    float3 *domega_in_dy, float *pdf)
{
	const DisneySpecularBsdf *bsdf = (const DisneySpecularBsdf *)sc;

	float3 N = bsdf->N;

	float cosNO = dot(N, I);
	if(cosNO > 0) {
		float3 X, Y, Z = N;

		if (bsdf->alpha_x == bsdf->alpha_y)
			make_orthonormals(Z, &X, &Y);
		else
			make_orthonormals_tangent(Z, bsdf->T, &X, &Y);

		/* importance sampling with distribution of visible normals. vectors are
		 * transformed to local space before and after */
		float3 local_I = make_float3(dot(X, I), dot(Y, I), cosNO);
		float3 local_m;
        float3 m;
		float G1o;

		local_m = importance_sample_microfacet_stretched(local_I, bsdf->alpha_x, bsdf->alpha_y,
			    randu, randv, false, &G1o);

		m = X*local_m.x + Y*local_m.y + Z*local_m.z;
		float cosThetaM = local_m.z;

		/* reflection or refraction? */
        float cosMO = dot(m, I);

        if(cosMO > 0) {
            /* eq. 39 - compute actual reflected direction */
            *omega_in = 2 * cosMO * m - I;

            if(dot(Ng, *omega_in) > 0) {
				if (fmaxf(bsdf->alpha_x, bsdf->alpha_y) <= 1e-4f) {
                    /* some high number for MIS */
                    *pdf = 1e6f;
                    *eval = make_float3(1e6f, 1e6f, 1e6f);
                }
                else {
                    /* microfacet normal is visible to this ray */
                    /* eq. 33 */
					float alpha2 = bsdf->alpha_x * bsdf->alpha_y;
                    float D, G1i;

					if (bsdf->alpha_x == bsdf->alpha_y) {
                        float cosThetaM2 = cosThetaM * cosThetaM;
                        float cosThetaM4 = cosThetaM2 * cosThetaM2;
                        float tanThetaM2 = 1/(cosThetaM2) - 1;
                        D = alpha2 / (M_PI_F * cosThetaM4 * (alpha2 + tanThetaM2) * (alpha2 + tanThetaM2));

                        /* eval BRDF*cosNI */
                        float cosNI = dot(N, *omega_in);

                        /* eq. 34: now calculate G1(i,m) */
                        G1i = 2 / (1 + safe_sqrtf(1 + alpha2 * (1 - cosNI * cosNI) / (cosNI * cosNI)));
                    }
                    else {
                        /* anisotropic distribution */
						float slope_x = -local_m.x / (local_m.z*bsdf->alpha_x);
						float slope_y = -local_m.y / (local_m.z*bsdf->alpha_y);
                        float slope_len = 1 + slope_x*slope_x + slope_y*slope_y;

                        float cosThetaM = local_m.z;
                        float cosThetaM2 = cosThetaM * cosThetaM;
                        float cosThetaM4 = cosThetaM2 * cosThetaM2;

                        D = 1 / ((slope_len * slope_len) * M_PI_F * alpha2 * cosThetaM4);

                        /* calculate G1(i,m) */
                        float cosNI = dot(N, *omega_in);

                        float tanThetaI2 = (1 - cosNI * cosNI) / (cosNI * cosNI);
                        float cosPhiI = dot(*omega_in, X);
                        float sinPhiI = dot(*omega_in, Y);

						float alphaI2 = (cosPhiI*cosPhiI)*(bsdf->alpha_x*bsdf->alpha_x) + (sinPhiI*sinPhiI)*(bsdf->alpha_y*bsdf->alpha_y);
                        alphaI2 /= cosPhiI*cosPhiI + sinPhiI*sinPhiI;

                        G1i = 2 / (1 + safe_sqrtf(1 + alphaI2 * tanThetaI2));
                    }

                    /* see eval function for derivation */
                    float common = (G1o * D) * 0.25f / cosNO;
                    *pdf = common;

					float FH = schlick_fresnel(dot(*omega_in, m));
					float3 F = bsdf->cspec0 * (1.0f - FH) + make_float3(1.0f, 1.0f, 1.0f) * FH; // lerp(sc->custom_color0, make_float3(1.0f, 1.0f, 1.0f), FH);

                    *eval = G1i * common * F;
                }

#ifdef __RAY_DIFFERENTIALS__
                *domega_in_dx = (2 * dot(m, dIdx)) * m - dIdx;
                *domega_in_dy = (2 * dot(m, dIdy)) * m - dIdy;
#endif
            }
        }
    }

	return LABEL_REFLECT|LABEL_GLOSSY;
}

CCL_NAMESPACE_END

#endif /* __BSDF_DISNEY_SPECULAR_H__ */

