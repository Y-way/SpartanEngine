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

//= INCLUDES =====================
#include "RHI_Pipeline.h"
#include "RHI_Device.h"
#include "RHI_Texture.h"
#include "RHI_Sampler.h"
#include "RHI_PipelineState.h"
#include "RHI_ConstantBuffer.h"
#include "RHI_Shader.h"
#include "RHI_Implementation.h"
#include "..\Rendering\Renderer.h"
#include "..\Utilities\Hash.h"
//================================

//= NAMESPACES =====
using namespace std;
//==================

namespace Spartan
{
    void RHI_Pipeline::SetConstantBuffer(uint32_t slot, RHI_ConstantBuffer* constant_buffer)
    {
        for (RHI_Descriptor& descriptor : m_descriptors)
        {
            if (descriptor.type == RHI_Descriptor_ConstantBuffer && descriptor.slot == slot + m_rhi_device->GetContextRhi()->shader_shift_buffer)
            {
                m_descriptor_dirty = descriptor.id != constant_buffer->GetId() ? true : m_descriptor_dirty;

                // Update
                descriptor.id       = constant_buffer->GetId();
                descriptor.resource = constant_buffer->GetResource();
                descriptor.size     = constant_buffer->GetSize();
                
                break;
            }
        }
    }

    void RHI_Pipeline::SetSampler(uint32_t slot, RHI_Sampler* sampler)
    {
        for (RHI_Descriptor& descriptor : m_descriptors)
        {
            if (descriptor.type == RHI_Descriptor_Sampler && descriptor.slot == slot + m_rhi_device->GetContextRhi()->shader_shift_sampler)
            {
                m_descriptor_dirty = descriptor.id != sampler->GetId() ? true : m_descriptor_dirty;

                // Update
                descriptor.id       = sampler->GetId();
                descriptor.resource = sampler->GetResource();

                break;
            }
        }
    }

    void RHI_Pipeline::SetTexture(uint32_t slot, RHI_Texture* texture)
    {
        for (RHI_Descriptor& descriptor : m_descriptors)
        {
            if (descriptor.type == RHI_Descriptor_Texture && descriptor.slot == slot + m_rhi_device->GetContextRhi()->shader_shift_texture)
            {
                if (!texture->IsSampled())
                {
                    LOG_ERROR("This texture can be sampled");
                    return;
                }

                m_descriptor_dirty = descriptor.id != texture->GetId() ? true : m_descriptor_dirty;

                // Update
                descriptor.id           = texture->GetId();
                descriptor.resource     = texture->GetResource_View();
                descriptor.layout       = texture->GetLayout();
                descriptor.user_data    = static_cast<void*>(texture);

                break;
            }
        }
    }

    void* RHI_Pipeline::GetDescriptorSet()
    {
        // Get the hash of the current descriptor blueprint
        size_t hash = GetDescriptorBlueprintHash(m_descriptors);

        // If the has is already present, then we don't need to update
        if (m_descriptor_resources.find(hash) != m_descriptor_resources.end())
        {
            if (m_descriptor_dirty)
            {
                m_descriptor_dirty = false;
                return m_descriptor_resources[hash];
            }

            return nullptr;
        }

        // Otherwise generate a new one and return that
        return CreateDescriptorSet(hash);
    }

    size_t RHI_Pipeline::GetDescriptorBlueprintHash(const std::vector<RHI_Descriptor>& descriptor_blueprint)
    {
        size_t hash = 0;

        for (const RHI_Descriptor& descriptor : m_descriptors)
        {
            Utility::Hash::hash_combine(hash, descriptor.slot);
            Utility::Hash::hash_combine(hash, descriptor.stage);
            Utility::Hash::hash_combine(hash, descriptor.id);
            Utility::Hash::hash_combine(hash, descriptor.size);
            Utility::Hash::hash_combine(hash, static_cast<uint32_t>(descriptor.type));
            Utility::Hash::hash_combine(hash, static_cast<uint32_t>(descriptor.layout));
        }

        return hash;
    }

    void RHI_Pipeline::ReflectShaders()
    {
        m_descriptors.clear();

        if (!m_state.shader_vertex)
        {
            LOG_ERROR("Vertex shader is invalid");
            return;
        }

        // Get vertex shader resources
        while (m_state.shader_vertex->GetCompilationState() == Shader_Compilation_Compiling) {}
        for (const RHI_Descriptor& descriptor : m_state.shader_vertex->GetDescriptors())
        {
            m_descriptors.emplace_back(descriptor);
        }

        // If there is a pixel shader, merge it's resources into our map as well
        if (m_state.shader_pixel)
        {
            while (m_state.shader_pixel->GetCompilationState() == Shader_Compilation_Compiling) {}
            for (const RHI_Descriptor& descriptor_reflected : m_state.shader_pixel->GetDescriptors())
            {
                // Assume that the descriptor has been created in the vertex shader and only try to update it's shader stage
                bool updated_existing = false;
                for (RHI_Descriptor& descriptor : m_descriptors)
                {
                    bool is_same_resource =
                        (descriptor.type == descriptor_reflected.type) &&
                        (descriptor.slot == descriptor_reflected.slot);

                    if ((descriptor.type == descriptor_reflected.type) && (descriptor.slot == descriptor_reflected.slot))
                    {
                        descriptor.stage |= descriptor_reflected.stage;
                        updated_existing = true;
                        break;
                    }
                }

                // If no updating took place, this descriptor is new, so add it
                if (!updated_existing)
                {
                    m_descriptors.emplace_back(descriptor_reflected);
                }
            }
        }
    }

    void RHI_Pipeline::RevertTextureLayouts(RHI_CommandList* cmd_list)
    {
        /*for (RHI_Descriptor& descriptor : m_descriptors)
        {
            if (descriptor.revert_layout)
            {
                if (RHI_Texture* texture = static_cast<RHI_Texture*>(descriptor.user_data))
                {
                    if (descriptor.type == RHI_Descriptor_Texture && descriptor.id == texture->GetId())
                    {
                        if (texture->GetLayout() != previous_layout)
                        {
                            texture->SetLayout(previous_layout, cmd_list);
                        }
                    }
                }

                descriptor.revert_layout = false;
            }
        }*/
    }
}
