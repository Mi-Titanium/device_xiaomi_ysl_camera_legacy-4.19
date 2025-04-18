/* Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#define LOG_TAG "QCameraHWI_Mem"

// System dependencies
#include <fcntl.h>
#include <stdio.h>
#include <utils/Errors.h>
#define MMAN_H <SYSTEM_HEADER_PREFIX/mman.h>
#include MMAN_H
#include "hardware/gralloc.h"
#include "gralloc_priv.h"

// Camera dependencies
#include "QCamera2HWI.h"
#include "QCameraMem.h"
#include "QCameraParameters.h"
#include "QCameraTrace.h"

// Media dependencies
#ifdef USE_MEDIA_EXTENSIONS
#include <media/hardware/HardwareAPI.h>
typedef struct VideoNativeHandleMetadata media_metadata_buffer;
#else
#include "QComOMXMetadata.h"
typedef struct encoder_media_buffer_type media_metadata_buffer;
#endif

extern "C" {
#include "mm_camera_dbg.h"
#include "mm_camera_interface.h"
}

using namespace android;

namespace qcamera {

// QCaemra2Memory base class

/*===========================================================================
 * FUNCTION   : QCameraMemory
 *
 * DESCRIPTION: default constructor of QCameraMemory
 *
 * PARAMETERS :
 *   @cached  : flag indicates if using cached memory
 *
 * RETURN     : None
 *==========================================================================*/
QCameraMemory::QCameraMemory(bool cached,
        QCameraMemoryPool *pool,
        cam_stream_type_t streamType, QCameraMemType bufType)
    :m_bCached(cached),
     mMemoryPool(pool),
     mStreamType(streamType),
     mBufType(bufType)
{
    mBufferCount = 0;
    reset();
}

/*===========================================================================
 * FUNCTION   : ~QCameraMemory
 *
 * DESCRIPTION: deconstructor of QCameraMemory
 *
 * PARAMETERS : none
 *
 * RETURN     : None
 *==========================================================================*/
QCameraMemory::~QCameraMemory()
{
}

/*===========================================================================
 * FUNCTION   : cacheOpsInternal
 *
 * DESCRIPTION: ion related memory cache operations
 *
 * PARAMETERS :
 *   @index   : index of the buffer
 *   @cmd     : cache ops command
 *   @vaddr   : ptr to the virtual address
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraMemory::cacheOpsInternal(uint32_t index, unsigned int cmd, void *vaddr)
{
    if (!m_bCached) {
        // Memory is not cached, no need for cache ops
        LOGD("No cache ops here for uncached memory");
        return OK;
    }

    int ret = OK;
#ifndef TARGET_ION_ABI_VERSION
    struct ion_flush_data cache_inv_data;
    struct ion_custom_data custom_data;

    if (index >= mBufferCount) {
        LOGE("index %d out of bound [0, %d)", index, mBufferCount);
        return BAD_INDEX;
    }

    memset(&cache_inv_data, 0, sizeof(cache_inv_data));
    memset(&custom_data, 0, sizeof(custom_data));
    cache_inv_data.vaddr = vaddr;
    cache_inv_data.fd = mMemInfo[index].fd;
    cache_inv_data.handle = mMemInfo[index].handle;
    cache_inv_data.length =
            ( /* FIXME: Should remove this after ION interface changes */ unsigned int)
            mMemInfo[index].size;
    custom_data.cmd = cmd;
    custom_data.arg = (unsigned long)&cache_inv_data;

    LOGH("addr = %p, fd = %d, handle = %lx length = %d, ION Fd = %d",
          cache_inv_data.vaddr, cache_inv_data.fd,
         (unsigned long)cache_inv_data.handle, cache_inv_data.length,
         mMemInfo[index].main_ion_fd);
    ret = ioctl(mMemInfo[index].main_ion_fd, ION_IOC_CUSTOM, &custom_data);
#else
    (void)index;
    (void)cmd;
    (void)vaddr;
    ret = NO_ERROR; // ioctl(mMemInfo[index].main_ion_fd, ION_IOC_CUSTOM, &custom_data);
#endif //TARGET_ION_ABI_VERSION
    if (ret < 0) {
        LOGE("Cache Invalidate failed: %s\n", strerror(errno));
    }

    return ret;
}

/*===========================================================================
 * FUNCTION   : getFd
 *
 * DESCRIPTION: return file descriptor of the indexed buffer
 *
 * PARAMETERS :
 *   @index   : index of the buffer
 *
 * RETURN     : file descriptor
 *==========================================================================*/
int QCameraMemory::getFd(uint32_t index) const
{
    if (index >= mBufferCount)
        return BAD_INDEX;

    return mMemInfo[index].fd;
}

/*===========================================================================
 * FUNCTION   : getSize
 *
 * DESCRIPTION: return buffer size of the indexed buffer
 *
 * PARAMETERS :
 *   @index   : index of the buffer
 *
 * RETURN     : buffer size
 *==========================================================================*/
ssize_t QCameraMemory::getSize(uint32_t index) const
{
    if (index >= mBufferCount)
        return BAD_INDEX;

    return (ssize_t)mMemInfo[index].size;
}

/*===========================================================================
 * FUNCTION   : getCnt
 *
 * DESCRIPTION: query number of buffers allocated
 *
 * PARAMETERS : none
 *
 * RETURN     : number of buffers allocated
 *==========================================================================*/
uint8_t QCameraMemory::getCnt() const
{
    return mBufferCount;
}

/*===========================================================================
 * FUNCTION   : reset
 *
 * DESCRIPTION: reset member variables
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraMemory::reset()
{
    size_t i, count;

    memset(mMemInfo, 0, sizeof(mMemInfo));

    count = sizeof(mMemInfo) / sizeof(mMemInfo[0]);
    for (i = 0; i < count; i++) {
        mMemInfo[i].fd = -1;
        mMemInfo[i].main_ion_fd = -1;
    }

    return;
}

/*===========================================================================
 * FUNCTION   : getMappable
 *
 * DESCRIPTION: query number of buffers available to map
 *
 * PARAMETERS : none
 *
 * RETURN     : number of buffers available to map
 *==========================================================================*/
uint8_t QCameraMemory::getMappable() const
{
    return mBufferCount;
}

/*===========================================================================
 * FUNCTION   : checkIfAllBuffersMapped
 *
 * DESCRIPTION: query if all buffers are mapped
 *
 * PARAMETERS : none
 *
 * RETURN     : 1 as buffer count is always equal to mappable count
 *==========================================================================*/
uint8_t QCameraMemory::checkIfAllBuffersMapped() const
{
    return 1;
}


/*===========================================================================
 * FUNCTION   : getBufDef
 *
 * DESCRIPTION: query detailed buffer information
 *
 * PARAMETERS :
 *   @offset  : [input] frame buffer offset
 *   @bufDef  : [output] reference to struct to store buffer definition
 *   @index   : [input] index of the buffer
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraMemory::getBufDef(const cam_frame_len_offset_t &offset,
        mm_camera_buf_def_t &bufDef, uint32_t index) const
{
    if (!mBufferCount) {
        LOGE("Memory not allocated");
        return;
    }
    bufDef.fd = mMemInfo[index].fd;
    bufDef.frame_len = mMemInfo[index].size;
    bufDef.buf_type = CAM_STREAM_BUF_TYPE_MPLANE;
    bufDef.mem_info = (void *)this;
    bufDef.planes_buf.num_planes = (int8_t)offset.num_planes;
    bufDef.buffer = getPtr(index);
    bufDef.buf_idx = index;

    /* Plane 0 needs to be set separately. Set other planes in a loop */
    bufDef.planes_buf.planes[0].length = offset.mp[0].len;
    bufDef.planes_buf.planes[0].m.userptr = (long unsigned int)mMemInfo[index].fd;
    bufDef.planes_buf.planes[0].data_offset = offset.mp[0].offset;
    bufDef.planes_buf.planes[0].reserved[0] = 0;
    for (int i = 1; i < bufDef.planes_buf.num_planes; i++) {
         bufDef.planes_buf.planes[i].length = offset.mp[i].len;
         bufDef.planes_buf.planes[i].m.userptr = (long unsigned int)mMemInfo[i].fd;
         bufDef.planes_buf.planes[i].data_offset = offset.mp[i].offset;
         bufDef.planes_buf.planes[i].reserved[0] =
                 bufDef.planes_buf.planes[i-1].reserved[0] +
                 bufDef.planes_buf.planes[i-1].length;
    }
}

