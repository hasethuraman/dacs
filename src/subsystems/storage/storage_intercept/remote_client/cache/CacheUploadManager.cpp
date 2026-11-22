/// Copyright (c) Microsoft Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "CacheUploadManager.h"
#include "StorageInterceptConfig.h"
#include "StorageInterceptLogger.h"
#include "StorageInterceptConstants.h"
#include "cache.pb.h"
#include "CacheUtils.h"
#include "ICacheClient.h"
#include "UploadDiagnostics.h"
#include <limits>
#include <thread>
#include <utility>
//---------------------------------------------------------------------------------------------------------------------
using namespace cache;
using namespace cacheutils;
using namespace std;
using namespace StorageInterceptConstants;
//---------------------------------------------------------------------------------------------------------------------
CacheUploadManager::CacheUploadManager(
    std::shared_ptr<CacheClientImpl> spCacheClientImpl,
    int numUploadThreads,
    uint64_t maxTotalBufferSize)
        : m_uploadBufferMemory(0),
      m_maxBufferMemory(maxTotalBufferSize),
      m_keyExpirySeconds(StorageInterceptConfig::GetInstance().GetCacheKeyExpirySeconds()),
      m_keyGroupId(StorageInterceptConfig::GetInstance().GetCacheKeyGroupId()),
      m_enableSyncUpload(StorageInterceptConfig::GetInstance().GetCacheEnableSyncUpload()),
            m_bEnableVersionedCacheKeys(StorageInterceptConfig::GetInstance().IsVersionedCacheKeysEnabled()),
            m_numUploadThreads(numUploadThreads),
            m_stop(false),
            m_spCacheClientImpl(std::move(spCacheClientImpl))
{
    if (uploaddiag::Enabled())
    {
        static std::atomic<uint64_t> nextId{0};
        m_diagnosticId = nextId.fetch_add(1) + 1;
        std::fprintf(stderr, "DACS_CPU_MANAGER pid=%ld manager=%llu workers=%d synchronous=%d\n",
            static_cast<long>(getpid()), (unsigned long long)m_diagnosticId,
            m_numUploadThreads, m_enableSyncUpload);
    }
    // start worker threads
    if (!m_enableSyncUpload)
    {
        for (int i = 0; i < m_numUploadThreads; ++i)
        {
            auto uploadThread = make_unique<thread>([this]() {
                this->UploadWorker();
            });
            m_vecUploadThread.push_back(std::move(uploadThread));
        }
    }

    LOG_INFO("Started CacheUploadManager. workers={}, max uploadbuffer={} bytes, expiry={}, groupId={} asyncupload={}",
        m_numUploadThreads,
        m_maxBufferMemory,
        m_keyExpirySeconds,
        m_keyGroupId,
        m_enableSyncUpload ? "false" : "true");
}
//---------------------------------------------------------------------------------------------------------------------
CacheUploadManager::~CacheUploadManager()
{
    if (!m_enableSyncUpload)
    {
        LOG_INFO("Stopping uploader threads");
        m_stop = true;
        m_cvQueue.notify_all();
        for (auto& uploadThread : m_vecUploadThread)
        {
            uploadThread->join();
        }
    }
    if (uploaddiag::Enabled())
        std::fprintf(stderr, "DACS_CPU_MANAGER_FINAL pid=%ld manager=%llu requests=%llu bytes=%llu failures=%llu pending_bytes=%llu\n",
            static_cast<long>(getpid()), (unsigned long long)m_diagnosticId,
            (unsigned long long)m_diagnosticRequests.load(), (unsigned long long)m_diagnosticBytes.load(),
            (unsigned long long)m_diagnosticFailures.load(), (unsigned long long)m_uploadBufferMemory.load());
}
//---------------------------------------------------------------------------------------------------------------------
bool CacheUploadManager::StartUpload(unique_ptr<UploadContext> uploadContext)
{
    uploaddiag::Scope uploadTimer(uploaddiag::Upload,
        uploadContext->m_fileData ? uploadContext->m_fileData->BufferSize : 0);
    // Long enough to hold string representation of std::size_t in base 10 (18446744073709551615)
    // which is 21 characters
    static thread_local char arrFileSizeBuffer[32];
    static thread_local char arrATimeBuffer[32];
    static thread_local char arrMTimeBuffer[32];

    static std::string strTrue("true");
    static std::string strFalse("false");

    LOG_DEBUG("Uploading mountName: {}, file: {}, length: {}, fileAttributes: {} attr-size {}",
              uploadContext->m_strMountName,
              uploadContext->m_strCacheKey,
              uploadContext->m_fileData.get() ? uploadContext->m_fileData->BufferSize : 0,
              uploadContext->m_fileAttributes.get() ? "yes" : "no",
              uploadContext->m_fileAttributes.get() ? uploadContext->m_fileAttributes->size : -1);

    Metadata metadata;
    std::vector<KeyValuePair> metadataEntries;
    if(uploadContext->m_fileAttributes.get())
    {
        metadataEntries.push_back({(char*)CACHE_META_FILE_ISDIR,
            (char*)(uploadContext->m_fileAttributes->isDir ? strTrue.c_str() : strFalse.c_str())});

        snprintf(arrFileSizeBuffer, sizeof(arrFileSizeBuffer), "%ld", uploadContext->m_fileAttributes->size);
        metadataEntries.push_back({(char*)CACHE_META_FILE_SIZE, arrFileSizeBuffer});

        snprintf(arrATimeBuffer, sizeof(arrATimeBuffer), "%ld", uploadContext->m_fileAttributes->aTime.tv_sec);
        metadataEntries.push_back({(char*)CACHE_META_FILE_ATIME, arrATimeBuffer});

        snprintf(arrMTimeBuffer, sizeof(arrMTimeBuffer), "%ld", uploadContext->m_fileAttributes->mTime.tv_sec);
        metadataEntries.push_back({(char*)CACHE_META_FILE_MTIME, arrMTimeBuffer});

        if (!uploadContext->m_fileAttributes->version.empty())
        {
            metadataEntries.push_back({(char*)CACHE_META_FILE_VERSION,
                (char*)uploadContext->m_fileAttributes->version.c_str()});
        }

        metadata.arrKeyValuePairs = metadataEntries.data();
        if (metadataEntries.size() > std::numeric_limits<uint32_t>::max())
        {
            LOG_ERROR("Cannot upload metadata: entry count {} exceeds the supported maximum {}",
                metadataEntries.size(), std::numeric_limits<uint32_t>::max());
            return false;
        }
        metadata.nKeyValuePairs = static_cast<uint32_t>(metadataEntries.size());
    }
    else
    {
        metadata.arrKeyValuePairs = nullptr;
        metadata.nKeyValuePairs = 0;
    }

    char* bufferToSend = nullptr;
    size_t bufferSizeToSend = 0;
    if(uploadContext->m_fileData.get())
    {
        bufferToSend = uploadContext->m_fileData->Buffer.get();
        bufferSizeToSend = uploadContext->m_fileData->BufferSize;
    }

    // Derive per-file groupId from version if provided, otherwise fall back to global config groupId
    const std::string effectiveGroupId = GetEffectiveGroupId(
        uploadContext->m_strFilePath, uploadContext->m_strVersion);

    // When groupId is set, lifecycle is managed by DeleteGroup(), expiry is not used.
    // The server ignores expiry when groupId is present, so avoid sending both.
    const uint32_t expirySeconds = effectiveGroupId.empty() ? m_keyExpirySeconds : 0;

    auto status = m_spCacheClientImpl->Upload(
        uploadContext->m_strCacheKey.c_str(),
        bufferToSend,
        bufferSizeToSend,
        expirySeconds,
        effectiveGroupId.empty() ? nullptr : effectiveGroupId.c_str(),
        metadata.nKeyValuePairs > 0 ? &metadata : nullptr,
        uploadContext->m_encryptionKey.empty() ? nullptr : uploadContext->m_encryptionKey.c_str());

    if (uploaddiag::Enabled())
    {
        m_diagnosticRequests.fetch_add(1);
        m_diagnosticBytes.fetch_add(bufferSizeToSend);
        if (status != CacheClientStatus::SUCCESS)
        {
            m_diagnosticFailures.fetch_add(1);
            uploaddiag::failures.fetch_add(1);
        }
    }
    return (status == CacheClientStatus::SUCCESS);
}
//---------------------------------------------------------------------------------------------------------------------
void CacheUploadManager::UploadWorker()
{
    uploaddiag::RoleScope role(uploaddiag::Uploader);
    LOG_DEBUG("Started UploadWorker");
    while (!m_stop)
    {
        std::unique_lock<std::mutex> lock(m_mutexQueue);
        uploaddiag::Scope idleTimer(uploaddiag::UploadIdleWait);
        m_cvQueue.wait(lock,
            [this]
            {
                return this->m_stop || (this->m_uploadQueue.size() > 0);
            });
        idleTimer.Stop();
        if (m_stop)
        {
            break;
        }

        unique_ptr<UploadContext> uploadContext;
        if (m_uploadQueue.size() > 0)
        {
            uploadContext = std::move(m_uploadQueue.front());
            m_uploadQueue.pop();
        }

        lock.unlock();

        if (uploadContext != nullptr)
        {
            size_t bufferSizeToSend = uploadContext->m_fileData.get() ? uploadContext->m_fileData->BufferSize : 0;
            if (uploaddiag::Enabled())
            {
                if (uploadContext->m_diagnosticQueuedNs != 0)
                    uploaddiag::Record(uploaddiag::QueueWait, bufferSizeToSend,
                        uploaddiag::Now() - uploadContext->m_diagnosticQueuedNs);
                uploaddiag::pendingBytes.fetch_sub(bufferSizeToSend);
            }
            StartUpload(std::move(uploadContext));
            m_uploadBufferMemory -= bufferSizeToSend;
        }
    }

    LOG_DEBUG("Shutdown UploadWorker");
}
//---------------------------------------------------------------------------------------------------------------------
bool CacheUploadManager::Upload(
    const std::string& mountName,
    const std::string& cacheKey,
    std::shared_ptr<const FileData> fileData,
    const std::string& strEncryptionKey,
    std::unique_ptr<::FileAttributes> fileAttributes,
    bool isWrite,
    const std::string& filePath,
    const std::string& version)
{
    uploaddiag::Scope enqueueTimer(uploaddiag::Enqueue,
        fileData ? fileData->BufferSize : 0);
    bool useSync = m_enableSyncUpload || isWrite; // Writes to cache are always sync

    if (useSync)
    {
        auto uploadContext = make_unique<UploadContext>(
            mountName, cacheKey, fileData, strEncryptionKey,
            std::move(fileAttributes), filePath, version);
        return StartUpload(std::move(uploadContext));
    }
    else
    {
        STORAGE_ASSERT(!(fileData.get() && fileData->bIsUserBuffer), "Async upload attempted with user buffer!!");
        size_t bufferSizeToSend = fileData.get() ? fileData->BufferSize : 0;
        {
            lock_guard lock(m_mutexQueue);
            if ((m_uploadBufferMemory + bufferSizeToSend) > m_maxBufferMemory)
            {
                LOG_RARELY(10, WARN, "Reached upload size limit, skipping. cacheKey={}, size={}, upload pending={}",
                    cacheKey, bufferSizeToSend, m_uploadBufferMemory);
                return false;
            }

            m_uploadBufferMemory += bufferSizeToSend;
            auto uploadContext = make_unique<UploadContext>(
                mountName, cacheKey, fileData, strEncryptionKey,
                std::move(fileAttributes), filePath, version);
            if (uploaddiag::Enabled())
            {
                uploadContext->m_diagnosticQueuedNs = uploaddiag::Now();
                uploaddiag::Max(uploaddiag::peakPendingBytes,
                    uploaddiag::pendingBytes.fetch_add(bufferSizeToSend) + bufferSizeToSend);
            }
            m_uploadQueue.push(std::move(uploadContext));
        }

        m_cvQueue.notify_one();
    }
    return true;
}
//---------------------------------------------------------------------------------------------------------------------
std::string CacheUploadManager::GetEffectiveGroupId(
    const std::string& filePath, const std::string& version) const
{
    if (m_bEnableVersionedCacheKeys && !version.empty())
    {
        return cacheutils::GetGroupIdForFileVersion(filePath, version);
    }
    return m_keyGroupId;
}
//---------------------------------------------------------------------------------------------------------------------
