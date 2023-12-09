// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
namespace D3D12TranslationLayer
{
    template<EShaderStage eShader>
    struct SShaderTraits;

#define SHADER_TRAITS_TRANSLATION_LAYER( initial, name, nameLower ) \
    template<> \
    struct SShaderTraits<e_##initial##S> \
    { \
    static const EDirtyBits c_ShaderResourcesDirty = e_##initial##SShaderResourcesDirty; \
    static const EDirtyBits c_SamplersDirty = e_##initial##SSamplersDirty; \
    static const EDirtyBits c_ConstantBuffersDirty = e_##initial##SConstantBuffersDirty; \
    static ImmediateContext::SStageState& CurrentStageState(ImmediateContext::SState& CurrentState) { return CurrentState.m_##initial##S; } \
    }
    SHADER_TRAITS_TRANSLATION_LAYER(C, COMPUTE, Compute);

    //----------------------------------------------------------------------------------------------------------------------------------
    inline void ImmediateContext::SetShaderResources(UINT StartSlot, __in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) UINT NumSRVs, SRV* const* ppSRVs)
    {
        typedef SShaderTraits<e_CS> TShaderTraits;

        ImmediateContext::SStageState& CurrentStageState = TShaderTraits::CurrentStageState(m_CurrentState);

        for (UINT i = 0; i < NumSRVs; ++i)
        {
            UINT slot = i + StartSlot;
            auto pSRV = ppSRVs[i];
            CurrentStageState.m_SRVs.UpdateBinding(slot, pSRV, e_CS);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    inline void ImmediateContext::SetSamplers(UINT StartSlot, __in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) UINT NumSamplers, Sampler* const* ppSamplers)
    {
        typedef SShaderTraits<e_CS> TShaderTraits;

        ImmediateContext::SStageState& CurrentStageState = TShaderTraits::CurrentStageState(m_CurrentState);

        for (UINT i = 0; i < NumSamplers; ++i)
        {
            UINT slot = i + StartSlot;
            auto pSampler = ppSamplers[i];
            CurrentStageState.m_Samplers.UpdateBinding(slot, pSampler);
        }
    }

    //----------------------------------------------------------------------------------------------------------------------------------
    inline void ImmediateContext::SetConstantBuffers(UINT StartSlot, __in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_HW_SLOT_COUNT) UINT NumBuffers, Resource* const* ppCBs, __in_ecount_opt(NumBuffers) CONST UINT* pFirstConstant, __in_ecount_opt(NumBuffers) CONST UINT* pNumConstants)
    {
        typedef SShaderTraits<e_CS> TShaderTraits;

        ImmediateContext::SStageState& CurrentStageState = TShaderTraits::CurrentStageState(m_CurrentState);

        for (UINT i = 0; i < NumBuffers; ++i)
        {
            UINT slot = i + StartSlot;
            Resource* pCB = ppCBs[i];
            CurrentStageState.m_CBs.UpdateBinding(slot, pCB, e_CS);

            UINT prevFirstConstant = CurrentStageState.m_uConstantBufferOffsets[slot];
            UINT prevNumConstants = CurrentStageState.m_uConstantBufferCounts[slot];

            UINT newFirstConstant = pFirstConstant ? pFirstConstant[i] : 0;
            UINT newNumConstants = pNumConstants ? pNumConstants[i] : D3D10_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;

            if (prevFirstConstant != newFirstConstant || prevNumConstants != newNumConstants)
            {
                CurrentStageState.m_CBs.SetDirtyBit(slot);
            }

            CurrentStageState.m_uConstantBufferOffsets[slot] = newFirstConstant;
            CurrentStageState.m_uConstantBufferCounts[slot] = newNumConstants;
        }
    }
};