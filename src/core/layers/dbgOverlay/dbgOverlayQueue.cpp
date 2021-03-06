/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2015-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "core/layers/dbgOverlay/dbgOverlayCmdBuffer.h"
#include "core/layers/dbgOverlay/dbgOverlayDevice.h"
#include "core/layers/dbgOverlay/dbgOverlayFpsMgr.h"
#include "core/layers/dbgOverlay/dbgOverlayImage.h"
#include "core/layers/dbgOverlay/dbgOverlayPlatform.h"
#include "core/layers/dbgOverlay/dbgOverlayQueue.h"
#include "core/layers/dbgOverlay/dbgOverlayTextWriter.h"
#include "core/layers/dbgOverlay/dbgOverlayTimeGraph.h"
#include "core/g_palPlatformSettings.h"
#include "palAutoBuffer.h"
#include "palDequeImpl.h"

using namespace Util;

namespace Pal
{
namespace DbgOverlay
{

// =====================================================================================================================
Queue::Queue(
    IQueue*    pNextQueue,
    Device*    pDevice,
    QueueType  queueType,
    EngineType engineType)
    :
    QueueDecorator(pNextQueue, pDevice),
    m_pDevice(pDevice),
    m_queueType(queueType),
    m_engineType(engineType),
    m_supportTimestamps(pDevice->GpuProps().engineProperties[engineType].flags.supportsTimestamps),
    m_timestampAlignment(pDevice->GpuProps().engineProperties[engineType].minTimestampAlignment),
    m_timestampMemorySize(2 * MaxGpuTimestampPairCount * m_timestampAlignment),
    m_nextTimestampOffset(0),
    m_pTimestampMemory(nullptr),
    m_gpuTimestampPairDeque(pDevice->GetPlatform())
{
}

// =====================================================================================================================
Queue::~Queue()
{
    Platform*const pPlatform = static_cast<Platform*>(m_pDevice->GetPlatform());

    pPlatform->GetFpsMgr()->NotifyQueueDestroyed(this);

    while (m_gpuTimestampPairDeque.NumElements() > 0)
    {
        GpuTimestampPair* pTimestamp = nullptr;
        m_gpuTimestampPairDeque.PopFront(&pTimestamp);
        DestroyGpuTimestampPair(pTimestamp);
    }

    if (m_pTimestampMemory != nullptr)
    {
        m_pTimestampMemory->Destroy();
        PAL_SAFE_FREE(m_pTimestampMemory, pPlatform);
    }
}

// =====================================================================================================================
Result Queue::Init()
{
    Result result = Result::Success;

    if (m_supportTimestamps)
    {
        result = CreateGpuTimestampPairMemory();
    }

    return result;
}

// =====================================================================================================================
Result Queue::CreateCmdBuffer(
    const CmdBufferCreateInfo& createInfo,
    ICmdBuffer**               ppCmdBuffer)
{
    Result result = Result::ErrorOutOfMemory;

    void*const pMemory = PAL_MALLOC(m_pDevice->GetCmdBufferSize(createInfo, nullptr),
                                    m_pDevice->GetPlatform(),
                                    AllocInternal);

    if (pMemory != nullptr)
    {
        result = m_pDevice->CreateCmdBuffer(createInfo, pMemory, ppCmdBuffer);
    }

    return result;
}

// =====================================================================================================================
Result Queue::CreateFence(
    const FenceCreateInfo& createInfo,
    IFence**               ppFence)
{
    Result result = Result::ErrorOutOfMemory;

    void*const pMemory = PAL_MALLOC(m_pDevice->GetFenceSize(nullptr), m_pDevice->GetPlatform(), AllocInternal);

    if (pMemory != nullptr)
    {
        result = m_pDevice->CreateFence(createInfo, pMemory, ppFence);
    }

    return result;
}

// =====================================================================================================================
// Allocates Gpu Memory for GpuTimestampPair structs
Result Queue::CreateGpuTimestampPairMemory()
{
    Result result = Result::Success;

    GpuMemoryCreateInfo gpuMemoryCreateInfo = {};

    gpuMemoryCreateInfo.size           = m_timestampMemorySize;
    gpuMemoryCreateInfo.vaRange        = VaRange::Default;
    gpuMemoryCreateInfo.heapCount      = 1;
    gpuMemoryCreateInfo.priority       = GpuMemPriority::Normal;
    gpuMemoryCreateInfo.priorityOffset = GpuMemPriorityOffset::Offset0;
    gpuMemoryCreateInfo.heaps[0]       = GpuHeapGartUswc;

    void*const pMemory = PAL_MALLOC(m_pDevice->GetGpuMemorySize(gpuMemoryCreateInfo, nullptr),
                                    m_pDevice->GetPlatform(),
                                    AllocInternal);

    if (pMemory != nullptr)
    {
        result = m_pDevice->CreateGpuMemory(gpuMemoryCreateInfo, pMemory, &m_pTimestampMemory);
    }
    else
    {
        result = Result::ErrorOutOfMemory;
    }

    GpuMemoryRef gpuMemoryRef = {};
    gpuMemoryRef.pGpuMemory = m_pTimestampMemory;

    if (result == Result::Success)
    {
        result = m_pDevice->AddGpuMemoryReferences(1, &gpuMemoryRef, this, GpuMemoryRefCantTrim);
    }

    if (result == Result::Success)
    {
        result = m_pTimestampMemory->Map(&m_pMappedTimestampData);
    }

    return result;
}

// =====================================================================================================================
Result Queue::PresentDirect(
    const PresentDirectInfo& presentInfo)
{
    Result result = Result::Success;

    const Result presentResult = QueueDecorator::PresentDirect(presentInfo);
    result = CollapseResults(presentResult, result);

    if (result == Result::Success)
    {
        Platform*const pPlatform = static_cast<Platform*>(m_pDevice->GetPlatform());

        pPlatform->GetFpsMgr()->IncrementFrameCount();
        pPlatform->ResetGpuWork();
    }

    return result;
}

// =====================================================================================================================
Result Queue::PresentSwapChain(
    const PresentSwapChainInfo& presentInfo)
{
    Result result = Result::Success;

    // Note: We must always call down to the next layer because we must release ownership of the image index.
    const Result presentResult = QueueDecorator::PresentSwapChain(presentInfo);
    result = CollapseResults(presentResult, result);

    if (result == Result::Success)
    {
        Platform*const pPlatform = static_cast<Platform*>(m_pDevice->GetPlatform());

        pPlatform->GetFpsMgr()->IncrementFrameCount();
        pPlatform->ResetGpuWork();
    }

    return result;
}

// =====================================================================================================================
Result Queue::Submit(
    const MultiSubmitInfo& submitInfo)
{
    PAL_ASSERT(submitInfo.perSubQueueInfoCount == 1);
    const auto& gpuProps = m_pDevice->GpuProps();
    Platform* pPlatform = static_cast<Platform*>(m_pDevice->GetPlatform());
    pPlatform->SetGpuWork(gpuProps.gpuIndex, true);

    // Determine if we should add timestamps to this submission.
    bool addTimestamps = m_supportTimestamps                     &&
                        (submitInfo.pPerSubQueueInfo != nullptr) &&
                        (submitInfo.pPerSubQueueInfo[0].cmdBufferCount > 0);
    if (addTimestamps)
    {
        // Other PAL layers assume that CmdPresent can only be in the last command buffer in a submission.
        // If we were to timestamp submissions with presents we would break those layers.
        // We don't timestamp IQueue's present calls either so this should be OK.
        auto*const pLastCmdBuffer = static_cast<CmdBuffer*>(
            submitInfo.pPerSubQueueInfo[0].ppCmdBuffers[submitInfo.pPerSubQueueInfo[0].cmdBufferCount - 1]);

        addTimestamps = (pLastCmdBuffer->ContainsPresent() == false);
    }

    Result result = Result::Success;

    if (addTimestamps)
    {
        // Try to reuse an existing GpuTimestampPair, otherwise create a new one if we still have space for it.
        GpuTimestampPair* pTimestamp = nullptr;

        if ((m_gpuTimestampPairDeque.NumElements() > 0) &&
            (m_gpuTimestampPairDeque.Front()->numActiveSubmissions == 0))
        {
            result = m_gpuTimestampPairDeque.PopFront(&pTimestamp);

            if (result == Result::Success)
            {
                result = m_pDevice->ResetFences(1, &pTimestamp->pFence);
            }
        }
        else if (m_nextTimestampOffset < m_timestampMemorySize)
        {
            result = CreateGpuTimestampPair(&pTimestamp);
        }

        // Immediately push it onto the back of the deque to avoid leaking memory if something fails.
        if (pTimestamp != nullptr)
        {
            // The timestamp should be null if any error occured.
            PAL_ASSERT(result == Result::Success);

            result = m_gpuTimestampPairDeque.PushBack(pTimestamp);

            if (result != Result::Success)
            {
                // We failed to push the timestamp onto the deque. To avoid leaking memory we must delete it.
                DestroyGpuTimestampPair(pTimestamp);
                pTimestamp = nullptr;
            }
        }

        // Submit to the next layer. We should do this even if a failure occured to avoid crashing the application.
        if (pTimestamp != nullptr)
        {
            // The timestamp should be null if any error occured.
            PAL_ASSERT(result == Result::Success);

            result = SubmitWithGpuTimestampPair(submitInfo, pTimestamp);
        }
        else
        {
            const Result submitResult = QueueDecorator::Submit(submitInfo);
            result = CollapseResults(submitResult, result);

            // Notify the FPS manager that we failed to timestamp this submit (the overlay text will reflect this).
            pPlatform->GetFpsMgr()->NotifySubmitWithoutTimestamp();
        }
    }
    else
    {
        result = QueueDecorator::Submit(submitInfo);
    }

    return result;
}

// =====================================================================================================================
Result Queue::SubmitWithGpuTimestampPair(
    const MultiSubmitInfo& submitInfo,
    GpuTimestampPair*      pTimestamp)
{
    // Caller should have made sure that there was at least one command buffer in here.
    PAL_ASSERT((submitInfo.pPerSubQueueInfo != nullptr) && (submitInfo.pPerSubQueueInfo[0].cmdBufferCount > 0));
    Result result = Result::Success;

    Platform* pPlatform = static_cast<Platform*>(m_pDevice->GetPlatform());

    // For a multi-queue submit, we only need to add our timestamps around the primary queue's command buffers.
    // Capacity increased by two to accommodate the two Command Buffers needed to time the submission
    const uint32 cmdBufferCapacity = submitInfo.pPerSubQueueInfo[0].cmdBufferCount + 2;

    AutoBuffer<PerSubQueueSubmitInfo, 16, PlatformDecorator>
        perSubQueueInfo(submitInfo.perSubQueueInfoCount, pPlatform);
    AutoBuffer<ICmdBuffer*,  256, PlatformDecorator> cmdBuffers(cmdBufferCapacity, pPlatform);
    AutoBuffer<CmdBufInfo,   256, PlatformDecorator> cmdBufInfoList(cmdBufferCapacity, pPlatform);

    if ((perSubQueueInfo.Capacity() < submitInfo.perSubQueueInfoCount) ||
        (cmdBuffers.Capacity()      < cmdBufferCapacity)               ||
        (cmdBufInfoList.Capacity()  < cmdBufferCapacity))
    {
        result = Result::ErrorOutOfMemory;
    }
    else
    {
        const IGpuMemory* pNextBlockIfFlipping[MaxBlockIfFlippingCount] = {};
        PAL_ASSERT(submitInfo.blockIfFlippingCount <= MaxBlockIfFlippingCount);

        for (uint32 queueIdx = 0; queueIdx < submitInfo.perSubQueueInfoCount; queueIdx++)
        {
            perSubQueueInfo[queueIdx] = submitInfo.pPerSubQueueInfo[queueIdx];
        }

        PerSubQueueSubmitInfo* pPrimarySubQueue = &perSubQueueInfo[0];
        pPrimarySubQueue->cmdBufferCount        = cmdBufferCapacity;
        pPrimarySubQueue->ppCmdBuffers          = &cmdBuffers[0];
        cmdBuffers[0]                           = pTimestamp->pBeginCmdBuffer;
        for (uint32 i = 0; i < submitInfo.pPerSubQueueInfo[0].cmdBufferCount; i++)
        {
            cmdBuffers[i + 1] = submitInfo.pPerSubQueueInfo[0].ppCmdBuffers[i];
        }
        cmdBuffers[submitInfo.pPerSubQueueInfo[0].cmdBufferCount + 1] = pTimestamp->pEndCmdBuffer;

        if (pPrimarySubQueue->pCmdBufInfoList != nullptr)
        {
            // Note that we must leave pCmdBufInfoList null if it was null in submitInfo.
            pPrimarySubQueue->pCmdBufInfoList = &cmdBufInfoList[0];
            cmdBufInfoList[0].u32All = 0;
            for (uint32 i = 0; i < submitInfo.pPerSubQueueInfo[0].cmdBufferCount; i++)
            {
                const auto& cmdBufInfo = submitInfo.pPerSubQueueInfo[0].pCmdBufInfoList[i];
                cmdBufInfoList[i + 1].u32All = cmdBufInfo.u32All;

                if (cmdBufInfoList[i + 1].isValid)
                {
                    cmdBufInfoList[i + 1].pPrimaryMemory = cmdBufInfo.pPrimaryMemory;
                }
            }
            cmdBufInfoList[submitInfo.pPerSubQueueInfo[0].cmdBufferCount + 1].u32All = 0;
        }

        MultiSubmitInfo finalSubmitInfo  = submitInfo;
        finalSubmitInfo.pPerSubQueueInfo = perSubQueueInfo.Data();

        result = QueueDecorator::Submit(finalSubmitInfo);

        if (result == Result::Success)
        {
            result = AssociateFenceWithLastSubmit(pTimestamp->pFence);
        }

        if (result == Result::Success)
        {
            pPlatform->GetFpsMgr()->UpdateSubmitTimelist(pTimestamp);
        }
    }
    return result;
}

// =====================================================================================================================
// Creates and initializes a new GpuTimestampPair
Result Queue::CreateGpuTimestampPair(
     GpuTimestampPair** ppTimestamp)
{
    Result result = Result::Success;

    GpuTimestampPair* pTimestamp = PAL_NEW(GpuTimestampPair, m_pDevice->GetPlatform(), AllocInternal);

    if (pTimestamp == nullptr)
    {
        result = Result::ErrorOutOfMemory;
    }

    if (result == Result::Success)
    {
        memset(pTimestamp, 0, sizeof(*pTimestamp));

        pTimestamp->pOwner             = this;
        pTimestamp->timestampFrequency = m_pDevice->GpuProps().timestampFrequency;
        Pal::FenceCreateInfo createInfo = {};
        result = CreateFence(createInfo, &pTimestamp->pFence);
    }

    if (result == Result::Success)
    {
        CmdBufferCreateInfo beginCmdBufferCreateInfo = {};
        beginCmdBufferCreateInfo.pCmdAllocator = m_pDevice->InternalCmdAllocator();
        beginCmdBufferCreateInfo.queueType     = m_queueType;
        beginCmdBufferCreateInfo.engineType    = m_engineType;

        result = CreateCmdBuffer(beginCmdBufferCreateInfo, &pTimestamp->pBeginCmdBuffer);
    }

    if (result == Result::Success)
    {
        CmdBufferCreateInfo endCmdBufferCreateInfo = {};
        endCmdBufferCreateInfo.pCmdAllocator = m_pDevice->InternalCmdAllocator();
        endCmdBufferCreateInfo.queueType     = m_queueType;
        endCmdBufferCreateInfo.engineType    = m_engineType;

        result = CreateCmdBuffer(endCmdBufferCreateInfo, &pTimestamp->pEndCmdBuffer);
    }

    if (result == Result::Success)
    {
        CmdBufferBuildInfo cmdBufferBuildInfo = {};
        cmdBufferBuildInfo.flags.optimizeExclusiveSubmit = 1;

        result = pTimestamp->pBeginCmdBuffer->Begin(NextCmdBufferBuildInfo(cmdBufferBuildInfo));
    }

    if (result == Result::Success)
    {
        pTimestamp->pBeginCmdBuffer->CmdWriteTimestamp(HwPipeBottom, *m_pTimestampMemory, m_nextTimestampOffset);
        result = pTimestamp->pBeginCmdBuffer->End();
    }

    if (result == Result::Success)
    {
        pTimestamp->pBeginTimestamp =
            static_cast<uint64*>(Util::VoidPtrInc(m_pMappedTimestampData, m_nextTimestampOffset));
        m_nextTimestampOffset += m_timestampAlignment;

        CmdBufferBuildInfo cmdBufferBuildInfo = {};
        cmdBufferBuildInfo.flags.optimizeExclusiveSubmit = 1;

        result = pTimestamp->pEndCmdBuffer->Begin(NextCmdBufferBuildInfo(cmdBufferBuildInfo));
    }

    if (result == Result::Success)
    {
        pTimestamp->pEndCmdBuffer->CmdWriteTimestamp(HwPipeBottom, *m_pTimestampMemory, m_nextTimestampOffset);
        result = pTimestamp->pEndCmdBuffer->End();
    }

    if (result == Result::Success)
    {
        pTimestamp->pEndTimestamp =
            static_cast<uint64*>(Util::VoidPtrInc(m_pMappedTimestampData, m_nextTimestampOffset));
        m_nextTimestampOffset += m_timestampAlignment;
    }

    if (result == Result::Success)
    {
        *ppTimestamp = pTimestamp;
    }
    else
    {
        DestroyGpuTimestampPair(pTimestamp);
    }

    return result;
}

// =====================================================================================================================
void Queue::DestroyGpuTimestampPair(
    GpuTimestampPair* pTimestamp)
{
    if (pTimestamp->pBeginCmdBuffer != nullptr)
    {
        pTimestamp->pBeginCmdBuffer->Destroy();
        PAL_SAFE_FREE(pTimestamp->pBeginCmdBuffer, m_pDevice->GetPlatform());
    }

    if (pTimestamp->pEndCmdBuffer != nullptr)
    {
        pTimestamp->pEndCmdBuffer->Destroy();
        PAL_SAFE_FREE(pTimestamp->pEndCmdBuffer, m_pDevice->GetPlatform());
    }

    if (pTimestamp->pFence != nullptr)
    {
        pTimestamp->pFence->Destroy();
        PAL_SAFE_FREE(pTimestamp->pFence, m_pDevice->GetPlatform());
    }

    PAL_SAFE_DELETE(pTimestamp, m_pDevice->GetPlatform());
}

} // DbgOverlay
} // Pal