/*===========================================================================
 * FUNCTION   : getUserBufDef
 *
 * DESCRIPTION: Fill Buffer structure with user buffer information
                           This also fills individual stream buffers inside batch baffer strcuture
 *
 * PARAMETERS :
 *   @buf_info : user buffer information
 *   @bufDef  : Buffer strcuture to fill user buf info
 *   @index   : index of the buffer
 *   @plane_offset : plane buffer information
 *   @planeBufDef  : [input] frame buffer offset
 *   @bufs    : Stream Buffer object
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraMemory::getUserBufDef(const cam_stream_user_buf_info_t &buf_info,
        mm_camera_buf_def_t &bufDef,
        uint32_t index,
        const cam_frame_len_offset_t &plane_offset,
        mm_camera_buf_def_t *planeBufDef,
        QCameraMemory *bufs) const
{
    struct msm_camera_user_buf_cont_t *cont_buf = NULL;
    uint32_t plane_idx = (index * buf_info.frame_buf_cnt);

    if (!mBufferCount) {
        LOGE("Memory not allocated");
        return INVALID_OPERATION;
    }

    for (int count = 0; count < mBufferCount; count++) {
        bufDef.fd = mMemInfo[count].fd;
        bufDef.buf_type = CAM_STREAM_BUF_TYPE_USERPTR;
        bufDef.frame_len = buf_info.size;
        bufDef.mem_info = (void *)this;
        bufDef.buffer = (void *)((uint8_t *)getPtr(count)
                + (index * buf_info.size));
        bufDef.buf_idx = index;
        bufDef.user_buf.num_buffers = (int8_t)buf_info.frame_buf_cnt;
        bufDef.user_buf.bufs_used = (int8_t)buf_info.frame_buf_cnt;

        //Individual plane buffer structure to be filled
        cont_buf = (struct msm_camera_user_buf_cont_t *)bufDef.buffer;
        cont_buf->buf_cnt = bufDef.user_buf.num_buffers;

        for (int i = 0; i < bufDef.user_buf.num_buffers; i++) {
            bufs->getBufDef(plane_offset, planeBufDef[plane_idx], plane_idx);
            bufDef.user_buf.buf_idx[i] = -1;
            cont_buf->buf_idx[i] = planeBufDef[plane_idx].buf_idx;
            plane_idx++;
        }
        bufDef.user_buf.plane_buf = planeBufDef;

        LOGD("num_buf = %d index = %d plane_idx = %d",
                 bufDef.user_buf.num_buffers, index, plane_idx);
    }
    return NO_ERROR;
}


/*===========================================================================
 * FUNCTION   : alloc
 *
 * DESCRIPTION: allocate requested number of buffers of certain size
 *
 * PARAMETERS :
 *   @count   : number of buffers to be allocated
 *   @size    : lenght of the buffer to be allocated
 *   @heap_id : heap id to indicate where the buffers will be allocated from
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraMemory::alloc(int count, size_t size, unsigned int heap_id,
        uint32_t secure_mode)
{
    int rc = OK;

    int new_bufCnt = mBufferCount + count;
    ATRACE_BEGIN_SNPRINTF("%s %zu %d", "Memsize", size, count);

    if (new_bufCnt > MM_CAMERA_MAX_NUM_FRAMES) {
        LOGE("Buffer count %d out of bound. Max is %d",
               new_bufCnt, MM_CAMERA_MAX_NUM_FRAMES);
        ATRACE_END();
        return BAD_INDEX;
    }

    for (int i = mBufferCount; i < new_bufCnt; i ++) {
        if ( NULL == mMemoryPool ) {
            LOGH("No memory pool available, allocating now");
            rc = allocOneBuffer(mMemInfo[i], heap_id, size, m_bCached,
                     secure_mode);
            if (rc < 0) {
                LOGE("AllocateIonMemory failed");
                for (int j = i-1; j >= 0; j--)
                    deallocOneBuffer(mMemInfo[j]);
                break;
            }
        } else {
            rc = mMemoryPool->allocateBuffer(mMemInfo[i],
                                             heap_id,
                                             size,
                                             m_bCached,
                                             mStreamType,
                                             secure_mode);
            if (rc < 0) {
                LOGE("Memory pool allocation failed");
                for (int j = i-1; j >= 0; j--)
                    mMemoryPool->releaseBuffer(mMemInfo[j],
                                               mStreamType);
                break;
            }
        }

    }
    ATRACE_END();
    return rc;
}

/*===========================================================================
 * FUNCTION   : dealloc
 *
 * DESCRIPTION: deallocate buffers
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraMemory::dealloc()
{
    for (int i = 0; i < mBufferCount; i++) {
        if ( NULL == mMemoryPool ) {
            deallocOneBuffer(mMemInfo[i]);
        } else {
            mMemoryPool->releaseBuffer(mMemInfo[i], mStreamType);
        }
    }
}

/*===========================================================================
 * FUNCTION   : allocOneBuffer
 *
 * DESCRIPTION: impl of allocating one buffers of certain size
 *
 * PARAMETERS :
 *   @memInfo : [output] reference to struct to store additional memory allocation info
 *   @heap    : [input] heap id to indicate where the buffers will be allocated from
 *   @size    : [input] lenght of the buffer to be allocated
 *   @cached  : [input] flag whether buffer needs to be cached
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraMemory::allocOneBuffer(QCameraMemInfo &memInfo,
        unsigned int heap_id, size_t size, bool cached, uint32_t secure_mode)
{
    int rc = OK;
    int main_ion_fd = -1;
    struct ion_allocation_data alloc;
    struct ion_fd_data ion_info_fd;

#ifndef TARGET_ION_ABI_VERSION
    struct ion_handle_data handle_data;
    main_ion_fd = open("/dev/ion", O_RDONLY);
#else
    main_ion_fd = ion_open();
#endif
    if (main_ion_fd < 0) {
        LOGE("Ion dev open failed: %s\n", strerror(errno));
        goto ION_OPEN_FAILED;
    }

    memset(&ion_info_fd, 0, sizeof(ion_info_fd));
    memset(&alloc, 0, sizeof(alloc));
    alloc.len = size;
    /* to make it page size aligned */
    alloc.len = (alloc.len + 4095U) & (~4095U);
    alloc.align = 4096;
    if (cached) {
        alloc.flags = ION_FLAG_CACHED;
    }
    alloc.heap_id_mask = heap_id;
    if (secure_mode == SECURE) {
        LOGD("Allocate secure buffer\n");
        alloc.flags = ION_FLAG_SECURE;
        alloc.heap_id_mask = ION_HEAP(ION_CP_MM_HEAP_ID);
        alloc.align = 1048576; // 1 MiB alignment to be able to protect later
        alloc.len = (alloc.len + 1048575U) & (~1048575U);
    }

#ifndef TARGET_ION_ABI_VERSION
    rc = ioctl(main_ion_fd, ION_IOC_ALLOC, &alloc);
#else
    rc = ion_alloc_fd(main_ion_fd, alloc.len, alloc.align, alloc.heap_id_mask,
              alloc.flags, &ion_info_fd.fd);
#endif //TARGET_ION_ABI_VERSION
    if (rc < 0) {
        LOGE("ION allocation failed: %s\n", strerror(errno));
        goto ION_ALLOC_FAILED;
    }

#ifndef TARGET_ION_ABI_VERSION
    ion_info_fd.handle = alloc.handle;
    rc = ioctl(main_ion_fd, ION_IOC_SHARE, &ion_info_fd);
    if (rc < 0) {
        LOGE("ION map failed %s\n", strerror(errno));
        goto ION_MAP_FAILED;
    }
#else
    ion_info_fd.handle = ion_info_fd.fd;
#endif //TARGET_ION_ABI_VERSION

    memInfo.main_ion_fd = main_ion_fd;
    memInfo.fd = ion_info_fd.fd;
    memInfo.handle = ion_info_fd.handle;
    memInfo.size = alloc.len;
    memInfo.cached = cached;
    memInfo.heap_id = heap_id;

    LOGD("ION buffer %lx with size %d allocated",
             (unsigned long)memInfo.handle, alloc.len);
    return OK;

#ifndef TARGET_ION_ABI_VERSION
ION_MAP_FAILED:
    memset(&handle_data, 0, sizeof(handle_data));
    handle_data.handle = ion_info_fd.handle;
    ioctl(main_ion_fd, ION_IOC_FREE, &handle_data);
#endif //TARGET_ION_ABI_VERSION
ION_ALLOC_FAILED:
#ifndef TARGET_ION_ABI_VERSION
    close(main_ion_fd);
#else
    ion_close(main_ion_fd);
#endif //TARGET_ION_ABI_VERSION
ION_OPEN_FAILED:
    return NO_MEMORY;

}

/*===========================================================================
 * FUNCTION   : deallocOneBuffer
 *
 * DESCRIPTION: impl of deallocating one buffers
 *
 * PARAMETERS :
 *   @memInfo : reference to struct that stores additional memory allocation info
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraMemory::deallocOneBuffer(QCameraMemInfo &memInfo)
{
    struct ion_handle_data handle_data;

    if (memInfo.fd >= 0) {
        close(memInfo.fd);
        memInfo.fd = -1;
    }

    if (memInfo.main_ion_fd >= 0) {
        memset(&handle_data, 0, sizeof(handle_data));
        handle_data.handle = memInfo.handle;
#ifndef  TARGET_ION_ABI_VERSION
        ioctl(memInfo.main_ion_fd, ION_IOC_FREE, &handle_data);
        close(memInfo.main_ion_fd);
#else
        ion_close(memInfo.main_ion_fd);
#endif  // TARGET_ION_ABI_VERSION
        memInfo.main_ion_fd = -1;
    }
    memInfo.handle = 0;
    memInfo.size = 0;
}

/*===========================================================================
 * FUNCTION   : QCameraMemoryPool
 *
 * DESCRIPTION: default constructor of QCameraMemoryPool
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCameraMemoryPool::QCameraMemoryPool()
{
    pthread_mutex_init(&mLock, NULL);
}


/*===========================================================================
 * FUNCTION   : ~QCameraMemoryPool
 *
 * DESCRIPTION: deconstructor of QCameraMemoryPool
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCameraMemoryPool::~QCameraMemoryPool()
{
    clear();
    pthread_mutex_destroy(&mLock);
}

/*===========================================================================
 * FUNCTION   : releaseBuffer
 *
 * DESCRIPTION: release one cached buffers
 *
 * PARAMETERS :
 *   @memInfo : reference to struct that stores additional memory allocation info
 *   @streamType: Type of stream the buffers belongs to
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraMemoryPool::releaseBuffer(
        struct QCameraMemory::QCameraMemInfo &memInfo,
        cam_stream_type_t streamType)
{
    pthread_mutex_lock(&mLock);

    mPools[streamType].push_back(memInfo);

    pthread_mutex_unlock(&mLock);
}

/*===========================================================================
 * FUNCTION   : clear
 *
 * DESCRIPTION: clears all cached buffers
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraMemoryPool::clear()
{
    pthread_mutex_lock(&mLock);

    for (int i = CAM_STREAM_TYPE_DEFAULT; i < CAM_STREAM_TYPE_MAX; i++ ) {
        List<struct QCameraMemory::QCameraMemInfo>::iterator it;
        it = mPools[i].begin();
        for( ; it != mPools[i].end() ; it++) {
            QCameraMemory::deallocOneBuffer(*it);
        }

        mPools[i].clear();
    }

    pthread_mutex_unlock(&mLock);
}

/*===========================================================================
 * FUNCTION   : findBufferLocked
 *
 * DESCRIPTION: search for a appropriate cached buffer
 *
 * PARAMETERS :
 *   @memInfo : reference to struct that stores additional memory allocation info
 *   @heap_id : type of heap
 *   @size    : size of the buffer
 *   @cached  : whether the buffer should be cached
 *   @streaType: type of stream this buffer belongs to
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraMemoryPool::findBufferLocked(
        struct QCameraMemory::QCameraMemInfo &memInfo, unsigned int heap_id,
        size_t size, bool cached, cam_stream_type_t streamType)
{
    int rc = NAME_NOT_FOUND;
    size_t alignsize = (size + 4095U) & (~4095U);
    if (mPools[streamType].empty()) {
        return NAME_NOT_FOUND;
    }

    List<struct QCameraMemory::QCameraMemInfo>::iterator it = mPools[streamType].begin();
    if (streamType == CAM_STREAM_TYPE_OFFLINE_PROC) {
        for( ; it != mPools[streamType].end() ; it++) {
            if( ((*it).size == alignsize) &&
                    ((*it).heap_id == heap_id) &&
                    ((*it).cached == cached) ) {
                memInfo = *it;
                LOGD("Found buffer %lx size %d",
                         (unsigned long)memInfo.handle, memInfo.size);
                mPools[streamType].erase(it);
                rc = NO_ERROR;
                break;
            }
        }
    } else {
        for( ; it != mPools[streamType].end() ; it++) {
            if(((*it).size >= size) &&
                    ((*it).heap_id == heap_id) &&
                    ((*it).cached == cached) ) {
                memInfo = *it;
                LOGD("Found buffer %lx size %d",
                         (unsigned long)memInfo.handle, memInfo.size);
                mPools[streamType].erase(it);
                rc = NO_ERROR;
                break;
            }
        }
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : allocateBuffer
 *
 * DESCRIPTION: allocates a buffer from the memory pool,
 *              it will re-use cached buffers if possible
 *
 * PARAMETERS :
 *   @memInfo : reference to struct that stores additional memory allocation info
 *   @heap_id : type of heap
 *   @size    : size of the buffer
 *   @cached  : whether the buffer should be cached
 *   @streaType: type of stream this buffer belongs to
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraMemoryPool::allocateBuffer(
        struct QCameraMemory::QCameraMemInfo &memInfo, unsigned int heap_id,
        size_t size, bool cached, cam_stream_type_t streamType,
        uint32_t secure_mode)
{
    int rc = NO_ERROR;

    pthread_mutex_lock(&mLock);

    rc = findBufferLocked(memInfo, heap_id, size, cached, streamType);
    if (NAME_NOT_FOUND == rc ) {
        LOGD("Buffer not found!");
        rc = QCameraMemory::allocOneBuffer(memInfo, heap_id, size, cached,
                 secure_mode);
    }

    pthread_mutex_unlock(&mLock);

    return rc;
}

/*===========================================================================
 * FUNCTION   : QCameraHeapMemory
 *
 * DESCRIPTION: constructor of QCameraHeapMemory for ion memory used internally in HAL
 *
 * PARAMETERS :
 *   @cached  : flag indicates if using cached memory
 *
 * RETURN     : none
 *==========================================================================*/
