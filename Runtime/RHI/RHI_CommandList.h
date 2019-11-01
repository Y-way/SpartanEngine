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

#pragma once

//= INCLUDES =================
#include <vector>
#include "RHI_Texture.h"
#include "RHI_Viewport.h"
#include "RHI_Definition.h"
#include "../Math/Vector4.h"
#include "../Math/Rectangle.h"
#include "RHI_PipelineCache.h"
//============================

namespace Spartan
{
	class Profiler;

    enum RHI_Cmd_List_State
    {
        RHI_Cmd_List_Idle,
        RHI_Cmd_List_Idle_Sync_Cpu_To_Gpu,
        RHI_Cmd_List_Recording,
        RHI_Cmd_List_Ended
    };

	enum RHI_Cmd_Type
	{
		RHI_Cmd_Begin,
		RHI_Cmd_End,
		RHI_Cmd_Draw,
		RHI_Cmd_DrawIndexed,
		RHI_Cmd_SetViewport,
		RHI_Cmd_SetScissorRectangle,
		RHI_Cmd_SetPrimitiveTopology,
		RHI_Cmd_SetInputLayout,
		RHI_Cmd_SetDepthStencilState,
		RHI_Cmd_SetRasterizerState,
		RHI_Cmd_SetBlendState,
		RHI_Cmd_SetVertexBuffer,
		RHI_Cmd_SetIndexBuffer,	
		RHI_Cmd_SetVertexShader,
		RHI_Cmd_SetPixelShader,
        RHI_Cmd_SetComputeShader,
		RHI_Cmd_SetConstantBuffers,
		RHI_Cmd_SetSamplers,
		RHI_Cmd_SetTextures,
		RHI_Cmd_SetRenderTargets,
		RHI_Cmd_ClearRenderTarget,
		RHI_Cmd_ClearDepthStencil,
        RHI_Cmd_Unknown
	};

	struct RHI_Command
	{
        RHI_Command() = default;

		void Clear()
		{
            type                = RHI_Cmd_Unknown;
            pass_name           = "N/A";
            input_layout        = nullptr;
            rasterizer_state    = nullptr;
            blend_state         = nullptr;
            primitive_topology  = PrimitiveTopology_Unknown;
            is_array            = true;

            render_target_count = 0;
            render_targets      = nullptr;

			texture_start_slot  = 0;
			texture_count	    = 0;
			textures		    = nullptr;

			sampler_start_slot  = 0;
			sampler_count	    = 0;
            samplers            = nullptr;

			constant_buffer_start_slot	= 0;
			constant_buffer_count		= 0;
			constant_buffer_scope		= Buffer_Unknown;
            constant_buffers            = nullptr;

			depth_stencil_state	= nullptr;
			depth_stencil		= nullptr;
			depth_clear			= 0;
			depth_clear_stencil = 0;
			depth_clear_flags	= 0;

			vertex_count	= 0;
			vertex_offset	= 0;
			index_count		= 0;
			index_offset	= 0;

			buffer_index    = nullptr;
			buffer_vertex   = nullptr;

			shader_vertex   = nullptr;
			shader_pixel    = nullptr;
            shader_compute  = nullptr;
		}

        // Misc
        RHI_Cmd_Type type                               = RHI_Cmd_Unknown;
        bool is_array                                   = true;
        std::string pass_name                           = "N/A";
        RHI_PrimitiveTopology_Mode primitive_topology   = PrimitiveTopology_Unknown;
        const RHI_InputLayout* input_layout             = nullptr;
        const RHI_RasterizerState* rasterizer_state     = nullptr;
        const RHI_BlendState* blend_state               = nullptr;

		// Render targets
		uint32_t render_target_count    = 0;
        const void* render_targets      = nullptr;
        void* render_target_clear       = nullptr;
		Math::Vector4 render_target_clear_color;

		// Texture
		uint32_t texture_start_slot	= 0;
		uint32_t texture_count      = 0;
		const void* textures        = nullptr;

