/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "cacheLayerBase.h"
#include "palVectorImpl.h"

namespace Util
{

// =====================================================================================================================
CacheLayerBase::CacheLayerBase(
    const AllocCallbacks& callbacks)
    :
    m_allocator   { callbacks },
    m_pNextLayer  { nullptr },
    m_loadPolicy  { LinkPolicy::PassData | LinkPolicy::PassCalls },
    m_storePolicy { LinkPolicy::PassData }
{
    // Alloc and Free MUST NOT be nullptr
    PAL_ASSERT(callbacks.pfnAlloc != nullptr);
    PAL_ASSERT(callbacks.pfnFree != nullptr);

    // pClientData SHOULD not be nullptr
    PAL_ALERT(callbacks.pClientData == nullptr);
}

// =====================================================================================================================
CacheLayerBase::~CacheLayerBase()
{
}

// =====================================================================================================================
// Validate inputs, then attempt to query our layer. On Result::NotFound attempt to query children
Result CacheLayerBase::Query(
    const Hash128* pHashId,
    QueryResult*   pQuery)
{
    Result result = Result::NotFound;

    if ((pHashId == nullptr) ||
        (pQuery == nullptr))
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        if (TestAnyFlagSet(m_loadPolicy, LinkPolicy::Skip) == false)
        {
            result = QueryInternal(pHashId, pQuery);
        }

        if ((result == Result::NotFound) &&
            (m_pNextLayer != nullptr) &&
            (TestAnyFlagSet(m_loadPolicy, LinkPolicy::PassCalls)))
        {
            result = m_pNextLayer->Query(pHashId, pQuery);

            if ((result == Result::Success) &&
                TestAllFlagsSet(m_loadPolicy, LinkPolicy::PassData | LinkPolicy::LoadOnQuery))
            {
                // On successful promotion pQuery may be updated to reflect our layer instead of the original
                Result promoteResult = PromoteData(m_loadPolicy, m_pNextLayer, pQuery);
                PAL_ALERT(IsErrorResult(promoteResult));
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Validate inputs, then store data to our layer. Propagate data down to children if needed.
Result CacheLayerBase::Store(
    const Hash128* pHashId,
    const void*    pData,
    size_t         dataSize)
{
    Result result = Result::Success;

    if ((pHashId == nullptr) ||
        (pData == nullptr))
    {
        result = Result::ErrorInvalidPointer;
    }
    else if (dataSize == 0)
    {
        result = Result::ErrorInvalidValue;
    }
    else
    {
        if (TestAnyFlagSet(m_storePolicy, LinkPolicy::Skip) == false)
        {
            result = StoreInternal(pHashId, pData, dataSize);
        }

        // Pass data to children on success
        if ((IsErrorResult(result) == false) &&
            (m_pNextLayer != nullptr) &&
            TestAnyFlagSet(m_storePolicy, LinkPolicy::PassData))
        {
            Result batchResult = Result::Unsupported;

            if (TestAnyFlagSet(m_storePolicy, LinkPolicy::BatchStore))
            {
                batchResult = BatchData(m_storePolicy, m_pNextLayer, pHashId, pData, dataSize);
            }

            if (batchResult == Result::Unsupported)
            {
                Result childResult = m_pNextLayer->Store(pHashId, pData, dataSize);
                PAL_ALERT(IsErrorResult(childResult));
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Validate inputs, then load data from our layer
Result CacheLayerBase::Load(
    const QueryResult* pQuery,
    void*              pBuffer)
{
    Result result = Result::ErrorUnknown;

    if ((pQuery == nullptr) ||
        (pBuffer == nullptr))
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        if (pQuery->pLayer == this)
        {
            result = LoadInternal(pQuery, pBuffer);
        }
        else
        {
            if ((m_pNextLayer != nullptr) &&
                TestAnyFlagSet(m_loadPolicy, LinkPolicy::PassCalls))
            {
                result = m_pNextLayer->Load(pQuery, pBuffer);

                if ((result == Result::Success) &&
                    TestAnyFlagSet(m_loadPolicy, LinkPolicy::PassData) &&
                    (TestAnyFlagSet(m_loadPolicy, LinkPolicy::LoadOnQuery) == false))
                {
                    // Copy the query since the one passed in cannot be altered
                    QueryResult tmpQuery      = *pQuery;
                    Result      promoteResult = PromoteData(m_loadPolicy, m_pNextLayer, &tmpQuery);
                    PAL_ALERT(IsErrorResult(promoteResult));
                }
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Link another cache layer to ourselves.
Result CacheLayerBase::Link(
    ICacheLayer* pNextLayer)
{
    m_pNextLayer  = pNextLayer;

    return Result::Success;
}

// =====================================================================================================================
// Set the policy to be used on Load calls
Result CacheLayerBase::SetLoadPolicy(
    uint32 loadPolicy)
{
    Result result = Result::Success;

    PAL_ASSERT((loadPolicy & LinkPolicy::BatchStore) == 0u);

    if ((loadPolicy & LinkPolicy::BatchStore) != 0u)
    {
        result = Result::ErrorInvalidValue;
    }

    if (result == Result::Success)
    {
        m_loadPolicy = loadPolicy;
    }

    return result;
}

// =====================================================================================================================
// Set the policy to be used on Store calls
Result CacheLayerBase::SetStorePolicy(
    uint32 storePolicy)
{
    Result result = Result::Success;

    PAL_ASSERT((storePolicy & LinkPolicy::LoadOnQuery) == 0u);

    if ((storePolicy & LinkPolicy::LoadOnQuery) != 0u)
    {
        result = Result::ErrorInvalidValue;
    }

    if (result == Result::Success)
    {
        m_storePolicy = storePolicy;
    }

    return result;
}

} //namespace Util