QCameraHeapMemory::QCameraHeapMemory(bool cached)
    : QCameraMemory(cached)
{
    for (int i = 0; i < MM_CAMERA_MAX_NUM_FRAMES; i ++)
        mPtr[i] = NULL;
}

/*===========================================================================
 * FUNCTION   : ~QCameraHeapMemory
 *
 * DESCRIPTION: deconstructor of QCameraHeapMemory
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCameraHeapMemory::~QCameraHeapMemory()
{
}

/*===========================================================================
 * FUNCTION   : getPtr
 *
 * DESCRIPTION: return buffer pointer
 *
 * PARAMETERS :
 *   @index   : index of the buffer
 *
 * RETURN     : buffer ptr
 *==========================================================================*/
void *QCameraHeapMemory::getPtr(uint32_t index) const
{
    if (index >= mBufferCount) {
        LOGE("index out of bound");
        return (void *)BAD_INDEX;
    }
    return mPtr[index];
}

/*===========================================================================
 * FUNCTION   : allocate
 *
 * DESCRIPTION: allocate requested number of buffers of certain size
 *
 * PARAMETERS :
 *   @count   : number of buffers to be allocated
 *   @size    : lenght of the buffer to be allocated
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraHeapMemory::allocate(uint8_t count, size_t size, uint32_t isSecure)
{
    int rc = -1;
    ATRACE_BEGIN_SNPRINTF("%s %zu %d", "HeapMemsize", size, count);
#ifndef TARGET_ION_ABI_VERSION
    uint32_t heap_id_mask = 0x1 << ION_IOMMU_HEAP_ID;
#else
    struct dma_buf_sync buf_sync;
    buf_sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    uint32_t heap_id_mask = 0x1 << ION_SYSTEM_HEAP_ID;
#endif // TARGET_ION_ABI_VERSION
    if (isSecure == SECURE) {
        rc = alloc(count, size, heap_id_mask, SECURE);
        if (rc < 0) {
            ATRACE_END();
            return rc;
        }
    } else {
        rc = alloc(count, size, heap_id_mask, NON_SECURE);
        if (rc < 0) {
            ATRACE_END();
            return rc;
        }

        for (int i = 0; i < count; i ++) {
            void *vaddr = mmap(NULL,
                        mMemInfo[i].size,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED,
                        mMemInfo[i].fd, 0);
            if (vaddr == MAP_FAILED) {
                for (int j = i-1; j >= 0; j --) {
                    munmap(mPtr[j], mMemInfo[j].size);
                    mPtr[j] = NULL;
                    deallocOneBuffer(mMemInfo[j]);
                }
                // Deallocate remaining buffers that have already been allocated
                for (int j = i; j < count; j++) {
                    deallocOneBuffer(mMemInfo[j]);
                }
                ATRACE_END();
                return NO_MEMORY;
            } else
                mPtr[i] = vaddr;
#ifdef TARGET_ION_ABI_VERSION
    rc = ioctl(mMemInfo[i].fd, DMA_BUF_IOCTL_SYNC, &buf_sync.flags);
    if (rc) {
        LOGE("Failed first DMA_BUF_IOCTL_SYNC start\n");
    }
#endif //TARGET_ION_ABI_VERSION

        }
    }
    if (rc == 0) {
        mBufferCount = count;
    }
    ATRACE_END();
    return OK;
}

/*===========================================================================
 * FUNCTION   : allocateMore
 *
 * DESCRIPTION: allocate more requested number of buffers of certain size
 *
 * PARAMETERS :
 *   @count   : number of buffers to be allocated
 *   @size    : lenght of the buffer to be allocated
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraHeapMemory::allocateMore(uint8_t count, size_t size)
{
    ATRACE_BEGIN_SNPRINTF("%s %zu %d", "HeapMemsize", size, count);
#ifndef TARGET_ION_ABI_VERSION
    uint32_t heap_id_mask = 0x1 << ION_IOMMU_HEAP_ID;
#else
    struct dma_buf_sync buf_sync;
    buf_sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    uint32_t heap_id_mask = 0x1 << ION_SYSTEM_HEAP_ID;
#endif // TARGET_ION_ABI_VERSION
    int rc = alloc(count, size, heap_id_mask, NON_SECURE);
    if (rc < 0) {
        ATRACE_END();
        return rc;
    }

    for (int i = mBufferCount; i < count + mBufferCount; i ++) {
        void *vaddr = mmap(NULL,
                    mMemInfo[i].size,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    mMemInfo[i].fd, 0);
        if (vaddr == MAP_FAILED) {
            for (int j = i-1; j >= mBufferCount; j --) {
                munmap(mPtr[j], mMemInfo[j].size);
                mPtr[j] = NULL;
                deallocOneBuffer(mMemInfo[j]);
            }
            ATRACE_END();
            return NO_MEMORY;
        } else {
            mPtr[i] = vaddr;
#ifdef TARGET_ION_ABI_VERSION
    rc = ioctl(mMemInfo[i].fd, DMA_BUF_IOCTL_SYNC, &buf_sync.flags);
    if (rc) {
        LOGE("Failed first DMA_BUF_IOCTL_SYNC start\n");
    }
#endif //TARGET_ION_ABI_VERSION
        }
    }
    mBufferCount = (uint8_t)(mBufferCount + count);
    ATRACE_END();
    return OK;
}

/*===========================================================================
 * FUNCTION   : deallocate
 *
 * DESCRIPTION: deallocate buffers
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraHeapMemory::deallocate()
{
#ifdef TARGET_ION_ABI_VERSION
    struct dma_buf_sync buf_sync;
    buf_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
#endif //TARGET_ION_ABI_VERSION
    for (int i = 0; i < mBufferCount; i++) {
#ifdef TARGET_ION_ABI_VERSION
    int rc = ioctl(mMemInfo[i].fd, DMA_BUF_IOCTL_SYNC, &buf_sync.flags);
    if (rc) {
        LOGE("Failed first DMA_BUF_IOCTL_SYNC start\n");
    }
#endif //TARGET_ION_ABI_VERSION
        munmap(mPtr[i], mMemInfo[i].size);
        mPtr[i] = NULL;
    }
    dealloc();
    mBufferCount = 0;
}

/*===========================================================================
 * FUNCTION   : cacheOps
 *
 * DESCRIPTION: ion related memory cache operations
 *
 * PARAMETERS :
 *   @index   : index of the buffer
 *   @cmd     : cache ops command
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraHeapMemory::cacheOps(uint32_t index, unsigned int cmd)
{
    if (index >= mBufferCount)
        return BAD_INDEX;
    return cacheOpsInternal(index, cmd, mPtr[index]);
}

/*===========================================================================
 * FUNCTION   : getRegFlags
 *
 * DESCRIPTION: query initial reg flags
 *
 * PARAMETERS :
 *   @regFlags: initial reg flags of the allocated buffers
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraHeapMemory::getRegFlags(uint8_t * /*regFlags*/) const
{
    return INVALID_OPERATION;
}

/*===========================================================================
 * FUNCTION   : getMemory
 *
 * DESCRIPTION: get camera memory
 *
 * PARAMETERS :
 *   @index   : buffer index
 *   @metadata: flag if it's metadata
 *
 * RETURN     : camera memory ptr
 *              NULL if not supported or failed
 *==========================================================================*/
camera_memory_t *QCameraHeapMemory::getMemory(uint32_t /*index*/, bool /*metadata*/) const
{
    return NULL;
}

