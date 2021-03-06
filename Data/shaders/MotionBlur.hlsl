/*
Copyright(c) 2016-2020 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//= INCLUDES ===========
#include "Common.hlsl"
#include "Velocity.hlsl"
//======================

static const uint g_motion_blur_samples = 16;

float4 mainPS(Pixel_PosUv input) : SV_TARGET
{
    float4 color    = tex.Sample(sampler_point_clamp, input.uv);
    float2 velocity = GetVelocity_Max(input.uv, tex_velocity, tex_depth);

    // Compute motion blur strength from camera's shutter speed
    float motion_blur_strength = saturate(g_camera_shutter_speed * 1.0f);
	
	// Scale with delta time
    motion_blur_strength /= g_delta_time + FLT_MIN;
	
	// Scale velocity
    velocity *= motion_blur_strength;
    
    // Early exit
    if (abs(velocity.x) + abs(velocity.y) < FLT_MIN)
        return color;
    
    [unroll]
    for (uint i = 1; i < g_motion_blur_samples; ++i)
    {
        float2 offset = velocity * (float(i) / float(g_motion_blur_samples - 1) - 0.5f);
        color += tex.SampleLevel(sampler_bilinear_clamp, input.uv + offset, 0);
    }

    return color / float(g_motion_blur_samples);
}