		// Samplers
		uint32_t sampler_start_slot = 0;
		uint32_t sampler_count      = 0;
        const void* samplers        = nullptr;

		// Constant buffers
		uint32_t constant_buffer_start_slot     = 0;
		uint32_t constant_buffer_count          = 0;
        RHI_Buffer_Scope constant_buffer_scope  = Buffer_Unknown;
        const void* constant_buffers            = nullptr;

		// Depth
		const RHI_DepthStencilState* depth_stencil_state	= nullptr;
		void* depth_stencil									= nullptr;
		float depth_clear									= 0;
		uint32_t depth_clear_stencil						= 0;
		uint32_t depth_clear_flags							= 0;

		// Misc	
		uint32_t vertex_count					= 0;
		uint32_t vertex_offset					= 0;
		uint32_t index_count					= 0;
		uint32_t index_offset					= 0;		
		const RHI_IndexBuffer* buffer_index		= nullptr;
		const RHI_VertexBuffer* buffer_vertex   = nullptr;
		const RHI_Shader* shader_vertex			= nullptr;
		const RHI_Shader* shader_pixel			= nullptr;
        const RHI_Shader* shader_compute        = nullptr;
		RHI_Viewport viewport;
		Math::Rectangle scissor_rectangle;
	};

	class SPARTAN_CLASS RHI_CommandList
	{
	public:
		RHI_CommandList(const std::shared_ptr<RHI_Device>& rhi_device, RHI_PipelineCache* rhi_pipeline_cache, Profiler* profiler);
		~RHI_CommandList();

		// Markers
		void Begin(const std::string& pass_name);
		void End();

		// Draw
		void Draw(uint32_t vertex_count);
		void DrawIndexed(uint32_t index_count, uint32_t index_offset, uint32_t vertex_offset);

		// Misc
		void SetViewport(const RHI_Viewport& viewport);
		void SetScissorRectangle(const Math::Rectangle& scissor_rectangle);
		void SetPrimitiveTopology(RHI_PrimitiveTopology_Mode primitive_topology);

		// Input layout
		void SetInputLayout(const RHI_InputLayout* input_layout);
		void SetInputLayout(const std::shared_ptr<RHI_InputLayout>& input_layout) { SetInputLayout(input_layout.get()); }

		// Depth-stencil state
		void SetDepthStencilState(const RHI_DepthStencilState* depth_stencil_state);
		void SetDepthStencilState(const std::shared_ptr<RHI_DepthStencilState>& depth_stencil_state) { SetDepthStencilState(depth_stencil_state.get()); }

		// Rasterizer state
		void SetRasterizerState(const RHI_RasterizerState* rasterizer_state);
		void SetRasterizerState(const std::shared_ptr<RHI_RasterizerState>& rasterizer_state) { SetRasterizerState(rasterizer_state.get()); }

		// Blend state
		void SetBlendState(const RHI_BlendState* blend_state);
		void SetBlendState(const std::shared_ptr<RHI_BlendState>& blend_state) { SetBlendState(blend_state.get()); }

		// Vertex buffer
		void SetBufferVertex(const RHI_VertexBuffer* buffer);
		void SetBufferVertex(const std::shared_ptr<RHI_VertexBuffer>& buffer) { SetBufferVertex(buffer.get()); }

		// Index buffer
		void SetBufferIndex(const RHI_IndexBuffer* buffer);
		void SetBufferIndex(const std::shared_ptr<RHI_IndexBuffer>& buffer) { SetBufferIndex(buffer.get()); }

		// Vertex shader
		void SetShaderVertex(const RHI_Shader* shader);
		void SetShaderVertex(const std::shared_ptr<RHI_Shader>& shader) { SetShaderVertex(shader.get()); }

		// Pixel shader
		void SetShaderPixel(const RHI_Shader* shader);
		void SetShaderPixel(const std::shared_ptr<RHI_Shader>& shader) { SetShaderPixel(shader.get()); }

        // Compute shader
        void SetShaderCompute(const RHI_Shader* shader);
        void SetShaderCompute(const std::shared_ptr<RHI_Shader>& shader) { SetShaderCompute(shader.get()); }