/*===========================================================================
 * FUNCTION   : getMatchBufIndex
 *
 * DESCRIPTION: query buffer index by opaque ptr
 *
 * PARAMETERS :
 *   @opaque  : opaque ptr
 *   @metadata: flag if it's metadata
 *
 * RETURN     : buffer index if match found,
 *              -1 if failed
 *==========================================================================*/
int QCameraHeapMemory::getMatchBufIndex(const void *opaque,
                                        bool metadata) const
{
    int index = -1;
    if (metadata) {
        return -1;
    }
    for (int i = 0; i < mBufferCount; i++) {
        if (mPtr[i] == opaque) {
            index = i;
            break;
        }
    }
    return index;
}

/*===========================================================================
 * FUNCTION   : QCameraMetadataStreamMemory
 *
 * DESCRIPTION: constructor of QCameraMetadataStreamMemory
 *              for ion memory used internally in HAL for metadata
 *
 * PARAMETERS :
 *   @cached  : flag indicates if using cached memory
 *
 * RETURN     : none
 *==========================================================================*/
QCameraMetadataStreamMemory::QCameraMetadataStreamMemory(bool cached)
    : QCameraHeapMemory(cached)
{
}

/*===========================================================================
 * FUNCTION   : ~QCameraMetadataStreamMemory
 *
 * DESCRIPTION: destructor of QCameraMetadataStreamMemory
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCameraMetadataStreamMemory::~QCameraMetadataStreamMemory()
{
    if (mBufferCount > 0) {
        LOGH("%s, buf_cnt > 0, deallocate buffers now.\n", __func__);
        deallocate();
    }
}

/*===========================================================================
 * FUNCTION   : getRegFlags
 *
 * DESCRIPTION: query initial reg flags
 *
 * PARAMETERS :
 *   @regFlags: initial reg flags of the allocated buffers
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraMetadataStreamMemory::getRegFlags(uint8_t *regFlags) const
{
    for (int i = 0; i < mBufferCount; i ++) {
        regFlags[i] = 1;
    }
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : QCameraStreamMemory
 *
 * DESCRIPTION: constructor of QCameraStreamMemory
 *              ION memory allocated directly from /dev/ion and shared with framework
 *
 * PARAMETERS :
 *   @memory    : camera memory request ops table
 *   @cached    : flag indicates if using cached memory
 *
 * RETURN     : none
 *==========================================================================*/
QCameraStreamMemory::QCameraStreamMemory(camera_request_memory memory,
        void* cbCookie,
        bool cached,
        QCameraMemoryPool *pool,
        cam_stream_type_t streamType, __unused cam_stream_buf_type bufType)
    :QCameraMemory(cached, pool, streamType),
     mGetMemory(memory),
     mCallbackCookie(cbCookie)
{
    for (int i = 0; i < MM_CAMERA_MAX_NUM_FRAMES; i ++)
        mCameraMemory[i] = NULL;
}

/*===========================================================================
 * FUNCTION   : ~QCameraStreamMemory
 *
 * DESCRIPTION: deconstructor of QCameraStreamMemory
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCameraStreamMemory::~QCameraStreamMemory()
{
}

/*===========================================================================
 * FUNCTION   : allocate
 *
 * DESCRIPTION: allocate requested number of buffers of certain size
 *
 * PARAMETERS :
 *   @count   : number of buffers to be allocated
 *   @size    : lenght of the buffer to be allocated
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraStreamMemory::allocate(uint8_t count, size_t size, uint32_t isSecure)
{
    ATRACE_BEGIN_SNPRINTF("%s %zu %d", "StreamMemsize", size, count);
#ifndef TARGET_ION_ABI_VERSION
    uint32_t heap_id_mask = 0x1 << ION_IOMMU_HEAP_ID;
#else
    uint32_t heap_id_mask = 0x1 << ION_SYSTEM_HEAP_ID;
#endif // TARGET_ION_ABI_VERSION
    int rc = alloc(count, size, heap_id_mask, isSecure);
    if (rc < 0) {
        ATRACE_END();
        return rc;
    }

    for (int i = 0; i < count; i ++) {
        if (isSecure == SECURE) {
            mCameraMemory[i] = 0;
        } else {
            mCameraMemory[i] = mGetMemory(mMemInfo[i].fd, mMemInfo[i].size, 1, mCallbackCookie);
        }
    }
    mBufferCount = count;
    ATRACE_END();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : allocateMore
 *
 * DESCRIPTION: allocate more requested number of buffers of certain size
 *
 * PARAMETERS :
 *   @count   : number of buffers to be allocated
 *   @size    : lenght of the buffer to be allocated
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraStreamMemory::allocateMore(uint8_t count, size_t size)
{
    ATRACE_BEGIN_SNPRINTF("%s %zu %d", "StreamMemsize", size, count);
#ifndef TARGET_ION_ABI_VERSION
    uint32_t heap_id_mask = 0x1 << ION_IOMMU_HEAP_ID;
#else
    uint32_t heap_id_mask = 0x1 << ION_SYSTEM_HEAP_ID;
#endif // TARGET_ION_ABI_VERSION
    int rc = alloc(count, size, heap_id_mask, NON_SECURE);
    if (rc < 0) {
        ATRACE_END();
        return rc;
    }

    for (int i = mBufferCount; i < mBufferCount + count; i++) {
        mCameraMemory[i] = mGetMemory(mMemInfo[i].fd, mMemInfo[i].size, 1, mCallbackCookie);
    }
    mBufferCount = (uint8_t)(mBufferCount + count);
    ATRACE_END();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : deallocate
 *
 * DESCRIPTION: deallocate buffers
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraStreamMemory::deallocate()
{
    for (int i = 0; i < mBufferCount; i ++) {
        if (mCameraMemory[i])
            mCameraMemory[i]->release(mCameraMemory[i]);
        mCameraMemory[i] = NULL;
    }
    dealloc();
    mBufferCount = 0;
}

/*===========================================================================
 * FUNCTION   : cacheOps
 *
 * DESCRIPTION: ion related memory cache operations
 *
 * PARAMETERS :
 *   @index   : index of the buffer
 *   @cmd     : cache ops command
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraStreamMemory::cacheOps(uint32_t index, unsigned int cmd)
{
    if (index >= mBufferCount)
        return BAD_INDEX;
    return cacheOpsInternal(index, cmd, mCameraMemory[index]->data);
}

/*===========================================================================
 * FUNCTION   : getRegFlags
 *
 * DESCRIPTION: query initial reg flags
 *
 * PARAMETERS :
 *   @regFlags: initial reg flags of the allocated buffers
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraStreamMemory::getRegFlags(uint8_t *regFlags) const
{
    for (int i = 0; i < mBufferCount; i ++)
        regFlags[i] = 1;
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : getMemory
 *
 * DESCRIPTION: get camera memory
 *
 * PARAMETERS :
 *   @index   : buffer index
 *   @metadata: flag if it's metadata
 *
 * RETURN     : camera memory ptr
 *              NULL if not supported or failed
 *==========================================================================*/
camera_memory_t *QCameraStreamMemory::getMemory(uint32_t index,
        bool metadata) const
{
    if (index >= mBufferCount || metadata)
        return NULL;
    return mCameraMemory[index];
}

/*===========================================================================
 * FUNCTION   : getMatchBufIndex
 *
 * DESCRIPTION: query buffer index by opaque ptr
 *
 * PARAMETERS :
 *   @opaque  : opaque ptr
 *   @metadata: flag if it's metadata
 *
 * RETURN     : buffer index if match found,
 *              -1 if failed
 *==========================================================================*/
int QCameraStreamMemory::getMatchBufIndex(const void *opaque,
                                          bool metadata) const
{
    int index = -1;
    if (metadata) {
        return -1;
    }
    for (int i = 0; i < mBufferCount; i++) {
        if (mCameraMemory[i]->data == opaque) {
            index = i;
            break;
        }
    }
    return index;
}

/*===========================================================================
 * FUNCTION   : getPtr
 *
 * DESCRIPTION: return buffer pointer
 *
 * PARAMETERS :
 *   @index   : index of the buffer
 *
 * RETURN     : buffer ptr
 *==========================================================================*/
void *QCameraStreamMemory::getPtr(uint32_t index) const
{
    if (index >= mBufferCount) {
        LOGE("index out of bound");
        return (void *)BAD_INDEX;
    }
    if (mCameraMemory[index] == 0) {
        return NULL;
    }
    return mCameraMemory[index]->data;
}

/*===========================================================================
 * FUNCTION   : QCameraVideoMemory
 *
 * DESCRIPTION: constructor of QCameraVideoMemory
 *              VideoStream buffers also include metadata buffers
 *
 * PARAMETERS :
 *   @memory    : camera memory request ops table
 *   @cached    : flag indicates if using cached ION memory
 *
 * RETURN     : none
 *==========================================================================*/
QCameraVideoMemory::QCameraVideoMemory(camera_request_memory memory, void* cbCookie,
                                       bool cached, QCameraMemType bufType)
    : QCameraStreamMemory(memory, cbCookie, cached)
{
    memset(mMetadata, 0, sizeof(mMetadata));
    memset(mNativeHandle, 0, sizeof(mNativeHandle));
    mMetaBufCount = 0;
    mBufType = bufType;
    //Set Default color conversion format
    mUsage = private_handle_t::PRIV_FLAGS_ITU_R_601_FR;

    //Set Default frame format
    mFormat = OMX_COLOR_FormatYUV420SemiPlanar;
}

