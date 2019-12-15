/*
Copyright(c) 2016-2019 Panos Karabelas

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

//= INCLUDES ==============================
#include "Renderer.h"
#include "Material.h"
#include "Model.h"
#include "Font/Font.h"
#include "../Profiling/Profiler.h"
#include "../Resource/IResource.h"
#include "ShaderVariation.h"
#include "Gizmos/Grid.h"
#include "Gizmos/Transform_Gizmo.h"
#include "../RHI/RHI_VertexBuffer.h"
#include "../RHI/RHI_ConstantBuffer.h"
#include "../RHI/RHI_Texture.h"
#include "../RHI/RHI_Sampler.h"
#include "../RHI/RHI_CommandList.h"
#include "../World/Entity.h"
#include "../World/Components/Renderable.h"
#include "../World/Components/Transform.h"
#include "../World/Components/Environment.h"
#include "../World/Components/Light.h"
#include "../World/Components/Camera.h"
//=========================================

//= NAMESPACES ===============
using namespace std;
using namespace Spartan::Math;
//============================

namespace Spartan
{
    void Renderer::SetGlobalSamplersAndConstantBuffers(RHI_CommandList* cmd_list)
    {
        // Set the buffers we will be using thought the frame
        cmd_list->SetConstantBuffer(0, RHI_Buffer_VertexShader | RHI_Buffer_PixelShader, m_buffer_frame_gpu);
        cmd_list->SetConstantBuffer(1, RHI_Buffer_VertexShader | RHI_Buffer_PixelShader, m_buffer_uber_gpu);
        cmd_list->SetConstantBuffer(2, RHI_Buffer_PixelShader, m_buffer_light_gpu);
        
        // Set the samplers we will be using thought the frame
        cmd_list->SetSampler(0, m_sampler_compare_depth);
        cmd_list->SetSampler(1, m_sampler_point_clamp);
        cmd_list->SetSampler(2, m_sampler_bilinear_clamp);
        cmd_list->SetSampler(3, m_sampler_bilinear_wrap);
        cmd_list->SetSampler(4, m_sampler_trilinear_clamp);
        cmd_list->SetSampler(5, m_sampler_anisotropic_wrap);
    }

    void Renderer::Pass_Main(RHI_CommandList* cmd_list)
	{
        // Validate RHI device as it's required almost everywhere
        if (!m_rhi_device)
            return;

        if (cmd_list->Begin("Pass_Main", RHI_Cmd_Marker))
        {
            // Update the frame buffer
            if (cmd_list->Begin("UpdateFrameBuffer", RHI_Cmd_Marker))
            {
                UpdateFrameBuffer();
                cmd_list->End();
            }

            if (!m_brdf_specular_lut_rendered)
            {
                Pass_BrdfSpecularLut(cmd_list);
                m_brdf_specular_lut_rendered = true;
            }

#ifdef API_GRAPHICS_D3D11
            Pass_LightDepth(cmd_list);
            if (GetOptionValue(Render_DepthPrepass))
            {
                Pass_DepthPrePass(cmd_list);
            }
            Pass_GBuffer(cmd_list);
            Pass_Ssao(cmd_list);
            Pass_Ssr(cmd_list);
            Pass_Light(cmd_list);
            Pass_Composition(cmd_list);
            Pass_PostProcess(cmd_list);
#endif
            Pass_Lines(cmd_list, m_render_targets[RenderTarget_Composition_Ldr]);
            Pass_Gizmos(cmd_list, m_render_targets[RenderTarget_Composition_Ldr]);
            Pass_DebugBuffer(cmd_list, m_render_targets[RenderTarget_Composition_Ldr]);
            Pass_PerformanceMetrics(cmd_list, m_render_targets[RenderTarget_Composition_Ldr]);

            cmd_list->End();
        }
	}

	void Renderer::Pass_LightDepth(RHI_CommandList* cmd_list)
	{
        // Description: All the opaque meshes are rendered (from the lights point of view),
        // outputting just their depth information into a depth map.

		// Acquire shader
		const auto& shader_depth = m_shaders[Shader_Depth_V];
		if (!shader_depth->IsCompiled())
			return;

        // Get opaque entities
        const auto& entities_opaque = m_entities[Renderer_Object_Opaque];
        if (entities_opaque.empty())
            return;

        // Get light entities
		const auto& entities_light = m_entities[Renderer_Object_Light];

        cmd_list->Begin("Pass_LightDepth");

        for (uint32_t light_index = 0; light_index < entities_light.size(); light_index++)
        {
			const Light* light = entities_light[light_index]->GetComponent<Light>().get();

            // Light can be null if it just got removed and our buffer doesn't update till the next frame
            if (!light)
                break;

			// Acquire light's shadow map
			const auto& shadow_map = light->GetShadowMap();
			if (!shadow_map)
				continue;

            // Begin command list
            cmd_list->Begin("Light");	
			cmd_list->SetBlendState(m_blend_disabled);
			cmd_list->SetDepthStencilState(m_depth_stencil_enabled_write);
			cmd_list->SetPrimitiveTopology(RHI_PrimitiveTopology_TriangleList);
            cmd_list->SetShaderPixel(nullptr);
			cmd_list->SetShaderVertex(shader_depth);
			cmd_list->SetInputLayout(shader_depth->GetInputLayout());
			cmd_list->SetViewport(shadow_map->GetViewport());

            // Set appropriate rasterizer state
            if (light->GetLightType() == LightType_Directional)
            {
                // "Pancaking" - https://www.gamedev.net/forums/topic/639036-shadow-mapping-and-high-up-objects/
                // It's basically a way to capture the silhouettes of potential shadow casters behind the light's view point.
                // Of course we also have to make sure that the light doesn't cull them in the first place (this is done automatically by the light)
                cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid_no_clip);
            }
            else
            {
                cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
            }

			// Tracking
			uint32_t currently_bound_geometry = 0;

			for (uint32_t i = 0; i < shadow_map->GetArraySize(); i++)
			{
				const auto cascade_depth_stencil    = shadow_map->GetResource_DepthStencil(i);
                const Matrix& view_projection       = light->GetViewMatrix(i) * light->GetProjectionMatrix(i);

				cmd_list->Begin("Array_" + to_string(i + 1));
                cmd_list->SetRenderTarget(nullptr, cascade_depth_stencil);
				cmd_list->ClearDepthStencil(cascade_depth_stencil, RHI_Clear_Depth, GetClearDepth());

                // Skip if it doesn't need to cast shadows
                if (!light->GetCastShadows())
                {
                    cmd_list->End(); // end of array
                    continue;
                }

				for (const auto& entity : entities_opaque)
				{
					// Acquire renderable component
					const auto& renderable = entity->GetRenderable_PtrRaw();
					if (!renderable)
						continue;

                    // Skip objects outside of the view frustum
                    if (!light->IsInViewFrustrum(renderable, i))
                        continue;

					// Acquire material
					const auto& material = renderable->GetMaterial();
					if (!material)
						continue;

					// Acquire geometry
					const auto& model = renderable->GeometryModel();
					if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
						continue;

					// Skip meshes that don't cast shadows
					if (!renderable->GetCastShadows())
						continue;

					// Skip transparent meshes (for now)
					if (material->GetColorAlbedo().w < 1.0f)
						continue;

					// Bind geometry
					if (currently_bound_geometry != model->GetId())
					{
						cmd_list->SetBufferIndex(model->GetIndexBuffer());
						cmd_list->SetBufferVertex(model->GetVertexBuffer());
						currently_bound_geometry = model->GetId();
					}

                    // Update uber buffer with cascade transform
                    m_buffer_uber_cpu.transform = entity->GetTransform_PtrRaw()->GetMatrix() * view_projection;
                    UpdateUberBuffer(); // only updates if needed

					cmd_list->DrawIndexed(renderable->GeometryIndexCount(), renderable->GeometryIndexOffset(), renderable->GeometryVertexOffset());
                    cmd_list->Submit();
				}
				cmd_list->End(); // end of array
			}
            cmd_list->End(); // end light
		}

        cmd_list->End(); // end lights
        cmd_list->Submit();
	}

    void Renderer::Pass_DepthPrePass(RHI_CommandList* cmd_list)
    {
        // Description: All the opaque meshes are rendered, outputting
        // just their depth information into a depth map.

        // Acquire required resources/data
        const auto& shader_depth    = m_shaders[Shader_Depth_V];
        const auto& tex_depth       = m_render_targets[RenderTarget_Gbuffer_Depth];
        const auto& entities        = m_entities[Renderer_Object_Opaque];

        // Ensure the shader has compiled
        if (!shader_depth->IsCompiled())
            return;

        // Star command list
        cmd_list->Begin("Pass_DepthPrePass");
        cmd_list->ClearDepthStencil(tex_depth->GetResource_DepthStencil(), RHI_Clear_Depth, GetClearDepth());

        if (!entities.empty())
        {
            cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
            cmd_list->SetBlendState(m_blend_disabled);
            cmd_list->SetDepthStencilState(m_depth_stencil_enabled_write);
            cmd_list->SetViewport(tex_depth->GetViewport());
            cmd_list->SetRenderTarget(nullptr, tex_depth->GetResource_DepthStencil());
            cmd_list->SetShaderVertex(shader_depth);
            cmd_list->SetShaderPixel(nullptr);
            cmd_list->SetPrimitiveTopology(RHI_PrimitiveTopology_TriangleList);
            cmd_list->SetInputLayout(shader_depth->GetInputLayout());

            // Variables that help reduce state changes
            uint32_t currently_bound_geometry = 0;

            // Draw opaque
            for (const auto& entity : entities)
            {
                // Get renderable
                const auto& renderable = entity->GetRenderable_PtrRaw();
                if (!renderable)
                    continue;

                // Get geometry
                const auto& model = renderable->GeometryModel();
                if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
                    continue;

                // Skip objects outside of the view frustum
                if (!m_camera->IsInViewFrustrum(renderable))
                    continue;

                // Bind geometry
                if (currently_bound_geometry != model->GetId())
                {
                    cmd_list->SetBufferIndex(model->GetIndexBuffer());
                    cmd_list->SetBufferVertex(model->GetVertexBuffer());
                    currently_bound_geometry = model->GetId();
                }

                // Update uber buffer with entity transform
                if (Transform* transform = entity->GetTransform_PtrRaw())
                {
                    // Update uber buffer with cascade transform
                    m_buffer_uber_cpu.transform = transform->GetMatrix() * m_buffer_frame_cpu.view_projection;
                    UpdateUberBuffer(); // only updates if needed
                }

                // Draw	
                cmd_list->DrawIndexed(renderable->GeometryIndexCount(), renderable->GeometryIndexOffset(), renderable->GeometryVertexOffset());
                cmd_list->Submit();
            }
        }

        cmd_list->End();
        cmd_list->Submit();
    }

	void Renderer::Pass_GBuffer(RHI_CommandList* cmd_list)
	{
        // Acquire required resources/shaders
        const auto& tex_albedo      = m_render_targets[RenderTarget_Gbuffer_Albedo];
        const auto& tex_normal      = m_render_targets[RenderTarget_Gbuffer_Normal];
        const auto& tex_material    = m_render_targets[RenderTarget_Gbuffer_Material];
        const auto& tex_velocity    = m_render_targets[RenderTarget_Gbuffer_Velocity];
        const auto& tex_depth       = m_render_targets[RenderTarget_Gbuffer_Depth];
        const auto& clear_color     = Vector4::Zero;
        const auto& shader_gbuffer  = m_shaders[Shader_Gbuffer_V];

        // Validate that the shader has compiled
        if (!shader_gbuffer->IsCompiled())
            return;

        // Pack render targets
        void* render_targets[]
        {
            tex_albedo->GetResource_RenderTarget(),
            tex_normal->GetResource_RenderTarget(),
            tex_material->GetResource_RenderTarget(),
            tex_velocity->GetResource_RenderTarget()
        };

        // Star command list
        cmd_list->Begin("Pass_GBuffer");
        cmd_list->ClearRenderTarget(tex_albedo->GetResource_RenderTarget(),   clear_color);
        cmd_list->ClearRenderTarget(tex_normal->GetResource_RenderTarget(),   clear_color);
        cmd_list->ClearRenderTarget(tex_material->GetResource_RenderTarget(), Vector4::Zero); // zeroed material buffer causes sky sphere to render
        cmd_list->ClearRenderTarget(tex_velocity->GetResource_RenderTarget(), clear_color);
        if (!GetOptionValue(Render_DepthPrepass))
        {
            cmd_list->ClearDepthStencil(tex_depth->GetResource_DepthStencil(), RHI_Clear_Depth, GetClearDepth());
        }

        if (!m_entities[Renderer_Object_Opaque].empty())
        {
            cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
            cmd_list->SetBlendState(m_blend_disabled);
            cmd_list->SetDepthStencilState(GetOptionValue(Render_DepthPrepass) ? m_depth_stencil_enabled_no_write : m_depth_stencil_enabled_write);
            cmd_list->SetPrimitiveTopology(RHI_PrimitiveTopology_TriangleList);
            cmd_list->SetViewport(tex_albedo->GetViewport());
            cmd_list->SetRenderTargets(render_targets, 4, tex_depth->GetResource_DepthStencil());
            cmd_list->SetShaderVertex(shader_gbuffer);
            cmd_list->SetInputLayout(shader_gbuffer->GetInputLayout());

            // Variables that help reduce state changes
            uint32_t currently_bound_geometry   = 0;
            uint32_t currently_bound_shader     = 0;
            uint32_t currently_bound_material   = 0;

            auto draw_entity = [this, &cmd_list, &currently_bound_geometry, &currently_bound_shader, &currently_bound_material](Entity* entity)
            {
                // Get renderable
                const auto& renderable = entity->GetRenderable_PtrRaw();
                if (!renderable)
                    return;

                // Get material
                const auto& material = renderable->GetMaterial();
                if (!material)
                    return;

                // Get shader
                const auto& shader = material->GetShader();
                if (!shader || !shader->IsCompiled())
                    return;

                // Get geometry
                const auto& model = renderable->GeometryModel();
                if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
                    return;

                // Skip objects outside of the view frustum
                if (!m_camera->IsInViewFrustrum(renderable))
                    return;

                // Set face culling (changes only if required)
                cmd_list->SetRasterizerState(GetRasterizerState(material->GetCullMode(), !GetOptionValue(Render_Debug_Wireframe) ? RHI_Fill_Solid : RHI_Fill_Wireframe));

                // Bind geometry
                if (currently_bound_geometry != model->GetId())
                {
                    cmd_list->SetBufferIndex(model->GetIndexBuffer());
                    cmd_list->SetBufferVertex(model->GetVertexBuffer());
                    currently_bound_geometry = model->GetId();
                }

                // Bind shader
                if (currently_bound_shader != shader->GetId())
                {
                    cmd_list->SetShaderPixel(static_pointer_cast<RHI_Shader>(shader));
                    currently_bound_shader = shader->GetId();
                }

                // Bind material
                if (currently_bound_material != material->GetId())
                {
                    // Bind material textures		
                    cmd_list->SetTexture(0, material->GetTexture(TextureType_Albedo).get());
                    cmd_list->SetTexture(1, material->GetTexture(TextureType_Roughness).get());
                    cmd_list->SetTexture(2, material->GetTexture(TextureType_Metallic).get());
                    cmd_list->SetTexture(3, material->GetTexture(TextureType_Normal).get());
                    cmd_list->SetTexture(4, material->GetTexture(TextureType_Height).get());
                    cmd_list->SetTexture(5, material->GetTexture(TextureType_Occlusion).get());
                    cmd_list->SetTexture(6, material->GetTexture(TextureType_Emission).get());
                    cmd_list->SetTexture(7, material->GetTexture(TextureType_Mask).get());

                    // Update uber buffer with material properties
                    m_buffer_uber_cpu.mat_albedo        = material->GetColorAlbedo();
                    m_buffer_uber_cpu.mat_tiling_uv     = material->GetTiling();
                    m_buffer_uber_cpu.mat_offset_uv     = material->GetOffset();
                    m_buffer_uber_cpu.mat_roughness_mul = material->GetMultiplier(TextureType_Roughness);
                    m_buffer_uber_cpu.mat_metallic_mul  = material->GetMultiplier(TextureType_Metallic);
                    m_buffer_uber_cpu.mat_normal_mul    = material->GetMultiplier(TextureType_Normal);
                    m_buffer_uber_cpu.mat_height_mul    = material->GetMultiplier(TextureType_Height);
                    m_buffer_uber_cpu.mat_shading_mode  = static_cast<float>(material->GetShadingMode());

                    currently_bound_material = material->GetId();
                }

                // Update uber buffer with entity transform
                if (Transform* transform = entity->GetTransform_PtrRaw())
                {
                    m_buffer_uber_cpu.transform     = transform->GetMatrix();
                    m_buffer_uber_cpu.wvp_current   = transform->GetMatrix() * m_buffer_frame_cpu.view_projection;
                    m_buffer_uber_cpu.wvp_previous  = transform->GetWvpLastFrame();
                    transform->SetWvpLastFrame(m_buffer_uber_cpu.wvp_current);
                }

                // Only happens if needed
                UpdateUberBuffer();

                // Render	
                cmd_list->DrawIndexed(renderable->GeometryIndexCount(), renderable->GeometryIndexOffset(), renderable->GeometryVertexOffset());
                m_profiler->m_renderer_meshes_rendered++;

                cmd_list->Submit();
            };

            // Draw opaque
            for (const auto& entity : m_entities[Renderer_Object_Opaque])
            {
                draw_entity(entity);
            }

            // Draw transparent (transparency of the poor)
            cmd_list->SetBlendState(m_blend_enabled);
            cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
            for (const auto& entity : m_entities[Renderer_Object_Transparent])
            {
                draw_entity(entity);
            }
        }

		cmd_list->End();
		cmd_list->Submit();
	}

	void Renderer::Pass_Ssao(RHI_CommandList* cmd_list)
	{
        // Acquire shaders
        const auto& shader_quad = m_shaders[Shader_Quad_V];
        const auto& shader_ssao = m_shaders[Shader_Ssao_P];
        if (!shader_quad->IsCompiled() || !shader_ssao->IsCompiled())
            return;

        // Acquire render targets
        auto& tex_ssao_raw      = m_render_targets[RenderTarget_Ssao_Raw];
        auto& tex_ssao_blurred  = m_render_targets[RenderTarget_Ssao_Blurred];
        auto& tex_ssao          = m_render_targets[RenderTarget_Ssao];

		cmd_list->Begin("Pass_Ssao");
		cmd_list->ClearRenderTarget(tex_ssao_raw->GetResource_RenderTarget(), Vector4::One);
        cmd_list->ClearRenderTarget(tex_ssao->GetResource_RenderTarget(),     Vector4::One);

		if (m_options & Render_SSAO)
		{
            // Update uber buffer
            m_buffer_uber_cpu.resolution = Vector2(tex_ssao_raw->GetWidth(), tex_ssao_raw->GetHeight());
            UpdateUberBuffer();

            cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from some previous pass)
            cmd_list->SetRenderTarget(tex_ssao_raw);
            cmd_list->SetTexture(0, m_render_targets[RenderTarget_Gbuffer_Depth]);
            cmd_list->SetTexture(1, m_render_targets[RenderTarget_Gbuffer_Normal]);
            cmd_list->SetTexture(2, m_tex_noise_normal);
            cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
            cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
            cmd_list->SetBlendState(m_blend_disabled);
            cmd_list->SetPrimitiveTopology(RHI_PrimitiveTopology_TriangleList);
            cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
            cmd_list->SetViewport(tex_ssao_raw->GetViewport());
            cmd_list->SetShaderVertex(shader_quad);
            cmd_list->SetInputLayout(shader_quad->GetInputLayout());
            cmd_list->SetShaderPixel(shader_ssao);
            cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
            cmd_list->Submit();

            // Bilateral blur
            const auto sigma = 2.0f;
            const auto pixel_stride = 2.0f;
            Pass_BlurBilateralGaussian(cmd_list, tex_ssao_raw, tex_ssao_blurred, sigma, pixel_stride);

            // Upscale to full size
            float ssao_scale = m_option_values[Option_Value_Ssao_Scale];
            if (ssao_scale < 1.0f)
            {
                Pass_Upsample(cmd_list, tex_ssao_blurred, tex_ssao);
            }
            else if (ssao_scale > 1.0f)
            {
                Pass_Downsample(cmd_list, tex_ssao_blurred, tex_ssao, Shader_Downsample_P);
            }
            else
            {
                tex_ssao_blurred.swap(tex_ssao);
            }
		}

		cmd_list->End();
        cmd_list->Submit();
	}

    void Renderer::Pass_Ssr(RHI_CommandList* cmd_list)
    {
        // Acquire shaders
        const auto& shader_quad = m_shaders[Shader_Quad_V];
        const auto& shader_ssr  = m_shaders[Shader_Ssr_P];
        if (!shader_quad->IsCompiled() || !shader_ssr->IsCompiled())
            return;

        // Acquire render targets
        auto& tex_ssr         = m_render_targets[RenderTarget_Ssr];
        auto& tex_ssr_blurred = m_render_targets[RenderTarget_Ssr_Blurred];

        cmd_list->Begin("Pass_Ssr");
        
        if (m_options & Render_SSR)
        {
            // Update uber buffer
            m_buffer_uber_cpu.resolution = Vector2(tex_ssr->GetWidth(), tex_ssr->GetHeight());
            UpdateUberBuffer();

            cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from some previous pass)
            cmd_list->SetTexture(0, m_render_targets[RenderTarget_Gbuffer_Normal]);
            cmd_list->SetTexture(1, m_render_targets[RenderTarget_Gbuffer_Depth]);
            cmd_list->SetTexture(2, m_render_targets[RenderTarget_Gbuffer_Material]);
            cmd_list->SetTexture(3, m_render_targets[RenderTarget_Composition_Ldr_2]);
            cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
            cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
            cmd_list->SetBlendState(m_blend_disabled);
            cmd_list->SetPrimitiveTopology(RHI_PrimitiveTopology_TriangleList);
            cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
            cmd_list->SetRenderTarget(tex_ssr);
            cmd_list->SetViewport(tex_ssr->GetViewport());
            cmd_list->SetShaderVertex(shader_quad);
            cmd_list->SetInputLayout(shader_quad->GetInputLayout());
            cmd_list->SetShaderPixel(shader_ssr);
            cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
            cmd_list->Submit();

            // Bilateral blur
            const auto sigma = 1.0f;
            const auto pixel_stride = 1.0f;
            Pass_BlurGaussian(cmd_list, tex_ssr, tex_ssr_blurred, sigma, pixel_stride);
        }
        else
        {
            cmd_list->ClearRenderTarget(tex_ssr->GetResource_RenderTarget(), Vector4(0.0f, 0.0f, 0.0f, 1.0f));
            cmd_list->ClearRenderTarget(tex_ssr_blurred->GetResource_RenderTarget(), Vector4(0.0f, 0.0f, 0.0f, 1.0f));
            cmd_list->Submit();
        }

        cmd_list->End();
    }

    void Renderer::Pass_Light(RHI_CommandList* cmd_list)
    {
        // Acquire shaders
        const auto& shader_quad                 = m_shaders[Shader_Quad_V];
        const auto& shader_light_directional    = m_shaders[Shader_LightDirectional_P];
        const auto& shader_light_point          = m_shaders[Shader_LightPoint_P];
        const auto& shader_light_spot           = m_shaders[Shader_LightSpot_P];
        if (!shader_quad->IsCompiled() || !shader_light_directional->IsCompiled() || !shader_light_point->IsCompiled() || !shader_light_spot->IsCompiled())
            return;

        // Acquire render targets
        auto& tex_diffuse       = m_render_targets[RenderTarget_Light_Diffuse];
        auto& tex_specular      = m_render_targets[RenderTarget_Light_Specular];
        auto& tex_volumetric    = m_render_targets[RenderTarget_Light_Volumetric];

        // Pack render targets
        void* render_targets[]
        {
            tex_diffuse->GetResource_RenderTarget(),
            tex_specular->GetResource_RenderTarget(),
            tex_volumetric->GetResource_RenderTarget()
        };

        // Begin
        cmd_list->Begin("Pass_Light");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_diffuse->GetWidth()), static_cast<float>(tex_diffuse->GetHeight()));
        UpdateUberBuffer();

        cmd_list->ClearRenderTarget(render_targets[0], Vector4::Zero);
        cmd_list->ClearRenderTarget(render_targets[1], Vector4::Zero);
        cmd_list->ClearRenderTarget(render_targets[2], Vector4::Zero);
        cmd_list->SetRenderTargets(render_targets, 3);
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
        cmd_list->SetViewport(tex_diffuse->GetViewport());
        cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);       
        cmd_list->SetPrimitiveTopology(RHI_PrimitiveTopology_TriangleList);
        cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
        cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
        cmd_list->SetShaderVertex(shader_quad);
        cmd_list->SetInputLayout(shader_quad->GetInputLayout());
        cmd_list->SetBlendState(m_blend_color_add); // light accumulation
        
        auto draw_lights = [this, &cmd_list, &shader_light_directional, &shader_light_point, &shader_light_spot](Renderer_Object_Type type)
        {
            const vector<Entity*>& entities = m_entities[type];
            if (entities.empty())
                return;

            // Choose correct shader
            RHI_Shader* shader = nullptr;
            if (type == Renderer_Object_LightDirectional)   shader = shader_light_directional.get();
            else if (type == Renderer_Object_LightPoint)    shader = shader_light_point.get();
            else if (type == Renderer_Object_LightSpot)     shader = shader_light_spot.get();

            // Update light buffer   
            UpdateLightBuffer(entities);
           
            // Draw
            for (const auto& entity : entities)
            {
                if (Light* light = entity->GetComponent<Light>().get())
                {
                    if (RHI_Texture* shadow_map = light->GetShadowMap().get())
                    {
                        cmd_list->SetTexture(0, m_render_targets[RenderTarget_Gbuffer_Normal]);
                        cmd_list->SetTexture(1, m_render_targets[RenderTarget_Gbuffer_Material]);
                        cmd_list->SetTexture(2, m_render_targets[RenderTarget_Gbuffer_Depth]);
                        cmd_list->SetTexture(3, m_render_targets[RenderTarget_Ssao]);
                        cmd_list->SetTexture(4, light->GetCastShadows() ? (light->GetLightType() == LightType_Directional ? shadow_map : nullptr) : nullptr);
                        cmd_list->SetTexture(5, light->GetCastShadows() ? (light->GetLightType() == LightType_Point       ? shadow_map : nullptr) : nullptr);
                        cmd_list->SetTexture(6, light->GetCastShadows() ? (light->GetLightType() == LightType_Spot        ? shadow_map : nullptr) : nullptr);
                        cmd_list->SetShaderPixel(shader);
                        cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
                        cmd_list->Submit();
                    }
                }
            }
        };

        // Draw lights
        draw_lights(Renderer_Object_LightDirectional);
        draw_lights(Renderer_Object_LightPoint);
        draw_lights(Renderer_Object_LightSpot);

        cmd_list->Submit();

        // If we are doing volumetric lighting, blur it
        if (m_options & Render_VolumetricLighting)
        {
            const auto sigma = 2.0f;
            const auto pixel_stride = 2.0f;
            Pass_BlurGaussian(cmd_list, tex_volumetric, m_render_targets[RenderTarget_Light_Volumetric_Blurred], sigma, pixel_stride);
        }

        cmd_list->End();
    }

	void Renderer::Pass_Composition(RHI_CommandList* cmd_list)
	{
        // Acquire shaders
        const auto& shader_quad         = m_shaders[Shader_Quad_V];
		const auto& shader_composition  = m_shaders[Shader_Composition_P];
		if (!shader_quad->IsCompiled() || !shader_composition->IsCompiled())
			return;

        // Acquire render target
        auto& tex_out = m_render_targets[RenderTarget_Composition_Hdr];

        // Begin command list
		cmd_list->Begin("Pass_Composition");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        UpdateUberBuffer();

		// Setup command list
        cmd_list->UnsetTextures();
        cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetTexture(0, m_render_targets[RenderTarget_Gbuffer_Albedo]);
        cmd_list->SetTexture(1, m_render_targets[RenderTarget_Gbuffer_Normal]);
        cmd_list->SetTexture(2, m_render_targets[RenderTarget_Gbuffer_Depth]);
        cmd_list->SetTexture(3, m_render_targets[RenderTarget_Gbuffer_Material]);
        cmd_list->SetTexture(4, m_render_targets[RenderTarget_Light_Diffuse]);
        cmd_list->SetTexture(5, m_render_targets[RenderTarget_Light_Specular]);
        cmd_list->SetTexture(6, (m_options & Render_VolumetricLighting) ? m_render_targets[RenderTarget_Light_Volumetric_Blurred] : m_tex_black);
        cmd_list->SetTexture(7, m_render_targets[RenderTarget_Ssr_Blurred]);
        cmd_list->SetTexture(8, GetEnvironmentTexture());
        cmd_list->SetTexture(9, m_render_targets[RenderTarget_Brdf_Specular_Lut]);
        cmd_list->SetTexture(10, m_render_targets[RenderTarget_Ssao]);
		cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
		cmd_list->SetBlendState(m_blend_disabled);
		cmd_list->SetPrimitiveTopology(RHI_PrimitiveTopology_TriangleList);
		cmd_list->SetViewport(tex_out->GetViewport());
		cmd_list->SetShaderVertex(shader_quad);
		cmd_list->SetInputLayout(shader_quad->GetInputLayout());
        cmd_list->SetShaderPixel(shader_composition);
		cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
		cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
		cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
		cmd_list->End();
		cmd_list->Submit();
	}

	void Renderer::Pass_PostProcess(RHI_CommandList* cmd_list)
	{
        // IN:  RenderTarget_Composition_Hdr
        // OUT: RenderTarget_Composition_Ldr

		// Acquire shader
		const auto& shader_quad = m_shaders[Shader_Quad_V];
		if (!shader_quad->IsCompiled())
			return;

		// All post-process passes share the following, so set them once here
		cmd_list->Begin("Pass_PostProcess");
		cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
		cmd_list->SetBlendState(m_blend_disabled);
		cmd_list->SetPrimitiveTopology(RHI_PrimitiveTopology_TriangleList);
		cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
		cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
		cmd_list->SetShaderVertex(shader_quad);
		cmd_list->SetInputLayout(shader_quad->GetInputLayout());

        // Acquire render targets
        auto& tex_in_hdr    = m_render_targets[RenderTarget_Composition_Hdr];
        auto& tex_out_hdr   = m_render_targets[RenderTarget_Composition_Hdr_2];
        auto& tex_in_ldr    = m_render_targets[RenderTarget_Composition_Ldr];
        auto& tex_out_ldr   = m_render_targets[RenderTarget_Composition_Ldr_2];

		// Render target swapping
		const auto swap_targets_hdr = [this, &cmd_list, &tex_in_hdr, &tex_out_hdr]() { cmd_list->Submit(); tex_in_hdr.swap(tex_out_hdr); };
        const auto swap_targets_ldr = [this, &cmd_list, &tex_in_ldr, &tex_out_ldr]() { cmd_list->Submit(); tex_in_ldr.swap(tex_out_ldr); };

		// TAA	
        if (GetOptionValue(Render_AntiAliasing_TAA))
        {
            Pass_TAA(cmd_list, tex_in_hdr, tex_out_hdr);
            swap_targets_hdr();
        }

        // Motion Blur
        if (GetOptionValue(Render_MotionBlur))
        {
            Pass_MotionBlur(cmd_list, tex_in_hdr, tex_out_hdr);
            swap_targets_hdr();
        }

		// Bloom
		if (GetOptionValue(Render_Bloom))
		{
			Pass_Bloom(cmd_list, tex_in_hdr, tex_out_hdr);
            swap_targets_hdr();
		}

		// Tone-Mapping
		if (m_option_values[Option_Value_Tonemapping] != 0)
		{
			Pass_ToneMapping(cmd_list, tex_in_hdr, tex_in_ldr); // HDR -> LDR
		}
        else
        {
            Pass_Copy(cmd_list, tex_in_hdr, tex_in_ldr);
        }

        // Dithering
        if (GetOptionValue(Render_Dithering))
        {
            Pass_Dithering(cmd_list, tex_in_ldr, tex_out_ldr);
            swap_targets_ldr();
        }

		// FXAA
		if (GetOptionValue(Render_AntiAliasing_FXAA))
		{
			Pass_FXAA(cmd_list, tex_in_ldr, tex_out_ldr);
            swap_targets_ldr();
		}

		// Sharpening
		if (GetOptionValue(Render_Sharpening_LumaSharpen))
		{
			Pass_LumaSharpen(cmd_list, tex_in_ldr, tex_out_ldr);
            swap_targets_ldr();
		}

		// Chromatic aberration
		if (GetOptionValue(Render_ChromaticAberration))
		{
			Pass_ChromaticAberration(cmd_list, tex_in_ldr, tex_out_ldr);
            swap_targets_ldr();
		}

		// Gamma correction
		Pass_GammaCorrection(cmd_list, tex_in_ldr, tex_out_ldr);
        swap_targets_ldr();

		cmd_list->End();
		cmd_list->Submit();
	}

    void Renderer::Pass_Upsample(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shader
        const auto& shader_vertex   = m_shaders[Shader_Quad_V];
        const auto& shader_pixel    = m_shaders[Shader_Upsample_P];
        if (!shader_vertex->IsCompiled() || !shader_pixel->IsCompiled())
            return;

        cmd_list->Begin("Pass_Upsample");

        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        UpdateUberBuffer();

        cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
        cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
        cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetViewport(tex_out->GetViewport());
        cmd_list->SetShaderVertex(shader_vertex);
        cmd_list->SetShaderPixel(shader_pixel);
        cmd_list->SetTexture(0, tex_in);
        cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
        cmd_list->End();
        cmd_list->Submit();
    }

    void Renderer::Pass_Downsample(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out, Renderer_Shader_Type pixel_shader)
    {
        // Acquire shader
        const auto& shader_vertex   = m_shaders[Shader_Quad_V];
        const auto& shader_pixel    = m_shaders[pixel_shader];
        if (!shader_vertex->IsCompiled() || !shader_pixel->IsCompiled())
            return;

        cmd_list->Begin("Pass_Downsample");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        UpdateUberBuffer();

        cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
        cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
        cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetViewport(tex_out->GetViewport());
        cmd_list->SetShaderVertex(shader_vertex);
        cmd_list->SetShaderPixel(shader_pixel);
        cmd_list->SetTexture(0, tex_in);
        cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
        cmd_list->End();
        cmd_list->Submit();
    }

	void Renderer::Pass_BlurBox(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out, const float sigma)
	{
		// Acquire shader
		const auto& shader_blurBox = m_shaders[Shader_BlurBox_P];
		if (!shader_blurBox->IsCompiled())
			return;

		cmd_list->Begin("Pass_BlurBox");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
		UpdateUberBuffer();

        cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
        cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
		cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		cmd_list->SetViewport(tex_out->GetViewport());
		cmd_list->SetShaderPixel(shader_blurBox);
		cmd_list->SetTexture(0, tex_in); // Shadows are in the alpha channel
		cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		cmd_list->End();
		cmd_list->Submit();
	}

	void Renderer::Pass_BlurGaussian(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out, const float sigma, const float pixel_stride)
	{
		if (tex_in->GetWidth() != tex_out->GetWidth() || tex_in->GetHeight() != tex_out->GetHeight() || tex_in->GetFormat() != tex_out->GetFormat())
		{
			LOG_ERROR("Invalid parameters, textures must match because they will get swapped");
			return;
		}

        // Acquire shaders
        const auto& shader_quad     = m_shaders[Shader_Quad_V];
        const auto& shader_gaussian = m_shaders[Shader_BlurGaussian_P];
        if (!shader_quad->IsCompiled() || !shader_gaussian->IsCompiled())
            return;

		// Start command list
		cmd_list->Begin("Pass_BlurGaussian");
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
        cmd_list->SetBlendState(m_blend_disabled);
        cmd_list->SetViewport(tex_out->GetViewport());
        cmd_list->SetShaderVertex(shader_quad);
        cmd_list->SetInputLayout(shader_quad->GetInputLayout());
        cmd_list->SetShaderPixel(shader_gaussian);
   
		// Horizontal Gaussian blur	
		{
            // Update uber buffer
            m_buffer_uber_cpu.resolution        = Vector2(static_cast<float>(tex_in->GetWidth()), static_cast<float>(tex_in->GetHeight()));
            m_buffer_uber_cpu.blur_direction    = Vector2(pixel_stride, 0.0f);
            m_buffer_uber_cpu.blur_sigma        = sigma;
            UpdateUberBuffer();

			cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
			cmd_list->SetRenderTarget(tex_out);
			cmd_list->SetTexture(0, tex_in);
			cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
            cmd_list->Submit();
		}

		// Vertical Gaussian blur
		{
            m_buffer_uber_cpu.blur_direction    = Vector2(0.0f, pixel_stride);
            m_buffer_uber_cpu.blur_sigma        = sigma;
            UpdateUberBuffer();

			cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
			cmd_list->SetRenderTarget(tex_in);
			cmd_list->SetTexture(0, tex_out);
			cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
            cmd_list->Submit();
		}

		cmd_list->End();
		
		// Swap textures
		tex_in.swap(tex_out);
	}

	void Renderer::Pass_BlurBilateralGaussian(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out, const float sigma, const float pixel_stride)
	{
		if (tex_in->GetWidth() != tex_out->GetWidth() || tex_in->GetHeight() != tex_out->GetHeight() || tex_in->GetFormat() != tex_out->GetFormat())
		{
			LOG_ERROR("Invalid parameters, textures must match because they will get swapped.");
			return;
		}

		// Acquire shaders
		const auto& shader_quad                 = m_shaders[Shader_Quad_V];
        const auto& shader_gaussianBilateral    = m_shaders[Shader_BlurGaussianBilateral_P];
		if (!shader_quad->IsCompiled() || !shader_gaussianBilateral->IsCompiled())
			return;

        // Acquire render targets
        auto& tex_depth     = m_render_targets[RenderTarget_Gbuffer_Depth];
        auto& tex_normal    = m_render_targets[RenderTarget_Gbuffer_Normal];

		// Start command list
        cmd_list->Begin("Pass_BlurBilateralGaussian");
        cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
        cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
        cmd_list->SetBlendState(m_blend_disabled);
		cmd_list->SetViewport(tex_out->GetViewport());	
		cmd_list->SetShaderVertex(shader_quad);
		cmd_list->SetInputLayout(shader_quad->GetInputLayout());
		cmd_list->SetShaderPixel(shader_gaussianBilateral);	

		// Horizontal Gaussian blur
		{
            // Update uber buffer
            m_buffer_uber_cpu.resolution        = Vector2(static_cast<float>(tex_in->GetWidth()), static_cast<float>(tex_in->GetHeight()));
            m_buffer_uber_cpu.blur_direction    = Vector2(pixel_stride, 0.0f);
            m_buffer_uber_cpu.blur_sigma        = sigma;
            UpdateUberBuffer();

			cmd_list->UnsetTextures(); // avoids d3d11 warning where render target is also bound as texture (from Pass_PreLight)
            cmd_list->SetRenderTarget(tex_out);
            cmd_list->SetTexture(0, tex_in);
            cmd_list->SetTexture(1, tex_depth);
            cmd_list->SetTexture(2, tex_normal);
			cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
            cmd_list->Submit();
		}

		// Vertical Gaussian blur
		{
            // Update uber
            m_buffer_uber_cpu.blur_direction    = Vector2(0.0f, pixel_stride);
            m_buffer_uber_cpu.blur_sigma        = sigma;
            UpdateUberBuffer();

            cmd_list->UnsetTexture(0); // avoids d3d11 warning where render target is also bound as texture (from horizontal pass)
            cmd_list->SetRenderTarget(tex_in);
            cmd_list->SetTexture(0, tex_out);
            cmd_list->SetTexture(1, tex_depth);
            cmd_list->SetTexture(2, tex_normal);
			cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
            cmd_list->Submit();
		}

		cmd_list->End();	
		tex_in.swap(tex_out);
	}

	void Renderer::Pass_TAA(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		// Acquire shaders
		const auto& shader_taa      = m_shaders[Shader_Taa_P];
		const auto& shader_texture  = m_shaders[Shader_Texture_P];
		if (!shader_taa->IsCompiled() || !shader_texture->IsCompiled())
			return;

        // Acquire render targets
        auto& tex_history   = m_render_targets[RenderTarget_Composition_Hdr_History];
        auto& tex_history_2 = m_render_targets[RenderTarget_Composition_Hdr_History_2];

		cmd_list->Begin("Pass_TAA");
		// Resolve and accumulate to history texture
		{
            // Update uber buffer
            m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            UpdateUberBuffer();

			cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from some previous pass)
            cmd_list->SetRenderTarget(tex_history_2);
            cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
            cmd_list->SetTexture(0, tex_history);
            cmd_list->SetTexture(1, tex_in);
            cmd_list->SetTexture(2, m_render_targets[RenderTarget_Gbuffer_Velocity]);
            cmd_list->SetTexture(3, m_render_targets[RenderTarget_Gbuffer_Depth]);
			cmd_list->SetViewport(tex_out->GetViewport());
			cmd_list->SetShaderPixel(shader_taa);
			cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
            cmd_list->Submit();
		}

		// Copy
        Pass_Copy(cmd_list, tex_history_2, tex_out);
		cmd_list->End();
		
		// Swap history texture so the above works again in the next frame
        tex_history.swap(tex_history_2);
	}

	void Renderer::Pass_Bloom(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		// Acquire shaders		
		const auto& shader_bloomBright	= m_shaders[Shader_BloomDownsampleLuminance_P];
		const auto& shader_bloomBlend	= m_shaders[Shader_BloomBlend_P];
		const auto& shader_downsample	= m_shaders[Shader_BloomDownsample_P];
		const auto& shader_upsample		= m_shaders[Shader_Upsample_P];
		if (!shader_downsample->IsCompiled() || !shader_bloomBright->IsCompiled() || !shader_upsample->IsCompiled() || !shader_downsample->IsCompiled())
			return;

		cmd_list->Begin("Pass_Bloom");
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
        cmd_list->SetBlendState(m_blend_disabled);

        cmd_list->Begin("Downsample_And_Luminance");
        {
            // Update uber buffer
            m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(m_render_tex_bloom[0]->GetWidth()), static_cast<float>(m_render_tex_bloom[0]->GetHeight()));
            UpdateUberBuffer();

            cmd_list->SetRenderTarget(m_render_tex_bloom[0]);
            cmd_list->SetViewport(m_render_tex_bloom[0]->GetViewport());
            cmd_list->SetShaderPixel(shader_bloomBright);
            cmd_list->SetTexture(0, tex_in);
            cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
        }
        cmd_list->End();

        // Downsample
        // The last bloom texture is the same size as the previous one (it's used for the Gaussian pass below), so we skip it
        for (int i = 0; i < static_cast<int>(m_render_tex_bloom.size() - 1); i++)
        {
            Pass_Downsample(cmd_list, m_render_tex_bloom[i], m_render_tex_bloom[i + 1], Shader_BloomDownsample_P);
        }

        auto upsample = [this, &cmd_list, &shader_upsample](shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
        {
            cmd_list->Begin("Upsample");
            {
                // Update uber buffer
                m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
                UpdateUberBuffer();

                cmd_list->SetBlendState(m_blend_bloom); // blend with previous
                cmd_list->SetRenderTarget(tex_out);
                cmd_list->SetViewport(tex_out->GetViewport());
                cmd_list->SetShaderPixel(shader_upsample);
                cmd_list->SetTexture(0, tex_in);
                cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
            }
            cmd_list->End();
            cmd_list->Submit(); // we have to submit because all upsample passes are using the same buffer
        };

		// Upsample + blend
        cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from some previous pass)
        for (int i = static_cast<int>(m_render_tex_bloom.size() - 1); i > 0; i--)
        {
            upsample(m_render_tex_bloom[i], m_render_tex_bloom[i - 1]);
        }
		
		cmd_list->Begin("Additive_Blending");
		{
            // Update uber buffer
            m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            UpdateUberBuffer();

            cmd_list->SetRenderTarget(tex_out);
            cmd_list->SetTexture(0, tex_in);
            cmd_list->SetTexture(1, m_render_tex_bloom.front());
            cmd_list->SetBlendState(m_blend_disabled);
			cmd_list->SetViewport(tex_out->GetViewport());
			cmd_list->SetShaderPixel(shader_bloomBlend);
			cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
		}
		cmd_list->End();

		cmd_list->End();
		cmd_list->Submit();
	}

	void Renderer::Pass_ToneMapping(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		// Acquire shader
		const auto& shader_toneMapping = m_shaders[Shader_ToneMapping_P];
		if (!shader_toneMapping->IsCompiled())
			return;

		cmd_list->Begin("Pass_ToneMapping");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        UpdateUberBuffer();

		cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
        cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetTexture(0, tex_in);
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		cmd_list->SetViewport(tex_out->GetViewport());
		cmd_list->SetShaderPixel(shader_toneMapping);
		cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
		cmd_list->End();
		cmd_list->Submit();
	}

	void Renderer::Pass_GammaCorrection(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		// Acquire shader
		const auto& shader_gammaCorrection = m_shaders[Shader_GammaCorrection_P];
		if (!shader_gammaCorrection->IsCompiled())
			return;

		cmd_list->Begin("Pass_GammaCorrection");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        UpdateUberBuffer();

		cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)#
        cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetTexture(0, tex_in);
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		cmd_list->SetViewport(tex_out->GetViewport());
		cmd_list->SetShaderPixel(shader_gammaCorrection);
		cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
		cmd_list->End();
		cmd_list->Submit();
	}

	void Renderer::Pass_FXAA(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		// Acquire shaders
		const auto& shader_luma = m_shaders[Shader_Luma_P];
		const auto& shader_fxaa = m_shaders[Shader_Fxaa_P];
		if (!shader_luma->IsCompiled() || !shader_fxaa->IsCompiled())
			return;

		cmd_list->Begin("Pass_FXAA");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        UpdateUberBuffer();

		cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
        cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		cmd_list->SetViewport(tex_out->GetViewport());

		// Luma
		cmd_list->SetRenderTarget(tex_out);	
		cmd_list->SetShaderPixel(shader_luma);
		cmd_list->SetTexture(0, tex_in);
		cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);

		// FXAA
		cmd_list->SetRenderTarget(tex_in);
		cmd_list->SetShaderPixel(shader_fxaa);
		cmd_list->SetTexture(0, tex_out);
		cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);

		cmd_list->End();
		cmd_list->Submit();

		// Swap the textures
		tex_in.swap(tex_out);
	}

	void Renderer::Pass_ChromaticAberration(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		// Acquire shader
		const auto& shader_chromaticAberration = m_shaders[Shader_ChromaticAberration_P];
		if (!shader_chromaticAberration->IsCompiled())
			return;

		cmd_list->Begin("Pass_ChromaticAberration");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        UpdateUberBuffer();

		cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
		cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		cmd_list->SetViewport(tex_out->GetViewport());
		cmd_list->SetShaderPixel(shader_chromaticAberration);
		cmd_list->SetTexture(0, tex_in);
		cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
		cmd_list->End();
		cmd_list->Submit();
	}

	void Renderer::Pass_MotionBlur(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		// Acquire shader
		const auto& shader_motionBlur = m_shaders[Shader_MotionBlur_P];
		if (!shader_motionBlur->IsCompiled())
			return;

		cmd_list->Begin("Pass_MotionBlur");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        UpdateUberBuffer();

        cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
        cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetTexture(0, tex_in);
        cmd_list->SetTexture(1, m_render_targets[RenderTarget_Gbuffer_Velocity]);
        cmd_list->SetTexture(2, m_render_targets[RenderTarget_Gbuffer_Depth]);
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		cmd_list->SetViewport(tex_out->GetViewport());
		cmd_list->SetShaderPixel(shader_motionBlur);
		cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
		cmd_list->End();
		cmd_list->Submit();
	}

	void Renderer::Pass_Dithering(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		// Acquire shader
		const auto& shader_dithering = m_shaders[Shader_Dithering_P];
		if (!shader_dithering->IsCompiled())
			return;

		cmd_list->Begin("Pass_Dithering");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        UpdateUberBuffer();

		cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)  
		cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		cmd_list->SetViewport(tex_out->GetViewport());
		cmd_list->SetShaderPixel(shader_dithering);
		cmd_list->SetTexture(0, tex_in);
		cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
		cmd_list->End();
		cmd_list->Submit();
	}

	void Renderer::Pass_LumaSharpen(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		// Acquire shader
		const auto& shader = m_shaders[Shader_Sharpen_Luma_P];
		if (!shader->IsCompiled())
			return;

		cmd_list->Begin("Pass_LumaSharpen");

        // Update uber buffer
        m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        UpdateUberBuffer();
	
		cmd_list->UnsetTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
		cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		cmd_list->SetViewport(tex_out->GetViewport());		
		cmd_list->SetShaderPixel(shader);
		cmd_list->SetTexture(0, tex_in);
		cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
		cmd_list->End();
		cmd_list->Submit();
	}

	void Renderer::Pass_Lines(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_out)
	{
		const bool draw_picking_ray = m_options & Render_Debug_PickingRay;
		const bool draw_aabb		= m_options & Render_Debug_AABB;
		const bool draw_grid		= m_options & Render_Debug_Grid;
        const bool draw_lights      = m_options & Render_Debug_Lights;
		const auto draw_lines		= !m_lines_list_depth_enabled.empty() || !m_lines_list_depth_disabled.empty(); // Any kind of lines, physics, user debug, etc.
		const auto draw				= draw_picking_ray || draw_aabb || draw_grid || draw_lines || draw_lights;
		if (!draw)
			return;

        // Acquire color shaders
        const auto& shader_color_v = m_shaders[Shader_Color_V];
        const auto& shader_color_p = m_shaders[Shader_Color_P];
        if (!shader_color_v->IsCompiled() || !shader_color_p->IsCompiled())
            return;

        if (cmd_list->Begin("Pass_Lines", RHI_Cmd_Marker))
        {
            // Generate lines for debug primitives offered by the renderer
            {
                // Picking ray
                if (draw_picking_ray)
                {
                    const auto& ray = m_camera->GetPickingRay();
                    DrawLine(ray.GetStart(), ray.GetStart() + ray.GetDirection() * m_camera->GetFarPlane(), Vector4(0, 1, 0, 1));
                }

                // Lights
                if (draw_lights)
                {
                    auto& lights = m_entities[Renderer_Object_Light];
                    for (const auto& entity : lights)
                    {
                        shared_ptr<Light>& light = entity->GetComponent<Light>();

                        if (light->GetLightType() == LightType_Spot)
                        {
                            Vector3 start = light->GetTransform()->GetPosition();
                            Vector3 end = light->GetTransform()->GetForward() * light->GetRange();
                            DrawLine(start, start + end, Vector4(0, 1, 0, 1));
                        }
                    }
                }

                // AABBs
                if (draw_aabb)
                {
                    for (const auto& entity : m_entities[Renderer_Object_Opaque])
                    {
                        if (auto renderable = entity->GetRenderable_PtrRaw())
                        {
                            DrawBox(renderable->GetAabb(), Vector4(0.41f, 0.86f, 1.0f, 1.0f));
                        }
                    }

                    for (const auto& entity : m_entities[Renderer_Object_Transparent])
                    {
                        if (auto renderable = entity->GetRenderable_PtrRaw())
                        {
                            DrawBox(renderable->GetAabb(), Vector4(0.41f, 0.86f, 1.0f, 1.0f));
                        }
                    }
                }
            }

            // Draw lines with depth
            {
                // Set render state
                RHI_PipelineState& pipeline_state           = cmd_list->GetPipelineState();
                pipeline_state.shader_vertex                = shader_color_v.get();
                pipeline_state.shader_pixel                 = shader_color_p.get();
                pipeline_state.input_layout                 = shader_color_v->GetInputLayout().get();
                pipeline_state.rasterizer_state             = m_rasterizer_cull_back_wireframe.get();
                pipeline_state.blend_state                  = m_blend_enabled.get();
                pipeline_state.depth_stencil_state          = m_depth_stencil_enabled_no_write.get();
                pipeline_state.vertex_buffer_stride         = m_quad.GetVertexBuffer()->GetStride(); // stride matches rect
                pipeline_state.render_target_color_texture  = tex_out.get();
                pipeline_state.render_target_depth_texture  = m_render_targets[RenderTarget_Gbuffer_Depth].get();
                pipeline_state.primitive_topology           = RHI_PrimitiveTopology_LineList;
                pipeline_state.viewport                     = tex_out->GetViewport();

                // Create and submit command list
                if (cmd_list->Begin("Lines_With_Depth", RHI_Cmd_Marker))
                {
                    // Grid
                    if (draw_grid)
                    {
                        // Create and submit command list
                        if (cmd_list->Begin("Grid", RHI_Cmd_Begin))
                        {
                            // Update uber buffer
                            m_buffer_uber_cpu.resolution    = m_resolution;
                            m_buffer_uber_cpu.transform     = m_gizmo_grid->ComputeWorldMatrix(m_camera->GetTransform()) * m_buffer_frame_cpu.view_projection_unjittered;
                            UpdateUberBuffer();

                            cmd_list->SetBufferIndex(m_gizmo_grid->GetIndexBuffer());
                            cmd_list->SetBufferVertex(m_gizmo_grid->GetVertexBuffer());
                            cmd_list->DrawIndexed(m_gizmo_grid->GetIndexCount(), 0, 0);
                            cmd_list->End();
                            cmd_list->Submit();
                        }
                    }

                    // Lines
                    const auto line_vertex_buffer_size = static_cast<uint32_t>(m_lines_list_depth_enabled.size());
                    if (line_vertex_buffer_size != 0)
                    {
                        // Grow vertex buffer (if needed)
                        if (line_vertex_buffer_size > m_vertex_buffer_lines->GetVertexCount())
                        {
                            m_vertex_buffer_lines->CreateDynamic<RHI_Vertex_PosCol>(line_vertex_buffer_size);
                        }

                        // Update vertex buffer
                        const auto buffer = static_cast<RHI_Vertex_PosCol*>(m_vertex_buffer_lines->Map());
                        copy(m_lines_list_depth_enabled.begin(), m_lines_list_depth_enabled.end(), buffer);
                        m_vertex_buffer_lines->Unmap();
                        m_lines_list_depth_enabled.clear();

                        // Create and submit command list
                        if (cmd_list->Begin("Lines", RHI_Cmd_Begin))
                        {
                            cmd_list->SetBufferVertex(m_vertex_buffer_lines);
                            cmd_list->Draw(line_vertex_buffer_size);
                            cmd_list->End();
                            cmd_list->Submit();
                        }
                    }

                    cmd_list->End();
                }
            }

            // Draw lines without depth
            {
                // Set render state
                RHI_PipelineState& pipeline_state           = cmd_list->GetPipelineState();
                pipeline_state.shader_vertex                = shader_color_v.get();
                pipeline_state.shader_pixel                 = shader_color_p.get();
                pipeline_state.input_layout                 = shader_color_v->GetInputLayout().get();
                pipeline_state.rasterizer_state             = m_rasterizer_cull_back_wireframe.get();
                pipeline_state.blend_state                  = m_blend_disabled.get();
                pipeline_state.depth_stencil_state          = m_depth_stencil_disabled.get();
                pipeline_state.vertex_buffer_stride         = m_quad.GetVertexBuffer()->GetStride(); // stride matches rect
                pipeline_state.render_target_color_texture  = tex_out.get();
                pipeline_state.primitive_topology           = RHI_PrimitiveTopology_LineList;
                pipeline_state.viewport                     = tex_out->GetViewport();

                // Create and submit command list
                if (cmd_list->Begin("Lines_No_Depth", RHI_Cmd_Begin))
                {
                    // Lines
                    const auto line_vertex_buffer_size = static_cast<uint32_t>(m_lines_list_depth_disabled.size());
                    if (line_vertex_buffer_size != 0)
                    {
                        // Grow vertex buffer (if needed)
                        if (line_vertex_buffer_size > m_vertex_buffer_lines->GetVertexCount())
                        {
                            m_vertex_buffer_lines->CreateDynamic<RHI_Vertex_PosCol>(line_vertex_buffer_size);
                        }

                        // Update vertex buffer
                        const auto buffer = static_cast<RHI_Vertex_PosCol*>(m_vertex_buffer_lines->Map());
                        copy(m_lines_list_depth_disabled.begin(), m_lines_list_depth_disabled.end(), buffer);
                        m_vertex_buffer_lines->Unmap();
                        m_lines_list_depth_disabled.clear();

                        cmd_list->SetBufferVertex(m_vertex_buffer_lines);
                        cmd_list->Draw(line_vertex_buffer_size);
                    }

                    cmd_list->End();
                    cmd_list->Submit();
                }
            }

            cmd_list->End();
        }
	}

	void Renderer::Pass_Gizmos(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_out)
	{
        // Early exit cases
		bool render_lights		                = m_options & Render_Debug_Lights;
		bool render_transform	                = m_options & Render_Debug_Transform;
        bool render				                = render_lights || render_transform;
		const auto& shader_quad_v               = m_shaders[Shader_Quad_V];
        const auto& shader_texture_p            = m_shaders[Shader_Texture_P];
        auto const& shader_gizmo_transform_v    = m_shaders[Shader_GizmoTransform_V];
        auto const& shader_gizmo_transform_p    = m_shaders[Shader_GizmoTransform_P];
		if (!render || !shader_quad_v->IsCompiled() || !shader_texture_p->IsCompiled() || !shader_gizmo_transform_v->IsCompiled() || !shader_gizmo_transform_p->IsCompiled())
			return;

        // Submit command list
		if (cmd_list->Begin("Pass_Gizmos", RHI_Cmd_Marker))
        {
		    auto& lights = m_entities[Renderer_Object_Light];
		    if (render_lights && !lights.empty())
		    {
                // Set render state
                RHI_PipelineState& pipeline_state           = cmd_list->GetPipelineState();
                pipeline_state.shader_vertex                = shader_quad_v.get();
                pipeline_state.shader_pixel                 = shader_texture_p.get();
                pipeline_state.input_layout                 = shader_quad_v->GetInputLayout().get();
                pipeline_state.rasterizer_state             = m_rasterizer_cull_back_solid.get();
                pipeline_state.blend_state                  = m_blend_enabled.get();
                pipeline_state.depth_stencil_state          = m_depth_stencil_disabled.get();
                pipeline_state.vertex_buffer_stride         = m_quad.GetVertexBuffer()->GetStride(); // stride matches rect
                pipeline_state.render_target_color_texture  = tex_out.get();
                pipeline_state.primitive_topology           = RHI_PrimitiveTopology_TriangleList;
                pipeline_state.viewport                     = tex_out->GetViewport();

                // Create and submit command list
		    	if (cmd_list->Begin("Lights", RHI_Cmd_Marker))
                {
		    	    for (const auto& entity : lights)
		    	    {
                        if (cmd_list->Begin("Lights", RHI_Cmd_Begin))
                        {
                            // Light can be null if it just got removed and our buffer doesn't update till the next frame
                            if (Light* light = entity->GetComponent<Light>().get())
                            {
                                auto position_light_world       = entity->GetTransform_PtrRaw()->GetPosition();
                                auto position_camera_world      = m_camera->GetTransform()->GetPosition();
                                auto direction_camera_to_light  = (position_light_world - position_camera_world).Normalized();
                                auto v_dot_l                    = Vector3::Dot(m_camera->GetTransform()->GetForward(), direction_camera_to_light);

                                // Only draw if it's inside our view
                                if (v_dot_l > 0.5f)
                                {
                                    // Compute light screen space position and scale (based on distance from the camera)
                                    auto position_light_screen = m_camera->WorldToScreenPoint(position_light_world);
                                    auto distance = (position_camera_world - position_light_world).Length() + M_EPSILON;
                                    auto scale = m_gizmo_size_max / distance;
                                    scale = Clamp(scale, m_gizmo_size_min, m_gizmo_size_max);

                                    // Choose texture based on light type
                                    shared_ptr<RHI_Texture> light_tex = nullptr;
                                    auto type = light->GetLightType();
                                    if (type == LightType_Directional)	light_tex = m_gizmo_tex_light_directional;
                                    else if (type == LightType_Point)	light_tex = m_gizmo_tex_light_point;
                                    else if (type == LightType_Spot)	light_tex = m_gizmo_tex_light_spot;

                                    // Construct appropriate rectangle
                                    auto tex_width = light_tex->GetWidth() * scale;
                                    auto tex_height = light_tex->GetHeight() * scale;
                                    auto rectangle = Math::Rectangle(position_light_screen.x - tex_width * 0.5f, position_light_screen.y - tex_height * 0.5f, tex_width, tex_height);
                                    if (rectangle != m_gizmo_light_rect)
                                    {
                                        m_gizmo_light_rect = rectangle;
                                        m_gizmo_light_rect.CreateBuffers(this);
                                    }

                                    // Update uber buffer
                                    m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(tex_width), static_cast<float>(tex_width));
                                    m_buffer_uber_cpu.transform = m_buffer_frame_cpu.view_projection_ortho;
                                    UpdateUberBuffer();

                                    cmd_list->SetTexture(0, light_tex);
                                    cmd_list->SetBufferIndex(m_gizmo_light_rect.GetIndexBuffer());
                                    cmd_list->SetBufferVertex(m_gizmo_light_rect.GetVertexBuffer());
                                    cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
                                }
                            }
                            cmd_list->End();
                            cmd_list->Submit();
                        }
		    	    }
                    cmd_list->End();
                }
		    }

		    // Transform
		    if (render_transform && m_gizmo_transform->Update(m_camera.get(), m_gizmo_transform_size, m_gizmo_transform_speed))
		    {
                // Set render state
                RHI_PipelineState& pipeline_state           = cmd_list->GetPipelineState();
                pipeline_state.shader_vertex                = shader_gizmo_transform_v.get();
                pipeline_state.shader_pixel                 = shader_gizmo_transform_p.get();
                pipeline_state.input_layout                 = shader_gizmo_transform_v->GetInputLayout().get();
                pipeline_state.rasterizer_state             = m_rasterizer_cull_back_solid.get();
                pipeline_state.blend_state                  = m_blend_enabled.get();
                pipeline_state.depth_stencil_state          = m_depth_stencil_disabled.get();
                pipeline_state.vertex_buffer_stride         = m_gizmo_transform->GetVertexBuffer()->GetStride();
                pipeline_state.render_target_color_texture  = tex_out.get();
                pipeline_state.primitive_topology           = RHI_PrimitiveTopology_TriangleList;
                pipeline_state.viewport                     = tex_out->GetViewport();

                // Create and submit command list
                if (cmd_list->Begin("Transform", RHI_Cmd_Marker))
                {
                    // Axis - X
                    if (cmd_list->Begin("Axis_X", RHI_Cmd_Begin))
                    {
                        m_buffer_uber_cpu.transform = m_gizmo_transform->GetHandle().GetTransform(Vector3::Right);
                        m_buffer_uber_cpu.transform_axis = m_gizmo_transform->GetHandle().GetColor(Vector3::Right);
                        UpdateUberBuffer();

                        cmd_list->SetBufferIndex(m_gizmo_transform->GetIndexBuffer());
                        cmd_list->SetBufferVertex(m_gizmo_transform->GetVertexBuffer());
                        cmd_list->DrawIndexed(m_gizmo_transform->GetIndexCount(), 0, 0);
                        cmd_list->End();
                        cmd_list->Submit();
                    }
                    
                    // Axis - Y
                    if (cmd_list->Begin("Axis_Y", RHI_Cmd_Begin))
                    {
                        m_buffer_uber_cpu.transform = m_gizmo_transform->GetHandle().GetTransform(Vector3::Up);
                        m_buffer_uber_cpu.transform_axis = m_gizmo_transform->GetHandle().GetColor(Vector3::Up);
                        UpdateUberBuffer();

                        cmd_list->DrawIndexed(m_gizmo_transform->GetIndexCount(), 0, 0);
                        cmd_list->End();
                        cmd_list->Submit();
                    }
                    
                    // Axis - Z
                    if (cmd_list->Begin("Axis_Z", RHI_Cmd_Begin))
                    {
                        m_buffer_uber_cpu.transform = m_gizmo_transform->GetHandle().GetTransform(Vector3::Forward);
                        m_buffer_uber_cpu.transform_axis = m_gizmo_transform->GetHandle().GetColor(Vector3::Forward);
                        UpdateUberBuffer();

                        cmd_list->DrawIndexed(m_gizmo_transform->GetIndexCount(), 0, 0);
                        cmd_list->End();
                        cmd_list->Submit();
                    }
                    
                    // Axes - XYZ
                    if (m_gizmo_transform->DrawXYZ())
                    {
                        if (cmd_list->Begin("Axis_XYZ", RHI_Cmd_Begin))
                        {
                            m_buffer_uber_cpu.transform = m_gizmo_transform->GetHandle().GetTransform(Vector3::One);
                            m_buffer_uber_cpu.transform_axis = m_gizmo_transform->GetHandle().GetColor(Vector3::One);
                            UpdateUberBuffer();

                            cmd_list->DrawIndexed(m_gizmo_transform->GetIndexCount(), 0, 0);
                            cmd_list->End();
                            cmd_list->Submit();
                        }
                    }

                    cmd_list->End();
                }
		    }

		    cmd_list->End();
        }
	}

	void Renderer::Pass_PerformanceMetrics(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_out)
	{
        // Early exit cases
        const bool draw             = m_options & Render_Debug_PerformanceMetrics;
        const bool empty            = m_profiler->GetMetrics().empty();
        const auto& shader_font_v   = m_shaders[Shader_Font_V];
        const auto& shader_font_p   = m_shaders[Shader_Font_P];
        if (!draw || empty || !shader_font_v->IsCompiled() || !shader_font_p->IsCompiled())
            return;

        // Set render state
        RHI_PipelineState& pipeline_state           = cmd_list->GetPipelineState();
        pipeline_state.shader_vertex                = shader_font_v.get();
        pipeline_state.shader_pixel                 = shader_font_p.get();
        pipeline_state.input_layout                 = shader_font_v->GetInputLayout().get();
        pipeline_state.rasterizer_state             = m_rasterizer_cull_back_solid.get();
        pipeline_state.blend_state                  = m_blend_enabled.get();
        pipeline_state.depth_stencil_state          = m_depth_stencil_disabled.get();
        pipeline_state.vertex_buffer_stride         = m_font->GetVertexBuffer()->GetStride();
        pipeline_state.render_target_color_texture  = tex_out.get();
        pipeline_state.primitive_topology           = RHI_PrimitiveTopology_TriangleList;
        pipeline_state.viewport                     = tex_out->GetViewport();

        // Submit command list
        if (cmd_list->Begin("Pass_PerformanceMetrics"))
        {
            // Update text
            const auto text_pos = Vector2(-static_cast<int>(m_viewport.width) * 0.5f + 1.0f, static_cast<int>(m_viewport.height) * 0.5f);
            m_font->SetText(m_profiler->GetMetrics(), text_pos);

            // Update uber buffer
            m_buffer_uber_cpu.resolution    = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            m_buffer_uber_cpu.color         = m_font->GetColor();
            UpdateUberBuffer();

            cmd_list->SetTexture(0, m_font->GetAtlas());
            cmd_list->SetBufferIndex(m_font->GetIndexBuffer());
            cmd_list->SetBufferVertex(m_font->GetVertexBuffer());
            cmd_list->DrawIndexed(m_font->GetIndexCount(), 0, 0);
            cmd_list->End();
            cmd_list->Submit();
        }
	}

	bool Renderer::Pass_DebugBuffer(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_out)
	{
		if (m_debug_buffer == Renderer_Buffer_None)
			return true;

		// Bind correct texture & shader pass
        shared_ptr<RHI_Texture> texture;
        Renderer_Shader_Type shader_type;
		if (m_debug_buffer == Renderer_Buffer_Albedo)
		{
			texture     = m_render_targets[RenderTarget_Gbuffer_Albedo];
			shader_type = Shader_Texture_P;
		}

		if (m_debug_buffer == Renderer_Buffer_Normal)
		{
			texture     = m_render_targets[RenderTarget_Gbuffer_Normal];
			shader_type = Shader_DebugNormal_P;
		}

		if (m_debug_buffer == Renderer_Buffer_Material)
		{
			texture     = m_render_targets[RenderTarget_Gbuffer_Material];
			shader_type = Shader_Texture_P;
		}

        if (m_debug_buffer == Renderer_Buffer_Diffuse)
        {
            texture     = m_render_targets[RenderTarget_Light_Diffuse];
            shader_type = Shader_DebugChannelRgbGammaCorrect_P;
        }

        if (m_debug_buffer == Renderer_Buffer_Specular)
        {
            texture     = m_render_targets[RenderTarget_Light_Specular];
            shader_type = Shader_DebugChannelRgbGammaCorrect_P;
        }

		if (m_debug_buffer == Renderer_Buffer_Velocity)
		{
			texture     = m_render_targets[RenderTarget_Gbuffer_Velocity];
			shader_type = Shader_DebugVelocity_P;
		}

		if (m_debug_buffer == Renderer_Buffer_Depth)
		{
			texture     = m_render_targets[RenderTarget_Gbuffer_Depth];
			shader_type = Shader_DebugChannelR_P;
		}

		if (m_debug_buffer == Renderer_Buffer_SSAO)
		{
			texture     = m_options & Render_SSAO ? m_render_targets[RenderTarget_Ssao] : m_tex_white;
			shader_type = Shader_DebugChannelR_P;
		}

        if (m_debug_buffer == Renderer_Buffer_SSR)
        {
            texture     = m_render_targets[RenderTarget_Ssr_Blurred];
            shader_type = Shader_DebugChannelRgbGammaCorrect_P;
        }

        if (m_debug_buffer == Renderer_Buffer_Bloom)
        {
            texture     = m_render_tex_bloom.front();
            shader_type = Shader_DebugChannelRgbGammaCorrect_P;
        }

        if (m_debug_buffer == Renderer_Buffer_VolumetricLighting)
        {
            texture     = m_render_targets[RenderTarget_Light_Volumetric_Blurred];
            shader_type = Shader_DebugChannelRgbGammaCorrect_P;
        }

        if (m_debug_buffer == Renderer_Buffer_Shadows)
        {
            texture     = m_render_targets[RenderTarget_Light_Diffuse];
            shader_type = Shader_DebugChannelA_P;
        }

        // Acquire shaders
        const auto& shader_quad     = m_shaders[Shader_Quad_V];
        const auto& shader_pixel    = m_shaders[shader_type];
        if (!shader_quad->IsCompiled() || !shader_pixel->IsCompiled())
            return false;

        // Set render state
        RHI_PipelineState& pipeline_state           = cmd_list->GetPipelineState();
        pipeline_state.shader_vertex                = shader_quad.get();
        pipeline_state.shader_pixel                 = shader_pixel.get();
        pipeline_state.input_layout                 = shader_quad->GetInputLayout().get();
        pipeline_state.rasterizer_state             = m_rasterizer_cull_back_solid.get();
        pipeline_state.blend_state                  = m_blend_disabled.get();
        pipeline_state.depth_stencil_state          = m_depth_stencil_disabled.get();
        pipeline_state.vertex_buffer_stride         = m_quad.GetVertexBuffer()->GetStride();
        pipeline_state.render_target_color_texture  = tex_out.get();
        pipeline_state.primitive_topology           = RHI_PrimitiveTopology_TriangleList;
        pipeline_state.viewport                     = tex_out->GetViewport();

        // // Submit command list
        if (cmd_list->Begin("Pass_DebugBuffer"))
        {
            // Update uber buffer
            m_buffer_uber_cpu.resolution    = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            m_buffer_uber_cpu.transform     = m_buffer_frame_cpu.view_projection_ortho;
            UpdateUberBuffer();

            cmd_list->SetTexture(0, texture);
            cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
            cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
            cmd_list->End();
            cmd_list->Submit();
        }

		return true;
	}

    void Renderer::Pass_BrdfSpecularLut(RHI_CommandList* cmd_list)
    {
        // Acquire shaders
        const auto& shader_quad                 = m_shaders[Shader_Quad_V];
        const auto& shader_brdf_specular_lut    = m_shaders[Shader_BrdfSpecularLut];
        if (!shader_quad->IsCompiled() || !shader_brdf_specular_lut->IsCompiled())
            return;

        // Acquire render target
        const auto& render_target = m_render_targets[RenderTarget_Brdf_Specular_Lut];

        // Set render state
        RHI_PipelineState& pipeline_state           = cmd_list->GetPipelineState();
        pipeline_state.shader_vertex                = shader_quad.get();
        pipeline_state.shader_pixel                 = shader_brdf_specular_lut.get();
        pipeline_state.input_layout                 = shader_quad->GetInputLayout().get();
        pipeline_state.rasterizer_state             = m_rasterizer_cull_back_solid.get();
        pipeline_state.blend_state                  = m_blend_disabled.get();
        pipeline_state.depth_stencil_state          = m_depth_stencil_disabled.get();
        pipeline_state.vertex_buffer_stride         = m_quad.GetVertexBuffer()->GetStride();
        pipeline_state.render_target_color_texture  = render_target.get();
        pipeline_state.primitive_topology           = RHI_PrimitiveTopology_TriangleList;
        pipeline_state.viewport                     = render_target->GetViewport();

        // Submit command list
        if (cmd_list->Begin("Pass_BrdfSpecularLut"))
        {
            // Update uber buffer
            m_buffer_uber_cpu.resolution = Vector2(static_cast<float>(render_target->GetWidth()), static_cast<float>(render_target->GetHeight()));
            UpdateUberBuffer();

            cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
            cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
            cmd_list->End();
            cmd_list->Submit();
        }
    }

    void Renderer::Pass_Copy(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        const auto& shader_quad     = m_shaders[Shader_Quad_V];
        const auto& shader_pixel    = m_shaders[Shader_Texture_P];
        if (!shader_quad->IsCompiled() || !shader_pixel->IsCompiled())
            return;

        // Draw
        cmd_list->Begin("Pass_Copy");

        // Update uber buffer
        m_buffer_uber_cpu.resolution    = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
        m_buffer_uber_cpu.transform     = m_buffer_frame_cpu.view_projection_ortho;
        UpdateUberBuffer();

        cmd_list->UnsetTextures();
        cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
        cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
        cmd_list->SetBlendState(m_blend_disabled);
        cmd_list->SetPrimitiveTopology(RHI_PrimitiveTopology_TriangleList);
        cmd_list->SetRenderTarget(tex_out);
        cmd_list->SetViewport(tex_out->GetViewport());
        cmd_list->SetShaderVertex(shader_quad);
        cmd_list->SetInputLayout(shader_quad->GetInputLayout());
        cmd_list->SetShaderPixel(shader_pixel);
        cmd_list->SetTexture(0, tex_in);
        cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
        cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
        cmd_list->DrawIndexed(Rectangle::GetIndexCount(), 0, 0);
        cmd_list->End();
        cmd_list->Submit();
    }
}
