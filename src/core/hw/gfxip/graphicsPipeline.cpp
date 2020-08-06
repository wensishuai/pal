/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2014-2020 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#include "core/device.h"
#include "core/platform.h"
#include "core/hw/gfxip/gfxDevice.h"
#include "core/hw/gfxip/graphicsPipeline.h"
#include "palFormatInfo.h"
#include "palMetroHash.h"
#include "palPipelineAbi.h"

using namespace Util;

namespace Pal
{

// =====================================================================================================================
GraphicsPipeline::GraphicsPipeline(
    Device* pDevice,
    bool    isInternal)
    :
    Pal::Pipeline(pDevice, isInternal),
    m_binningOverride(BinningOverride::Default),
    m_vertexBufferCount(0),
    m_numColorTargets(0),
    m_logicOp(LogicOp::Copy)
{
    m_flags.u32All = 0;

    memset(&m_targetSwizzledFormats[0], 0, sizeof(m_targetSwizzledFormats));
    memset(&m_targetWriteMasks[0],      0, sizeof(m_targetWriteMasks));
    memset(&m_viewInstancingDesc,       0, sizeof(m_viewInstancingDesc));
}

// =====================================================================================================================
// Initialize this graphics pipeline based on the provided creation info.
Result GraphicsPipeline::Init(
    const GraphicsPipelineCreateInfo&         createInfo,
    const GraphicsPipelineInternalCreateInfo& internalInfo)
{
    Result result = Result::Success;

    if ((createInfo.pPipelineBinary != nullptr) && (createInfo.pipelineBinarySize != 0))
    {
        m_pipelineBinaryLen = createInfo.pipelineBinarySize;
        m_pPipelineBinary   = PAL_MALLOC(m_pipelineBinaryLen, m_pDevice->GetPlatform(), AllocInternal);
        if (m_pPipelineBinary == nullptr)
        {
            result = Result::ErrorOutOfMemory;
        }
        else
        {
            memcpy(m_pPipelineBinary, createInfo.pPipelineBinary, m_pipelineBinaryLen);
        }
    }
    else
    {
        result = Result::ErrorInvalidPointer;
    }

    if (result == Result::Success)
    {
        PAL_ASSERT(m_pPipelineBinary != nullptr);
        result = InitFromPipelineBinary(createInfo, internalInfo);
    }

    return result;
}

// =====================================================================================================================
// Initializes this pipeline from the pipeline binary data stored in this object, combined with the specified create
// info.
Result GraphicsPipeline::InitFromPipelineBinary(
    const GraphicsPipelineCreateInfo&         createInfo,
    const GraphicsPipelineInternalCreateInfo& internalInfo)
{
    // Store the ROP code this pipeline was created with
    m_logicOp = createInfo.cbState.logicOp;

    m_flags.perpLineEndCapsEnable = createInfo.rsState.perpLineEndCapsEnable;

    m_flags.fastClearElim    = internalInfo.flags.fastClearElim;
    m_flags.fmaskDecompress  = internalInfo.flags.fmaskDecompress;
    m_flags.dccDecompress    = internalInfo.flags.dccDecompress;
    m_flags.resolveFixedFunc = internalInfo.flags.resolveFixedFunc;

    m_binningOverride = createInfo.rsState.binningOverride;

    m_flags.lateAllocVsLimit = createInfo.useLateAllocVsLimit;
    m_lateAllocVsLimit       = createInfo.lateAllocVsLimit;
    m_vertexBufferCount      = createInfo.iaState.vertexBufferCount;
    for (uint8 i = 0; i < MaxColorTargets; ++i)
    {
        m_targetSwizzledFormats[i] = createInfo.cbState.target[i].swizzledFormat;
        m_targetWriteMasks[i]      = createInfo.cbState.target[i].channelWriteMask;
        if ((Formats::IsUndefined(m_targetSwizzledFormats[i].format) == false) ||
            (m_targetWriteMasks[i] != 0))
        {
            m_numColorTargets = i + 1;
        }
    }

    m_viewInstancingDesc                   = createInfo.viewInstancingDesc;
    m_viewInstancingDesc.viewInstanceCount = Max(m_viewInstancingDesc.viewInstanceCount, 1u);

    AbiReader abiReader(m_pDevice->GetPlatform(), m_pPipelineBinary);
    Result result = abiReader.Init();

    MsgPackReader      metadataReader;
    CodeObjectMetadata metadata;

    if (result == Result::Success)
    {
        result = abiReader.GetMetadata(&metadataReader, &metadata);
    }

    if (result == Result::Success)
    {
        ExtractPipelineInfo(metadata, ShaderType::Vertex, ShaderType::Pixel);

        DumpPipelineElf("PipelineGfx",
                        ((metadata.pipeline.hasEntry.name != 0) ? &metadata.pipeline.name[0] : nullptr));

        if (ShaderHashIsNonzero(m_info.shader[static_cast<uint32>(ShaderType::Geometry)].hash))
        {
            m_flags.gsEnabled = 1;
        }
        if (ShaderHashIsNonzero(m_info.shader[static_cast<uint32>(ShaderType::Hull)].hash) &&
            ShaderHashIsNonzero(m_info.shader[static_cast<uint32>(ShaderType::Domain)].hash))
        {
            m_flags.tessEnabled = 1;
        }

        m_flags.vportArrayIdx = (metadata.pipeline.flags.usesViewportArrayIndex != 0);

        const auto& psStageMetadata = metadata.pipeline.hardwareStage[static_cast<uint32>(Abi::HardwareStage::Ps)];

        m_flags.psUsesUavs          = (psStageMetadata.flags.usesUavs          != 0);
        m_flags.psUsesRovs          = (psStageMetadata.flags.usesRovs          != 0);
        m_flags.psWritesUavs        = (psStageMetadata.flags.writesUavs        != 0);
        m_flags.psWritesDepth       = (psStageMetadata.flags.writesDepth       != 0);
        m_flags.psUsesAppendConsume = (psStageMetadata.flags.usesAppendConsume != 0);

        result = HwlInit(createInfo, abiReader, metadata, &metadataReader);
    }

    return result;
}

} // Pal