/*===========================================================================
 * FUNCTION   : ~QCameraVideoMemory
 *
 * DESCRIPTION: deconstructor of QCameraVideoMemory
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCameraVideoMemory::~QCameraVideoMemory()
{
}

/*===========================================================================
 * FUNCTION   : allocate
 *
 * DESCRIPTION: allocate requested number of buffers of certain size
 *
 * PARAMETERS :
 *   @count   : number of buffers to be allocated
 *   @size    : lenght of the buffer to be allocated
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraVideoMemory::allocate(uint8_t count, size_t size, uint32_t isSecure)
{
    ATRACE_BEGIN_SNPRINTF("%s %zu %d", "VideoMemsize", size, count);
    int rc = QCameraStreamMemory::allocate(count, size, isSecure);
    if (rc < 0) {
        ATRACE_END();
        return rc;
    }

    if (!(mBufType & QCAMERA_MEM_TYPE_BATCH)) {
        /*
        *    FDs = 1
        *    numInts  = 5 //offset, size, usage, timestamp, format + 1 for buffer index
        */
        rc = allocateMeta(count, 1, VIDEO_METADATA_NUM_INTS);
        if (rc != NO_ERROR) {
            ATRACE_END();
            return rc;
        }
        for (int i = 0; i < count; i ++) {
            native_handle_t *nh =  mNativeHandle[i];
            if (!nh) {
                LOGE("Error in getting video native handle");
                ATRACE_END();
                return NO_MEMORY;
            }
            nh->data[0] = mMemInfo[i].fd;
            nh->data[1] = 0;
            nh->data[2] = (int)mMemInfo[i].size;
            nh->data[3] = mUsage;
            nh->data[4] = 0; //dummy value for timestamp in non-batch mode
            nh->data[5] = mFormat;
        }
    }
    mBufferCount = count;
    ATRACE_END();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : allocateMore
 *
 * DESCRIPTION: allocate more requested number of buffers of certain size
 *
 * PARAMETERS :
 *   @count   : number of buffers to be allocated
 *   @size    : lenght of the buffer to be allocated
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraVideoMemory::allocateMore(uint8_t count, size_t size)
{
    ATRACE_BEGIN_SNPRINTF("%s %zu %d", "VideoMemsize", size, count);
    int rc = QCameraStreamMemory::allocateMore(count, size);
    if (rc < 0) {
        ATRACE_END();
        return rc;
    }

    if (!(mBufType & QCAMERA_MEM_TYPE_BATCH)) {
        for (int i = mBufferCount; i < count + mBufferCount; i ++) {
            mMetadata[i] = mGetMemory(-1,
                    sizeof(media_metadata_buffer), 1, mCallbackCookie);
            if (!mMetadata[i]) {
                LOGE("allocation of video metadata failed.");
                for (int j = mBufferCount; j <= i-1; j ++) {
                    mMetadata[j]->release(mMetadata[j]);
                    mCameraMemory[j]->release(mCameraMemory[j]);
                    mCameraMemory[j] = NULL;
                    deallocOneBuffer(mMemInfo[j]);;
                }
                ATRACE_END();
                return NO_MEMORY;
            }
            media_metadata_buffer * packet =
                    (media_metadata_buffer *)mMetadata[i]->data;
            //FDs = 1
            //numInts  = 5 (offset, size, usage, timestamp, format)
            mNativeHandle[i] = native_handle_create(1,
                    (VIDEO_METADATA_NUM_INTS + VIDEO_METADATA_NUM_COMMON_INTS));
#ifdef USE_MEDIA_EXTENSIONS
            packet->eType = kMetadataBufferTypeNativeHandleSource;
            packet->pHandle = NULL;
#else
            packet->buffer_type = kMetadataBufferTypeCameraSource;
            packet->meta_handle = mNativeHandle[i];
#endif
            native_handle_t *nh =  mNativeHandle[i];
            if (!nh) {
                LOGE("Error in getting video native handle");
                ATRACE_END();
                return NO_MEMORY;
            }
            nh->data[0] = mMemInfo[i].fd;
            nh->data[1] = 0;
            nh->data[2] = (int)mMemInfo[i].size;
            nh->data[3] = mUsage;
            nh->data[4] = 0; //dummy value for timestamp in non-batch mode
            nh->data[5] = mFormat;
            nh->data[6] = i;
        }
    }
    mBufferCount = (uint8_t)(mBufferCount + count);
    mMetaBufCount = mBufferCount;
    ATRACE_END();
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : allocateMeta
 *
 * DESCRIPTION: allocate video encoder metadata structure
 *
 * PARAMETERS :
 *   @fd_cnt : Total FD count
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraVideoMemory::allocateMeta(uint8_t buf_cnt, int numFDs, int numInts)
{
    int rc = NO_ERROR;
    int mTotalInts = 0;

    for (int i = 0; i < buf_cnt; i++) {
        mMetadata[i] = mGetMemory(-1,
                sizeof(media_metadata_buffer), 1, mCallbackCookie);
        if (!mMetadata[i]) {
            LOGE("allocation of video metadata failed.");
            for (int j = (i - 1); j >= 0; j--) {
                if (NULL != mNativeHandle[j]) {
                   native_handle_delete(mNativeHandle[j]);
                }
                mMetadata[j]->release(mMetadata[j]);
            }
            return NO_MEMORY;
        }
        media_metadata_buffer *packet =
                (media_metadata_buffer *)mMetadata[i]->data;
        mTotalInts = (numInts * numFDs);
        mNativeHandle[i] = native_handle_create(numFDs,
                (mTotalInts + VIDEO_METADATA_NUM_COMMON_INTS));
        if (mNativeHandle[i] == NULL) {
            LOGE("Error in getting video native handle");
            for (int j = (i - 1); j >= 0; j--) {
                mMetadata[i]->release(mMetadata[i]);
                if (NULL != mNativeHandle[j]) {
                   native_handle_delete(mNativeHandle[j]);
                }
                mMetadata[j]->release(mMetadata[j]);
            }
            return NO_MEMORY;
        } else {
            //assign buffer index to native handle.
            native_handle_t *nh =  mNativeHandle[i];
            nh->data[numFDs + mTotalInts] = i;
        }
#ifdef USE_MEDIA_EXTENSIONS
        packet->eType = kMetadataBufferTypeNativeHandleSource;
        packet->pHandle = NULL;
#else
        packet->buffer_type = kMetadataBufferTypeCameraSource;
        packet->meta_handle = mNativeHandle[i];
#endif
    }
    mMetaBufCount = buf_cnt;
    return rc;
}

/*===========================================================================
 * FUNCTION   : deallocateMeta
 *
 * DESCRIPTION: deallocate video metadata buffers
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraVideoMemory::deallocateMeta()
{
    for (int i = 0; i < mMetaBufCount; i++) {
        native_handle_t *nh = mNativeHandle[i];
        if (NULL != nh) {
           if (native_handle_delete(nh)) {
               LOGE("Unable to delete native handle");
           }
        } else {
           LOGE("native handle not available");
        }
        mNativeHandle[i] = NULL;
        mMetadata[i]->release(mMetadata[i]);
        mMetadata[i] = NULL;
    }
    mMetaBufCount = 0;
}


/*===========================================================================
 * FUNCTION   : deallocate
 *
 * DESCRIPTION: deallocate buffers
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraVideoMemory::deallocate()
{
    deallocateMeta();

    QCameraStreamMemory::deallocate();
    mBufferCount = 0;
    mMetaBufCount = 0;
}

/*===========================================================================
 * FUNCTION   : getMemory
 *
 * DESCRIPTION: get camera memory
 *
 * PARAMETERS :
 *   @index   : buffer index
 *   @metadata: flag if it's metadata
 *
 * RETURN     : camera memory ptr
 *              NULL if not supported or failed
 *==========================================================================*/
camera_memory_t *QCameraVideoMemory::getMemory(uint32_t index,
        bool metadata) const
{
    int i;
    if (index >= mMetaBufCount || (!metadata && index >= mBufferCount))
        return NULL;

    if (metadata) {
#ifdef USE_MEDIA_EXTENSIONS
        media_metadata_buffer *packet = NULL;

        for (i = 0; i < mMetaBufCount; i++) {
            packet = (media_metadata_buffer *)mMetadata[i]->data;
            if (packet != NULL && packet->pHandle == NULL) {
                packet->pHandle = mNativeHandle[index];
                break;
            }
        }
        if (i < mMetaBufCount) {
            return mMetadata[i];
        } else {
            LOGE("No free video meta memory");
            return NULL;
        }
#else
        return mMetadata[index];
#endif
    } else {
        return mCameraMemory[index];
    }
}

/*===========================================================================
 * FUNCTION   : getNativeHandle
 *
 * DESCRIPTION: getNativeHandle from video buffer
 *
 * PARAMETERS :
 *   @index   : buffer index
 *
 * RETURN     : native_handle_t  * type of handle
 *==========================================================================*/
native_handle_t *QCameraVideoMemory::getNativeHandle(uint32_t index, bool metadata)
{
    if (index >= mMetaBufCount || !metadata)
        return NULL;
    return mNativeHandle[index];
}

/*===========================================================================
 * FUNCTION   : closeNativeHandle
 *
 * DESCRIPTION: static function to close video native handle.
 *
 * PARAMETERS :
 *   @data  : ptr to video frame to be returned
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraVideoMemory::closeNativeHandle(const void *data)
{
    int32_t rc = NO_ERROR;

#ifdef USE_MEDIA_EXTENSIONS
    const media_metadata_buffer *packet =
            (const media_metadata_buffer *)data;
    if ((packet != NULL) && (packet->eType ==
            kMetadataBufferTypeNativeHandleSource)
            && (packet->pHandle)) {
        native_handle_close(packet->pHandle);
        native_handle_delete(packet->pHandle);
    } else {
        LOGE("Invalid Data. Could not release");
        return BAD_VALUE;
    }
#endif
   return rc;
}

/*===========================================================================
 * FUNCTION   : closeNativeHandle
 *
 * DESCRIPTION: close video native handle and update cached ptrs
 *
 * PARAMETERS :
 *   @data     : ptr to video frame to be returned
 *   @metadata : Flag to update metadata mode
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraVideoMemory::closeNativeHandle(const void *data, bool metadata)
{
    int32_t rc = NO_ERROR;

#ifdef USE_MEDIA_EXTENSIONS
    if (metadata) {
        const media_metadata_buffer *packet =
                    (const media_metadata_buffer *)data;
        if ((packet != NULL) && (packet->eType ==
                kMetadataBufferTypeNativeHandleSource)
                && (packet->pHandle)) {
            native_handle_close(packet->pHandle);
            native_handle_delete(packet->pHandle);
            for (int i = 0; i < mMetaBufCount; i++) {
                if(mMetadata[i]->data == data) {
                    media_metadata_buffer *mem =
                            (media_metadata_buffer *)mMetadata[i]->data;
                    mem->pHandle = NULL;
                    break;
                }
            }
        } else {
            LOGE("Invalid Data. Could not release");
            return BAD_VALUE;
        }
    } else {
        LOGW("Warning: Not of type video meta buffer");
    }
#endif
    return rc;
}

/*===========================================================================
 * FUNCTION   : getMatchBufIndex
 *
 * DESCRIPTION: query buffer index by opaque ptr
 *
 * PARAMETERS :
 *   @opaque  : opaque ptr
 *   @metadata: flag if it's metadata
 *
 * RETURN     : buffer index if match found,
 *              -1 if failed
 *==========================================================================*/
int QCameraVideoMemory::getMatchBufIndex(const void *opaque,
                                         bool metadata) const
{
    int index = -1;

    if (metadata) {
#ifdef USE_MEDIA_EXTENSIONS
        const media_metadata_buffer *packet =
                (const media_metadata_buffer *)opaque;
        native_handle_t *nh = NULL;
        if ((packet != NULL) && (packet->eType ==
                kMetadataBufferTypeNativeHandleSource)
                && (packet->pHandle)) {
            nh = (native_handle_t *)packet->pHandle;
            int mCommonIdx = (nh->numInts + nh->numFds -
                    VIDEO_METADATA_NUM_COMMON_INTS);
            for (int i = 0; i < mMetaBufCount; i++) {
                if(nh->data[mCommonIdx] == mNativeHandle[i]->data[mCommonIdx]) {
                    index = i;
                    break;
                }
            }
        }
#else
        for (int i = 0; i < mMetaBufCount; i++) {
            if(mMetadata[i]->data == opaque) {
                index = i;
                break;
            }
        }
#endif
    } else {
        for (int i = 0; i < mBufferCount; i++) {
            if (mCameraMemory[i]->data == opaque) {
                index = i;
                break;
            }
        }
    }
    return index;
}

/*===========================================================================
 * FUNCTION   : setVideoInfo
 *
 * DESCRIPTION: set native window gralloc ops table
 *
 * PARAMETERS :
 *   @usage : usage bit for video
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraVideoMemory::setVideoInfo(int usage, cam_format_t format)
{
    mUsage |= usage;
    mFormat = convCamtoOMXFormat(format);
}

/*===========================================================================
 * FUNCTION   : convCamtoOMXFormat
 *
 * DESCRIPTION: map cam_format_t to corresponding OMX format
 *
 * PARAMETERS :
 *   @format : format in cam_format_t type
 *
 * RETURN     : omx format
 *==========================================================================*/
int QCameraVideoMemory::convCamtoOMXFormat(cam_format_t format)
{
    int omxFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    switch (format) {
        case CAM_FORMAT_YUV_420_NV21:
        case CAM_FORMAT_YUV_420_NV21_VENUS:
        case CAM_FORMAT_YUV_420_NV21_ADRENO:
            omxFormat = QOMX_COLOR_FormatYVU420SemiPlanar;
            break;
        case CAM_FORMAT_YUV_420_NV12:
        case CAM_FORMAT_YUV_420_NV12_VENUS:
            omxFormat = OMX_COLOR_FormatYUV420SemiPlanar;
            break;
        case CAM_FORMAT_YUV_420_NV12_UBWC:
            omxFormat = QOMX_COLOR_FORMATYUV420PackedSemiPlanar32mCompressed;
            break;
        default:
            omxFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    }
    return omxFormat;
}

/*===========================================================================
 * FUNCTION   : QCameraGrallocMemory
 *
 * DESCRIPTION: constructor of QCameraGrallocMemory
 *              preview stream buffers are allocated from gralloc native_windoe
 *
 * PARAMETERS :
 *   @memory    : camera memory request ops table
 *
 * RETURN     : none
 *==========================================================================*/
QCameraGrallocMemory::QCameraGrallocMemory(camera_request_memory memory, void* cbCookie)
        : QCameraMemory(true), mColorSpace(ITU_R_601_FR)
{
    mMinUndequeuedBuffers = 0;
    mMappableBuffers = 0;
    mWindow = NULL;
    mWidth = mHeight = mStride = mScanline = mUsage = 0;
    mFormat = HAL_PIXEL_FORMAT_YCrCb_420_SP;
    mCallbackCookie = cbCookie;
    mGetMemory = memory;
    for (int i = 0; i < MM_CAMERA_MAX_NUM_FRAMES; i ++) {
        mBufferHandle[i] = NULL;
        mLocalFlag[i] = BUFFER_NOT_OWNED;
        mPrivateHandle[i] = NULL;
        mBufferStatus[i] = STATUS_IDLE;
        mCameraMemory[i] = NULL;
    }
}

/*===========================================================================
 * FUNCTION   : ~QCameraGrallocMemory
 *
 * DESCRIPTION: deconstructor of QCameraGrallocMemory
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCameraGrallocMemory::~QCameraGrallocMemory()
{
}

/*===========================================================================
 * FUNCTION   : setWindowInfo
 *
 * DESCRIPTION: set native window gralloc ops table
 *
 * PARAMETERS :
 *   @window  : gralloc ops table ptr
 *   @width   : width of preview frame
 *   @height  : height of preview frame
 *   @stride  : stride of preview frame
 *   @scanline: scanline of preview frame
 *   @foramt  : format of preview image
 *   @maxFPS : max fps of preview stream
 *   @usage : usage bit for gralloc
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraGrallocMemory::setWindowInfo(preview_stream_ops_t *window,
        int width, int height, int stride, int scanline, int format, int maxFPS, int usage)
{
    mWindow = window;
    mWidth = width;
    mHeight = height;
    mStride = stride;
    mScanline = scanline;
    mFormat = format;
    mUsage = usage;
    setMaxFPS(maxFPS);
}

/*===========================================================================
 * FUNCTION   : setMaxFPS
 *
 * DESCRIPTION: set max fps
 *
 * PARAMETERS :
 *   @maxFPS : max fps of preview stream
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraGrallocMemory::setMaxFPS(int maxFPS)
{
    /* input will be in multiples of 1000 */
    maxFPS = (maxFPS + 500)/1000;

    /* set the lower cap to 30 always, because we are not supporting runtime update of fps info
      to display. Otherwise MDP may result in underruns (for example if initial fps is 15max and later
      changed to 30).*/
    if (maxFPS < 30) {
        maxFPS = 30;
    }

    /* the new fps will be updated in metadata of the next frame enqueued to display*/
    mMaxFPS = maxFPS;
    LOGH("Setting max fps %d to display", maxFPS);
}