		// Constant buffer
		void SetConstantBuffers(uint32_t start_slot, RHI_Buffer_Scope scope, const void* constant_buffers, uint32_t constant_buffer_count, bool is_array = true);
        void SetConstantBuffer(const uint32_t start_slot, RHI_Buffer_Scope scope, RHI_Sampler* constant_buffer)                                 { SetConstantBuffers(start_slot, scope, constant_buffer ? constant_buffer->GetResource() : nullptr, 1, false); }
		void SetConstantBuffer(const uint32_t start_slot, RHI_Buffer_Scope scope, const std::shared_ptr<RHI_ConstantBuffer>& constant_buffer)   { SetConstantBuffers(start_slot, scope, constant_buffer ? constant_buffer->GetResource() : nullptr, 1, false); }

		// Sampler
		void SetSamplers(uint32_t start_slot, const void* samplers, uint32_t sampler_count, bool is_array = true);
        void SetSampler(const uint32_t start_slot, RHI_Sampler* sampler)                          { SetSamplers(start_slot, sampler ? sampler->GetResource() : nullptr, 1, false); }
        void SetSampler(const uint32_t start_slot, const std::shared_ptr<RHI_Sampler>& sampler)   { SetSamplers(start_slot, sampler ? sampler->GetResource() : nullptr, 1, false); }

		// Texture
		void SetTextures(const uint32_t start_slot, const void* textures, uint32_t texture_count, bool is_array = true);
		void SetTexture(const uint32_t start_slot, RHI_Texture* texture)                        { SetTextures(start_slot, texture ? texture->GetResource_Texture() : nullptr, 1, false); }
		void SetTexture(const uint32_t start_slot, const std::shared_ptr<RHI_Texture>& texture)	{ SetTextures(start_slot, texture ? texture->GetResource_Texture() : nullptr, 1, false); }
		void ClearTextures()																    { SetTextures(0, m_textures_empty.data(), static_cast<uint32_t>(m_textures_empty.size())); }

		// Render targets
		void SetRenderTargets(const void* render_targets, uint32_t render_target_count, void* depth_stencil = nullptr, bool is_array = true);
		void SetRenderTarget(void* render_target, void* depth_stencil = nullptr)                                { SetRenderTargets(render_target, 1, depth_stencil, false); }
		void SetRenderTarget(const std::shared_ptr<RHI_Texture>& render_target, void* depth_stencil = nullptr)  { SetRenderTargets(render_target ? render_target->GetResource_RenderTarget() : nullptr, 1, depth_stencil, false); }
		void ClearRenderTarget(void* render_target, const Math::Vector4& color);
		void ClearDepthStencil(void* depth_stencil, uint32_t flags, float depth, uint32_t stencil = 0);

        RHI_PipelineState& GetPipelineState()
        {
            m_pipeline_state.Clear();
            return m_pipeline_state;
        }

		bool Submit(bool profile = true);

	private:
		void Clear();

		// Dependencies
		std::shared_ptr<RHI_Device> m_rhi_device;
        RHI_PipelineState m_pipeline_state;
        RHI_PipelineCache* m_rhi_pipeline_cache = nullptr;
        Profiler* m_profiler = nullptr;
		std::vector<void*> m_textures_empty = std::vector<void*>(10);

		// API
		RHI_Command& GetCmd();
		RHI_Command m_empty_cmd; // for GetCmd()
		std::vector<RHI_Command> m_commands;
		std::vector<void*> m_cmd_buffers;
		std::vector<void*> m_semaphores_cmd_list_consumed;
        std::vector<void*> m_fences_in_flight;
		uint32_t m_initial_capacity     = 10000;
		uint32_t m_command_count	    = 0;	
		RHI_Pipeline* m_pipeline	    = nullptr;
		void* m_cmd_pool			    = nullptr;
		uint32_t m_buffer_index		    = 0;
        RHI_Cmd_List_State m_cmd_state  = RHI_Cmd_List_Idle;
	};
}