/*===========================================================================
 * FUNCTION   : displayBuffer
 *
 * DESCRIPTION: send received frame to display
 *
 * PARAMETERS :
 *   @index   : index of preview frame
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraGrallocMemory::displayBuffer(uint32_t index)
{
    int err = NO_ERROR;
    int dequeuedIdx = BAD_INDEX;

    if (BUFFER_NOT_OWNED == mLocalFlag[index]) {
        LOGE("buffer to be enqueued is not owned");
        return INVALID_OPERATION;
    }

    err = mWindow->enqueue_buffer(mWindow, (buffer_handle_t *)mBufferHandle[index]);
    if(err != 0) {
        LOGE("enqueue_buffer failed, err = %d", err);
    } else {
        LOGD("enqueue_buffer hdl=%p", *mBufferHandle[index]);
        mLocalFlag[index] = BUFFER_NOT_OWNED;
    }

    buffer_handle_t *buffer_handle = NULL;
    int stride = 0;
    err = mWindow->dequeue_buffer(mWindow, &buffer_handle, &stride);
    if (err == NO_ERROR && buffer_handle != NULL) {
        int i;
        LOGD("dequed buf hdl =%p", *buffer_handle);
        for(i = 0; i < mMappableBuffers; i++) {
            if(mBufferHandle[i] == buffer_handle) {
                LOGD("Found buffer in idx:%d", i);
                mLocalFlag[i] = BUFFER_OWNED;
                dequeuedIdx = i;
                break;
            }
        }

        if ((dequeuedIdx == BAD_INDEX) && (mMappableBuffers < mBufferCount)) {
            dequeuedIdx = mMappableBuffers;
            LOGD("Placing buffer in idx:%d", dequeuedIdx);
            mBufferHandle[dequeuedIdx] = buffer_handle;
            mLocalFlag[dequeuedIdx] = BUFFER_OWNED;

            mPrivateHandle[dequeuedIdx] =
                    (struct private_handle_t *)(*mBufferHandle[dequeuedIdx]);
#ifndef TARGET_ION_ABI_VERSION
            mMemInfo[dequeuedIdx].main_ion_fd = open("/dev/ion", O_RDONLY);
#else
            mMemInfo[dequeuedIdx].main_ion_fd = ion_open();
#endif //TARGET_ION_ABI_VERSION
            if (mMemInfo[dequeuedIdx].main_ion_fd < 0) {
                LOGE("failed: could not open ion device");
                return BAD_INDEX;
            }
            struct ion_fd_data ion_info_fd;
            memset(&ion_info_fd, 0, sizeof(ion_info_fd));
            ion_info_fd.fd = mPrivateHandle[dequeuedIdx]->fd;
#ifndef TARGET_ION_ABI_VERSION
            if (ioctl(mMemInfo[dequeuedIdx].main_ion_fd,
                      ION_IOC_IMPORT, &ion_info_fd) < 0) {
                LOGE("ION import failed\n");
                return BAD_INDEX;
            }
#else
            ion_info_fd.handle = ion_info_fd.fd;
#endif
            mCameraMemory[dequeuedIdx] =
                    mGetMemory(mPrivateHandle[dequeuedIdx]->fd,
                    (size_t)mPrivateHandle[dequeuedIdx]->size,
                    1,
                    mCallbackCookie);
            LOGH("idx = %d, fd = %d, size = %d, offset = %d",
                     dequeuedIdx, mPrivateHandle[dequeuedIdx]->fd,
                    mPrivateHandle[dequeuedIdx]->size,
                    mPrivateHandle[dequeuedIdx]->offset);
            mMemInfo[dequeuedIdx].fd = mPrivateHandle[dequeuedIdx]->fd;
            mMemInfo[dequeuedIdx].size =
                    (size_t)mPrivateHandle[dequeuedIdx]->size;
            mMemInfo[dequeuedIdx].handle = ion_info_fd.handle;

            mMappableBuffers++;
        }
    } else {
        LOGW("dequeue_buffer, no free buffer from display now");
    }
    return dequeuedIdx;
}

/*===========================================================================
 * FUNCTION   : enqueueBuffer
 *
 * DESCRIPTION: enqueue camera frame to display
 *
 * PARAMETERS :
 *   @index   : index of frame
 *   @timeStamp : frame presentation time
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraGrallocMemory::enqueueBuffer(uint32_t index, nsecs_t timeStamp)
{
    int32_t err = NO_ERROR;

    if (BUFFER_NOT_OWNED == mLocalFlag[index]) {
        LOGE("buffer to be enqueued is not owned");
        return INVALID_OPERATION;
    }

    if (timeStamp != 0) {
        err = mWindow->set_timestamp(mWindow, timeStamp);
        if (err != NO_ERROR){
            LOGE("Failed to native window timestamp");
        }
    }

    err = mWindow->enqueue_buffer(mWindow, (buffer_handle_t *)mBufferHandle[index]);
    if(err != 0) {
        LOGE("enqueue_buffer failed, err = %d", err);
    } else {
        LOGD("enqueue_buffer hdl=%p", *mBufferHandle[index]);
        mLocalFlag[index] = BUFFER_NOT_OWNED;
    }
    return err;
}

/*===========================================================================
 * FUNCTION   : dequeueBuffer
 *
 * DESCRIPTION: receive a buffer from gralloc
 *
 * PARAMETERS : None
 *
 * RETURN     : int32_t
 *              NO_ERROR/Buffer index : Success
 *              < 0 failure code
 *==========================================================================*/
int32_t QCameraGrallocMemory::dequeueBuffer()
{
    int32_t err = NO_ERROR;
    int32_t dequeuedIdx = BAD_INDEX;
    buffer_handle_t *buffer_handle = NULL;
    int32_t stride = 0;

    dequeuedIdx = BAD_INDEX;
    err = mWindow->dequeue_buffer(mWindow, &buffer_handle, &stride);
    if ((err == NO_ERROR) && (buffer_handle != NULL)) {
        int i;
        LOGD("dequed buf hdl =%p", *buffer_handle);
        for(i = 0; i < mMappableBuffers; i++) {
            if(mBufferHandle[i] == buffer_handle) {
                LOGD("Found buffer in idx:%d", i);
                mLocalFlag[i] = BUFFER_OWNED;
                dequeuedIdx = i;
                break;
            }
        }

        if ((dequeuedIdx == BAD_INDEX) &&
                (mMappableBuffers < mBufferCount)) {
            dequeuedIdx = mMappableBuffers;
            LOGD("Placing buffer in idx:%d", dequeuedIdx);
            mBufferHandle[dequeuedIdx] = buffer_handle;
            mLocalFlag[dequeuedIdx] = BUFFER_OWNED;

            mPrivateHandle[dequeuedIdx] =
                    (struct private_handle_t *)(*mBufferHandle[dequeuedIdx]);
            //update max fps info
            setMetaData(mPrivateHandle[dequeuedIdx], UPDATE_REFRESH_RATE, (void*)&mMaxFPS);
#ifndef TARGET_ION_ABI_VERSION
            mMemInfo[dequeuedIdx].main_ion_fd = open("/dev/ion", O_RDONLY);
#else
            mMemInfo[dequeuedIdx].main_ion_fd = ion_open();
#endif //TARGET_ION_ABI_VERSION
            if (mMemInfo[dequeuedIdx].main_ion_fd < 0) {
                LOGE("failed: could not open ion device");
                return BAD_INDEX;
            }
            struct ion_fd_data ion_info_fd;
            memset(&ion_info_fd, 0, sizeof(ion_info_fd));
#ifndef TARGET_ION_ABI_VERSION
            ion_info_fd.fd = mPrivateHandle[dequeuedIdx]->fd;
            if (ioctl(mMemInfo[dequeuedIdx].main_ion_fd,
                    ION_IOC_IMPORT, &ion_info_fd) < 0) {
                LOGE("ION import failed\n");
                return BAD_INDEX;
            }
#else
           if(ion_import(mMemInfo[dequeuedIdx].main_ion_fd,  ion_info_fd.fd,  &ion_info_fd.handle) < 0)
           {
               LOGE("ION import failed\n");
               return BAD_INDEX;
           }
#endif //TARGET_ION_ABI_VERSION
            setMetaData(mPrivateHandle[dequeuedIdx], UPDATE_COLOR_SPACE,
                    &mColorSpace);
            mCameraMemory[dequeuedIdx] =
                    mGetMemory(mPrivateHandle[dequeuedIdx]->fd,
                    (size_t)mPrivateHandle[dequeuedIdx]->size,
                    1,
                    mCallbackCookie);
            LOGH("idx = %d, fd = %d, size = %d, offset = %d",
                     dequeuedIdx, mPrivateHandle[dequeuedIdx]->fd,
                    mPrivateHandle[dequeuedIdx]->size,
                    mPrivateHandle[dequeuedIdx]->offset);
            mMemInfo[dequeuedIdx].fd = mPrivateHandle[dequeuedIdx]->fd;
            mMemInfo[dequeuedIdx].size =
                    (size_t)mPrivateHandle[dequeuedIdx]->size;
            mMemInfo[dequeuedIdx].handle = ion_info_fd.handle;

            mMappableBuffers++;
        }
    } else {
        LOGW("dequeue_buffer, no free buffer from display now");
    }

    return dequeuedIdx;
}


/*===========================================================================
 * FUNCTION   : allocate
 *
 * DESCRIPTION: allocate requested number of buffers of certain size
 *
 * PARAMETERS :
 *   @count   : number of buffers to be allocated
 *   @size    : lenght of the buffer to be allocated
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraGrallocMemory::allocate(uint8_t count, size_t /*size*/,
        uint32_t /*isSecure*/)
{
    ATRACE_BEGIN_SNPRINTF("%s %d", "Grallocbufcnt", count);
    int err = 0;
    status_t ret = NO_ERROR;
    int gralloc_usage = 0;
    struct ion_fd_data ion_info_fd;
    memset(&ion_info_fd, 0, sizeof(ion_info_fd));

    LOGD("E ");

    if (!mWindow) {
        LOGE("Invalid native window");
        ATRACE_END();
        ret = INVALID_OPERATION;
        goto end;
    }

    // Increment buffer count by min undequeued buffer.
    err = mWindow->get_min_undequeued_buffer_count(mWindow,&mMinUndequeuedBuffers);
    if (err != 0) {
        LOGE("get_min_undequeued_buffer_count  failed: %s (%d)",
                strerror(-err), -err);
        ret = UNKNOWN_ERROR;
        goto end;
    }

    err = mWindow->set_buffer_count(mWindow, count);
    if (err != 0) {
         LOGE("set_buffer_count failed: %s (%d)",
                    strerror(-err), -err);
         ret = UNKNOWN_ERROR;
         goto end;
    }

    err = mWindow->set_buffers_geometry(mWindow, mWidth, mHeight, mFormat);
    if (err != 0) {
         LOGE("set_buffers_geometry failed: %s (%d)",
                strerror(-err), -err);
         ret = UNKNOWN_ERROR;
         goto end;
    }

    gralloc_usage = GRALLOC_USAGE_HW_CAMERA_WRITE;
    gralloc_usage |= mUsage;
    err = mWindow->set_usage(mWindow, gralloc_usage);
    if(err != 0) {
        /* set_usage error out */
        LOGE("set_usage rc = %d", err);
        ret = UNKNOWN_ERROR;
        goto end;
    }
    LOGH("usage = %d, geometry: %p, %d, %d, %d, %d, %d",
           gralloc_usage, mWindow, mWidth, mHeight, mStride,
          mScanline, mFormat);

    mBufferCount = count;
    if ((count < mMappableBuffers) || (mMappableBuffers == 0)) {
        mMappableBuffers = count;
    }

    //Allocate cnt number of buffers from native window
    for (int cnt = 0; cnt < mMappableBuffers; cnt++) {
        int stride;
        err = mWindow->dequeue_buffer(mWindow, &mBufferHandle[cnt], &stride);
        if(!err) {
            LOGD("dequeue buf hdl =%p", mBufferHandle[cnt]);
            mLocalFlag[cnt] = BUFFER_OWNED;
        } else {
            mLocalFlag[cnt] = BUFFER_NOT_OWNED;
            LOGE("dequeue_buffer idx = %d err = %d", cnt, err);
        }

        LOGD("dequeue buf: %p\n", mBufferHandle[cnt]);

        if(err != 0) {
            LOGE("dequeue_buffer failed: %s (%d)",
                   strerror(-err), -err);
            ret = UNKNOWN_ERROR;
            for(int i = 0; i < cnt; i++) {
                // Deallocate buffers when the native window is gone
                struct ion_handle_data ion_handle;
                memset(&ion_handle, 0, sizeof(ion_handle));
                ion_handle.handle = mMemInfo[i].handle;
#ifndef TARGET_ION_ABI_VERSION
                if (ioctl(mMemInfo[i].main_ion_fd, ION_IOC_FREE, &ion_handle) < 0) {
                    ALOGE("ion free failed");
                }
                close(mMemInfo[i].main_ion_fd);
#else
                ion_close(mMemInfo[i].main_ion_fd);
#endif //TARGET_ION_ABI_VERSION
                if(mLocalFlag[i] != BUFFER_NOT_OWNED) {
                    err = mWindow->cancel_buffer(mWindow, mBufferHandle[i]);
                    LOGH("cancel_buffer: hdl =%p", (*mBufferHandle[i]));
                }
                mLocalFlag[i] = BUFFER_NOT_OWNED;
                mBufferHandle[i] = NULL;
            }
            reset();
            goto end;
        }

        mPrivateHandle[cnt] =
            (struct private_handle_t *)(*mBufferHandle[cnt]);
        //update max fps info
        setMetaData(mPrivateHandle[cnt], UPDATE_REFRESH_RATE, (void*)&mMaxFPS);
#ifndef TARGET_ION_ABI_VERSION
        mMemInfo[cnt].main_ion_fd = open("/dev/ion", O_RDONLY);
#else
        mMemInfo[cnt].main_ion_fd = ion_open();
#endif //TARGET_ION_ABI_VERSION
        if (mMemInfo[cnt].main_ion_fd < 0) {
            LOGE("failed: could not open ion device");
            for(int i = 0; i < cnt; i++) {
                struct ion_handle_data ion_handle;
                memset(&ion_handle, 0, sizeof(ion_handle));
                ion_handle.handle = mMemInfo[i].handle;
#ifndef TARGET_ION_ABI_VERSION
                if (ioctl(mMemInfo[i].main_ion_fd, ION_IOC_FREE, &ion_handle) < 0) {
                    LOGE("ion free failed");
                }
                close(mMemInfo[i].main_ion_fd);
#else
                ion_close(mMemInfo[i].main_ion_fd);
#endif //TARGET_ION_ABI_VERSION
                if(mLocalFlag[i] != BUFFER_NOT_OWNED) {
                    err = mWindow->cancel_buffer(mWindow, mBufferHandle[i]);
                    LOGH("cancel_buffer: hdl =%p", (*mBufferHandle[i]));
                }
                mLocalFlag[i] = BUFFER_NOT_OWNED;
                mBufferHandle[i] = NULL;
            }
            reset();
            ret = UNKNOWN_ERROR;
            goto end;
        } else {
            ion_info_fd.fd = mPrivateHandle[cnt]->fd;
#ifndef TARGET_ION_ABI_VERSION
            if (ioctl(mMemInfo[cnt].main_ion_fd,
                      ION_IOC_IMPORT, &ion_info_fd) < 0) {
                LOGE("ION import failed\n");
                for(int i = 0; i < cnt; i++) {
                    struct ion_handle_data ion_handle;
                    memset(&ion_handle, 0, sizeof(ion_handle));
                    ion_handle.handle = mMemInfo[i].handle;
                    if (ioctl(mMemInfo[i].main_ion_fd, ION_IOC_FREE, &ion_handle) < 0) {
                        LOGE("ion free failed");
                    }
                    close(mMemInfo[i].main_ion_fd);

                    if(mLocalFlag[i] != BUFFER_NOT_OWNED) {
                        err = mWindow->cancel_buffer(mWindow, mBufferHandle[i]);
                        LOGH("cancel_buffer: hdl =%p", (*mBufferHandle[i]));
                    }
                    mLocalFlag[i] = BUFFER_NOT_OWNED;
                    mBufferHandle[i] = NULL;
                }
                close(mMemInfo[cnt].main_ion_fd);
                reset();
                ret = UNKNOWN_ERROR;
                goto end;
            }
#else
           ion_info_fd.handle = ion_info_fd.fd;
#endif //TARGET_ION_ABI_VERSION
        }
        setMetaData(mPrivateHandle[cnt], UPDATE_COLOR_SPACE, &mColorSpace);
        mCameraMemory[cnt] =
            mGetMemory(mPrivateHandle[cnt]->fd,
                    (size_t)mPrivateHandle[cnt]->size,
                    1,
                    mCallbackCookie);
        LOGH("idx = %d, fd = %d, size = %d, offset = %d",
               cnt, mPrivateHandle[cnt]->fd,
              mPrivateHandle[cnt]->size,
              mPrivateHandle[cnt]->offset);
        mMemInfo[cnt].fd = mPrivateHandle[cnt]->fd;
        mMemInfo[cnt].size = (size_t)mPrivateHandle[cnt]->size;
        mMemInfo[cnt].handle = ion_info_fd.handle;
    }

    //Cancel min_undequeued_buffer buffers back to the window
    for (int i = 0; i < mMinUndequeuedBuffers; i ++) {
        err = mWindow->cancel_buffer(mWindow, mBufferHandle[i]);
        mLocalFlag[i] = BUFFER_NOT_OWNED;
    }

end:
    if (ret != NO_ERROR) {
        mMappableBuffers = 0;
    }
    LOGD("X ");
    ATRACE_END();
    return ret;
}


/*===========================================================================
 * FUNCTION   : allocateMore
 *
 * DESCRIPTION: allocate more requested number of buffers of certain size
 *
 * PARAMETERS :
 *   @count   : number of buffers to be allocated
 *   @size    : lenght of the buffer to be allocated
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraGrallocMemory::allocateMore(uint8_t /*count*/, size_t /*size*/)
{
    LOGE("Not implenmented yet");
    return UNKNOWN_ERROR;
}

/*===========================================================================
 * FUNCTION   : deallocate
 *
 * DESCRIPTION: deallocate buffers
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraGrallocMemory::deallocate()
{
    LOGD("E ", __FUNCTION__);

    for (int cnt = 0; cnt < mMappableBuffers; cnt++) {
        mCameraMemory[cnt]->release(mCameraMemory[cnt]);
        struct ion_handle_data ion_handle;
        memset(&ion_handle, 0, sizeof(ion_handle));
        ion_handle.handle = mMemInfo[cnt].handle;
#ifndef TARGET_ION_ABI_VERSION
        if (ioctl(mMemInfo[cnt].main_ion_fd, ION_IOC_FREE, &ion_handle) < 0) {
            LOGE("ion free failed");
        }
        close(mMemInfo[cnt].main_ion_fd);
#else
       ion_close(mMemInfo[cnt].main_ion_fd);
#endif //TARGET_ION_ABI_VERSION
        if(mLocalFlag[cnt] != BUFFER_NOT_OWNED) {
            if (mWindow) {
                mWindow->cancel_buffer(mWindow, mBufferHandle[cnt]);
                LOGH("cancel_buffer: hdl =%p", (*mBufferHandle[cnt]));
            } else {
                LOGE("Preview window is NULL, cannot cancel_buffer: hdl =%p",
                      (*mBufferHandle[cnt]));
            }
        }
        mLocalFlag[cnt] = BUFFER_NOT_OWNED;
        LOGH("put buffer %d successfully", cnt);
    }
    mBufferCount = 0;
    mMappableBuffers = 0;
    LOGD("X ",__FUNCTION__);
}

/*===========================================================================
 * FUNCTION   : cacheOps
 *
 * DESCRIPTION: ion related memory cache operations
 *
 * PARAMETERS :
 *   @index   : index of the buffer
 *   @cmd     : cache ops command
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraGrallocMemory::cacheOps(uint32_t index, unsigned int cmd)
{
    if (index >= mMappableBuffers)
        return BAD_INDEX;
    return cacheOpsInternal(index, cmd, mCameraMemory[index]->data);
}

/*===========================================================================
 * FUNCTION   : getRegFlags
 *
 * DESCRIPTION: query initial reg flags
 *
 * PARAMETERS :
 *   @regFlags: initial reg flags of the allocated buffers
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int QCameraGrallocMemory::getRegFlags(uint8_t *regFlags) const
{
    int i = 0;
    for (i = 0; i < mMinUndequeuedBuffers; i ++)
        regFlags[i] = 0;
    for (; i < mMappableBuffers; i ++)
        regFlags[i] = 1;
    for (; i < mBufferCount; i ++)
        regFlags[i] = 0;
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : getMemory
 *
 * DESCRIPTION: get camera memory
 *
 * PARAMETERS :
 *   @index   : buffer index
 *   @metadata: flag if it's metadata
 *
 * RETURN     : camera memory ptr
 *              NULL if not supported or failed
 *==========================================================================*/
camera_memory_t *QCameraGrallocMemory::getMemory(uint32_t index,
        bool metadata) const
{
    if (index >= mMappableBuffers || metadata)
        return NULL;
    return mCameraMemory[index];
}

/*===========================================================================
 * FUNCTION   : getMatchBufIndex
 *
 * DESCRIPTION: query buffer index by opaque ptr
 *
 * PARAMETERS :
 *   @opaque  : opaque ptr
 *   @metadata: flag if it's metadata
 *
 * RETURN     : buffer index if match found,
 *              -1 if failed
 *==========================================================================*/
int QCameraGrallocMemory::getMatchBufIndex(const void *opaque,
                                           bool metadata) const
{
    int index = -1;
    if (metadata) {
        return -1;
    }
    for (int i = 0; i < mMappableBuffers; i++) {
        if (mCameraMemory[i]->data == opaque) {
            index = i;
            break;
        }
    }
    return index;
}

/*===========================================================================
 * FUNCTION   : getPtr
 *
 * DESCRIPTION: return buffer pointer
 *
 * PARAMETERS :
 *   @index   : index of the buffer
 *
 * RETURN     : buffer ptr
 *==========================================================================*/
void *QCameraGrallocMemory::getPtr(uint32_t index) const
{
    if (index >= mMappableBuffers) {
        LOGE("index out of bound");
        return (void *)BAD_INDEX;
    }
    return mCameraMemory[index]->data;
}

/*===========================================================================
 * FUNCTION   : setMappable
 *
 * DESCRIPTION: configure the number of buffers ready to map
 *
 * PARAMETERS :
 *   @mappable : the number of desired mappable buffers
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraGrallocMemory::setMappable(uint8_t mappable)
{
    if (mMappableBuffers == 0) {
        mMappableBuffers = mappable;
    }
}

/*===========================================================================
 * FUNCTION   : getMappable
 *
 * DESCRIPTION: query number of buffers already allocated
 *
 * PARAMETERS : none
 *
 * RETURN     : number of buffers already allocated
 *==========================================================================*/
uint8_t QCameraGrallocMemory::getMappable() const
{
    return mMappableBuffers;
}

/*===========================================================================
 * FUNCTION   : checkIfAllBuffersMapped
 *
 * DESCRIPTION: check if all buffers for the are mapped
 *
 * PARAMETERS : none
 *
 * RETURN     : 1 if all buffers mapped
 *              0 if total buffers not equal to mapped buffers
 *==========================================================================*/
uint8_t QCameraGrallocMemory::checkIfAllBuffersMapped() const
{
    LOGH("mBufferCount: %d, mMappableBuffers: %d",
             mBufferCount, mMappableBuffers);
    return (mBufferCount == mMappableBuffers);
}

/*===========================================================================
 * FUNCTION   : setBufferStatus
 *
 * DESCRIPTION: set buffer status
 *
 * PARAMETERS :
 *   @index   : index of the buffer
 *   @status  : status of the buffer, whether skipped,etc
 *
 * RETURN     : none
 *==========================================================================*/
void QCameraGrallocMemory::setBufferStatus(uint32_t index, BufferStatus status)
{
    if (index >= mBufferCount) {
        LOGE("index out of bound");
        return;
    }
    mBufferStatus[index] = status;
}

}; //namespace qcamera
