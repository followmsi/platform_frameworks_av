/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "C2Buffer"
#include <utils/Log.h>

#include <C2BufferPriv.h>

#include <ion/ion.h>
#include <sys/mman.h>

namespace android {

// standard ERRNO mappings
template<int N> constexpr C2Error _c2_errno2error_impl();
template<> constexpr C2Error _c2_errno2error_impl<0>()       { return C2_OK; }
template<> constexpr C2Error _c2_errno2error_impl<EINVAL>()  { return C2_BAD_VALUE; }
template<> constexpr C2Error _c2_errno2error_impl<EACCES>()  { return C2_NO_PERMISSION; }
template<> constexpr C2Error _c2_errno2error_impl<EPERM>()   { return C2_NO_PERMISSION; }
template<> constexpr C2Error _c2_errno2error_impl<ENOMEM>()  { return C2_NO_MEMORY; }

// map standard errno-s to the equivalent C2Error
template<int... N> struct _c2_map_errno_impl;
template<int E, int ... N> struct _c2_map_errno_impl<E, N...> {
    static C2Error map(int result) {
        if (result == E) {
            return _c2_errno2error_impl<E>();
        } else {
            return _c2_map_errno_impl<N...>::map(result);
        }
    }
};
template<> struct _c2_map_errno_impl<> {
    static C2Error map(int result) {
        return result == 0 ? C2_OK : C2_CORRUPTED;
    }
};

template<int... N>
C2Error c2_map_errno(int result) {
    return _c2_map_errno_impl<N...>::map(result);
}

namespace {

// Inherit from the parent, share with the friend.

class DummyCapacityAspect : public _C2LinearCapacityAspect {
    using _C2LinearCapacityAspect::_C2LinearCapacityAspect;
    friend class ::android::C2ReadView;
    friend class ::android::C2ConstLinearBlock;
};

class C2DefaultReadView : public C2ReadView {
    using C2ReadView::C2ReadView;
    friend class ::android::C2ConstLinearBlock;
};

class C2DefaultWriteView : public C2WriteView {
    using C2WriteView::C2WriteView;
    friend class ::android::C2LinearBlock;
};

class C2AcquirableReadView : public C2Acquirable<C2ReadView> {
    using C2Acquirable::C2Acquirable;
    friend class ::android::C2ConstLinearBlock;
};

class C2AcquirableWriteView : public C2Acquirable<C2WriteView> {
    using C2Acquirable::C2Acquirable;
    friend class ::android::C2LinearBlock;
};

class C2DefaultConstLinearBlock : public C2ConstLinearBlock {
    using C2ConstLinearBlock::C2ConstLinearBlock;
    friend class ::android::C2LinearBlock;
};

class C2DefaultLinearBlock : public C2LinearBlock {
    using C2LinearBlock::C2LinearBlock;
    friend class ::android::C2DefaultBlockAllocator;
};

}  // namespace

/* ======================================= ION ALLOCATION ====================================== */

/**
 * ION handle
 */
struct C2HandleIon : public C2Handle {
    C2HandleIon(int ionFd, ion_user_handle_t buffer) : C2Handle(cHeader),
          mFds{ ionFd, buffer },
          mInts{ kMagic } { }

    static bool isValid(const C2Handle * const o);

    int ionFd() const { return mFds.mIon; }
    ion_user_handle_t buffer() const { return mFds.mBuffer; }

    void setBuffer(ion_user_handle_t bufferFd) { mFds.mBuffer = bufferFd; }

protected:
    struct {
        int mIon;
        int mBuffer; // ion_user_handle_t
    } mFds;
    struct {
        int mMagic;
    } mInts;

private:
    typedef C2HandleIon _type;
    enum {
        kMagic = 'ion1',
        numFds = sizeof(mFds) / sizeof(int),
        numInts = sizeof(mInts) / sizeof(int),
        version = sizeof(C2Handle) + sizeof(mFds) + sizeof(mInts)
    };
    //constexpr static C2Handle cHeader = { version, numFds, numInts, {} };
    const static C2Handle cHeader;
};

const C2Handle C2HandleIon::cHeader = {
    C2HandleIon::version,
    C2HandleIon::numFds,
    C2HandleIon::numInts,
    {}
};

// static
bool C2HandleIon::isValid(const C2Handle * const o) {
    if (!o || memcmp(o, &cHeader, sizeof(cHeader))) {
        return false;
    }
    const C2HandleIon *other = static_cast<const C2HandleIon*>(o);
    return other->mInts.mMagic == kMagic;
}

// TODO: is the dup of an ion fd identical to ion_share?

class C2AllocationIon : public C2LinearAllocation {
public:
    virtual C2Error map(
        size_t offset, size_t size, C2MemoryUsage usage, int *fence,
        void **addr /* nonnull */);
    virtual C2Error unmap(void *addr, size_t size, int *fenceFd);
    virtual bool isValid() const;
    virtual ~C2AllocationIon();
    virtual const C2Handle *handle() const;
    virtual bool equals(const std::shared_ptr<C2LinearAllocation> &other) const;

    // internal methods
    C2AllocationIon(int ionFd, size_t size, size_t align, unsigned heapMask, unsigned flags);
    C2AllocationIon(int ionFd, size_t size, int shareFd);
    int dup() const;
    C2Error status() const;

protected:
    class Impl;
    Impl *mImpl;
};

class C2AllocationIon::Impl {
public:
    // NOTE: using constructor here instead of a factory method as we will need the
    // error value and this simplifies the error handling by the wrapper.
    Impl(int ionFd, size_t capacity, size_t align, unsigned heapMask, unsigned flags)
        : mInit(C2_OK),
          mHandle(ionFd, -1),
          mMapFd(-1),
          mCapacity(capacity) {
        ion_user_handle_t buffer = -1;
        int ret = ion_alloc(mHandle.ionFd(), mCapacity, align, heapMask, flags, &buffer);
        if (ret == 0) {
            mHandle.setBuffer(buffer);
        } else {
            mInit = c2_map_errno<ENOMEM, EACCES, EINVAL>(-ret);
        }
    }

    Impl(int ionFd, size_t capacity, int shareFd)
        : mHandle(ionFd, -1),
          mMapFd(-1),
          mCapacity(capacity) {
        ion_user_handle_t buffer;
        mInit = ion_import(mHandle.ionFd(), shareFd, &buffer);
        if (mInit == 0) {
            mHandle.setBuffer(buffer);
        }
        (void)mCapacity; // TODO
    }

    C2Error map(size_t offset, size_t size, C2MemoryUsage usage, int *fenceFd, void **addr) {
        (void)fenceFd; // TODO: wait for fence
        *addr = nullptr;
        int prot = PROT_NONE;
        int flags = MAP_PRIVATE;
        if (usage.mConsumer & GRALLOC_USAGE_SW_READ_MASK) {
            prot |= PROT_READ;
        }
        if (usage.mProducer & GRALLOC_USAGE_SW_WRITE_MASK) {
            prot |= PROT_WRITE;
            flags = MAP_SHARED;
        }

        size_t alignmentBytes = offset % PAGE_SIZE;
        size_t mapOffset = offset - alignmentBytes;
        size_t mapSize = size + alignmentBytes;

        C2Error err = C2_OK;
        if (mMapFd == -1) {
            int ret = ion_map(mHandle.ionFd(), mHandle.buffer(), mapSize, prot,
                              flags, mapOffset, (unsigned char**)&mMapAddr, &mMapFd);
            if (ret) {
                mMapFd = -1;
                *addr = nullptr;
                err = c2_map_errno<EINVAL>(-ret);
            } else {
                *addr = (uint8_t *)mMapAddr + alignmentBytes;
                mMapAlignmentBytes = alignmentBytes;
                mMapSize = mapSize;
            }
        } else {
            mMapAddr = mmap(nullptr, mapSize, prot, flags, mMapFd, mapOffset);
            if (mMapAddr == MAP_FAILED) {
                mMapAddr = *addr = nullptr;
                err = c2_map_errno<EINVAL>(errno);
            } else {
                *addr = (uint8_t *)mMapAddr + alignmentBytes;
                mMapAlignmentBytes = alignmentBytes;
                mMapSize = mapSize;
            }
        }
        return err;
    }

    C2Error unmap(void *addr, size_t size, int *fenceFd) {
        if (addr != (uint8_t *)mMapAddr + mMapAlignmentBytes ||
                size + mMapAlignmentBytes != mMapSize) {
            return C2_BAD_VALUE;
        }
        int err = munmap(mMapAddr, mMapSize);
        if (err != 0) {
            return c2_map_errno<EINVAL>(errno);
        }
        if (fenceFd) {
            *fenceFd = -1;
        }
        return C2_OK;
    }

    ~Impl() {
        if (mMapFd != -1) {
            close(mMapFd);
            mMapFd = -1;
        }

        (void)ion_free(mHandle.ionFd(), mHandle.buffer());
    }

    C2Error status() const {
        return mInit;
    }

    const C2Handle * handle() const {
        return &mHandle;
    }

    int dup() const {
        int fd = -1;
        if (mInit != 0 || ion_share(mHandle.ionFd(), mHandle.buffer(), &fd) != 0) {
            fd = -1;
        }
        return fd;
    }

private:
    C2Error mInit;
    C2HandleIon mHandle;
    int mMapFd; // only one for now
    void *mMapAddr;
    size_t mMapAlignmentBytes;
    size_t mMapSize;
    size_t mCapacity;
};

C2Error C2AllocationIon::map(
    size_t offset, size_t size, C2MemoryUsage usage, int *fenceFd, void **addr) {
    return mImpl->map(offset, size, usage, fenceFd, addr);
}

C2Error C2AllocationIon::unmap(void *addr, size_t size, int *fenceFd) {
    return mImpl->unmap(addr, size, fenceFd);
}

bool C2AllocationIon::isValid() const {
    return mImpl->status() == C2_OK;
}

C2Error C2AllocationIon::status() const {
    return mImpl->status();
}

bool C2AllocationIon::equals(const std::shared_ptr<C2LinearAllocation> &other) const {
    return other != nullptr &&
        other->handle(); // TODO
}

const C2Handle *C2AllocationIon::handle() const {
    return mImpl->handle();
}

C2AllocationIon::~C2AllocationIon() {
    delete mImpl;
}

C2AllocationIon::C2AllocationIon(int ionFd, size_t size, size_t align, unsigned heapMask, unsigned flags)
    : C2LinearAllocation(size),
      mImpl(new Impl(ionFd, size, align, heapMask, flags)) { }

C2AllocationIon::C2AllocationIon(int ionFd, size_t size, int shareFd)
    : C2LinearAllocation(size),
      mImpl(new Impl(ionFd, size, shareFd)) { }

int C2AllocationIon::dup() const {
    return mImpl->dup();
}

/* ======================================= ION ALLOCATOR ====================================== */

C2AllocatorIon::C2AllocatorIon() : mInit(C2_OK), mIonFd(ion_open()) {
    if (mIonFd < 0) {
        switch (errno) {
        case ENOENT:    mInit = C2_UNSUPPORTED; break;
        default:        mInit = c2_map_errno<EACCES>(errno); break;
        }
    }
}

C2AllocatorIon::~C2AllocatorIon() {
    if (mInit == C2_OK) {
        ion_close(mIonFd);
    }
}

/**
 * Allocates a 1D allocation of given |capacity| and |usage|. If successful, the allocation is
 * stored in |allocation|. Otherwise, |allocation| is set to 'nullptr'.
 *
 * \param capacity        the size of requested allocation (the allocation could be slightly
 *                      larger, e.g. to account for any system-required alignment)
 * \param usage           the memory usage info for the requested allocation. \note that the
 *                      returned allocation may be later used/mapped with different usage.
 *                      The allocator should layout the buffer to be optimized for this usage,
 *                      but must support any usage. One exception: protected buffers can
 *                      only be used in a protected scenario.
 * \param allocation      pointer to where the allocation shall be stored on success. nullptr
 *                      will be stored here on failure
 *
 * \retval C2_OK        the allocation was successful
 * \retval C2_NO_MEMORY not enough memory to complete the allocation
 * \retval C2_TIMED_OUT the allocation timed out
 * \retval C2_NO_PERMISSION     no permission to complete the allocation
 * \retval C2_BAD_VALUE capacity or usage are not supported (invalid) (caller error)
 * \retval C2_UNSUPPORTED       this allocator does not support 1D allocations
 * \retval C2_CORRUPTED some unknown, unrecoverable error occured during allocation (unexpected)
 */
C2Error C2AllocatorIon::allocateLinearBuffer(
        uint32_t capacity, C2MemoryUsage usage, std::shared_ptr<C2LinearAllocation> *allocation) {
    *allocation = nullptr;
    if (mInit != C2_OK) {
        return C2_UNSUPPORTED;
    }

    // get align, heapMask and flags
    //size_t align = 1;
    size_t align = 0;
    unsigned heapMask = ~0;
    unsigned flags = 0;
    //TODO
    (void) usage;
#if 0
    int err = mUsageMapper(usage, capacity, &align, &heapMask, &flags);
    if (err < 0) {
        return c2_map_errno<EINVAL, ENOMEM, EACCES>(-err);
    }
#endif

    std::shared_ptr<C2AllocationIon> alloc
        = std::make_shared<C2AllocationIon>(mIonFd, capacity, align, heapMask, flags);
    C2Error ret = alloc->status();
    if (ret == C2_OK) {
        *allocation = alloc;
    }
    return ret;
}

/**
 * (Re)creates a 1D allocation from a native |handle|. If successful, the allocation is stored
 * in |allocation|. Otherwise, |allocation| is set to 'nullptr'.
 *
 * \param handle      the handle for the existing allocation
 * \param allocation  pointer to where the allocation shall be stored on success. nullptr
 *                  will be stored here on failure
 *
 * \retval C2_OK        the allocation was recreated successfully
 * \retval C2_NO_MEMORY not enough memory to recreate the allocation
 * \retval C2_TIMED_OUT the recreation timed out (unexpected)
 * \retval C2_NO_PERMISSION     no permission to recreate the allocation
 * \retval C2_BAD_VALUE invalid handle (caller error)
 * \retval C2_UNSUPPORTED       this allocator does not support 1D allocations
 * \retval C2_CORRUPTED some unknown, unrecoverable error occured during allocation (unexpected)
 */
C2Error C2AllocatorIon::recreateLinearBuffer(
        const C2Handle *handle, std::shared_ptr<C2LinearAllocation> *allocation) {
    *allocation = nullptr;
    if (mInit != C2_OK) {
        return C2_UNSUPPORTED;
    }

    if (!C2HandleIon::isValid(handle)) {
        return C2_BAD_VALUE;
    }

    // TODO: get capacity and validate it
    const C2HandleIon *h = static_cast<const C2HandleIon*>(handle);
    std::shared_ptr<C2AllocationIon> alloc
        = std::make_shared<C2AllocationIon>(mIonFd, 0 /* capacity */, h->buffer());
    C2Error ret = alloc->status();
    if (ret == C2_OK) {
        *allocation = alloc;
    }
    return ret;
}

/* ========================================== 1D BLOCK ========================================= */

class C2Block1D::Impl {
public:
    const C2Handle *handle() const {
        return mAllocation->handle();
    }

    Impl(std::shared_ptr<C2LinearAllocation> alloc)
        : mAllocation(alloc) {}

private:
    std::shared_ptr<C2LinearAllocation> mAllocation;
};

const C2Handle *C2Block1D::handle() const {
    return mImpl->handle();
};

C2Block1D::C2Block1D(std::shared_ptr<C2LinearAllocation> alloc)
    : _C2LinearRangeAspect(alloc.get()), mImpl(new Impl(alloc)) {
}

C2Block1D::C2Block1D(std::shared_ptr<C2LinearAllocation> alloc, size_t offset, size_t size)
    : _C2LinearRangeAspect(alloc.get(), offset, size), mImpl(new Impl(alloc)) {
}

class C2ReadView::Impl {
public:
    explicit Impl(const uint8_t *data)
        : mData(data), mError(C2_OK) {}

    explicit Impl(C2Error error)
        : mData(nullptr), mError(error) {}

    const uint8_t *data() const {
        return mData;
    }

    C2Error error() const {
        return mError;
    }

private:
    const uint8_t *mData;
    C2Error mError;
};

C2ReadView::C2ReadView(const _C2LinearCapacityAspect *parent, const uint8_t *data)
    : _C2LinearCapacityAspect(parent), mImpl(std::make_shared<Impl>(data)) {}

C2ReadView::C2ReadView(C2Error error)
    : _C2LinearCapacityAspect(0u), mImpl(std::make_shared<Impl>(error)) {}

const uint8_t *C2ReadView::data() const {
    return mImpl->data();
}

C2ReadView C2ReadView::subView(size_t offset, size_t size) const {
    if (offset > capacity()) {
        offset = capacity();
    }
    if (size > capacity() - offset) {
        size = capacity() - offset;
    }
    // TRICKY: newCapacity will just be used to grab the size.
    DummyCapacityAspect newCapacity((uint32_t)size);
    return C2ReadView(&newCapacity, data() + offset);
}

C2Error C2ReadView::error() {
    return mImpl->error();
}

class C2WriteView::Impl {
public:
    explicit Impl(uint8_t *base)
        : mBase(base), mError(C2_OK) {}

    explicit Impl(C2Error error)
        : mBase(nullptr), mError(error) {}

    uint8_t *base() const {
        return mBase;
    }

    C2Error error() const {
        return mError;
    }

private:
    uint8_t *mBase;
    C2Error mError;
};

C2WriteView::C2WriteView(const _C2LinearRangeAspect *parent, uint8_t *base)
    : _C2EditableLinearRange(parent), mImpl(std::make_shared<Impl>(base)) {}

C2WriteView::C2WriteView(C2Error error)
    : _C2EditableLinearRange(nullptr), mImpl(std::make_shared<Impl>(error)) {}

uint8_t *C2WriteView::base() { return mImpl->base(); }

uint8_t *C2WriteView::data() { return mImpl->base() + offset(); }

C2Error C2WriteView::error() { return mImpl->error(); }

class C2ConstLinearBlock::Impl {
public:
    explicit Impl(std::shared_ptr<C2LinearAllocation> alloc)
        : mAllocation(alloc), mBase(nullptr), mSize(0u), mError(C2_CORRUPTED) {}

    ~Impl() {
        if (mBase != nullptr) {
            // TODO: fence
            C2Error err = mAllocation->unmap(mBase, mSize, nullptr);
            if (err != C2_OK) {
                // TODO: Log?
            }
        }
    }

    C2ConstLinearBlock subBlock(size_t offset, size_t size) const {
        return C2ConstLinearBlock(mAllocation, offset, size);
    }

    void map(size_t offset, size_t size) {
        if (mBase == nullptr) {
            void *base = nullptr;
            mError = mAllocation->map(
                    offset, size, { C2MemoryUsage::kSoftwareRead, 0 }, nullptr, &base);
            // TODO: fence
            if (mError == C2_OK) {
                mBase = (uint8_t *)base;
                mSize = size;
            }
        }
    }

    const uint8_t *base() const { return mBase; }

    C2Error error() const { return mError; }

private:
    std::shared_ptr<C2LinearAllocation> mAllocation;
    uint8_t *mBase;
    size_t mSize;
    C2Error mError;
};

C2ConstLinearBlock::C2ConstLinearBlock(std::shared_ptr<C2LinearAllocation> alloc)
    : C2Block1D(alloc), mImpl(std::make_shared<Impl>(alloc)) {}

C2ConstLinearBlock::C2ConstLinearBlock(
        std::shared_ptr<C2LinearAllocation> alloc, size_t offset, size_t size)
    : C2Block1D(alloc, offset, size), mImpl(std::make_shared<Impl>(alloc)) {}

C2Acquirable<C2ReadView> C2ConstLinearBlock::map() const {
    mImpl->map(offset(), size());
    if (mImpl->base() == nullptr) {
        C2DefaultReadView view(mImpl->error());
        return C2AcquirableReadView(mImpl->error(), mFence, view);
    }
    DummyCapacityAspect newCapacity(size());
    C2DefaultReadView view(&newCapacity, mImpl->base());
    return C2AcquirableReadView(mImpl->error(), mFence, view);
}

C2ConstLinearBlock C2ConstLinearBlock::subBlock(size_t offset, size_t size) const {
    return mImpl->subBlock(offset, size);
}

class C2LinearBlock::Impl {
public:
    Impl(std::shared_ptr<C2LinearAllocation> alloc)
        : mAllocation(alloc), mBase(nullptr), mSize(0u), mError(C2_CORRUPTED) {}

    ~Impl() {
        if (mBase != nullptr) {
            // TODO: fence
            C2Error err = mAllocation->unmap(mBase, mSize, nullptr);
            if (err != C2_OK) {
                // TODO: Log?
            }
        }
    }

    void map(size_t capacity) {
        if (mBase == nullptr) {
            void *base = nullptr;
            // TODO: fence
            mError = mAllocation->map(
                    0u,
                    capacity,
                    { C2MemoryUsage::kSoftwareRead, C2MemoryUsage::kSoftwareWrite },
                    nullptr,
                    &base);
            if (mError == C2_OK) {
                mBase = (uint8_t *)base;
                mSize = capacity;
            }
        }
    }

    C2ConstLinearBlock share(size_t offset, size_t size, C2Fence &fence) {
        // TODO
        (void) fence;
        return C2DefaultConstLinearBlock(mAllocation, offset, size);
    }

    uint8_t *base() const { return mBase; }

    C2Error error() const { return mError; }

    C2Fence fence() const { return mFence; }

private:
    std::shared_ptr<C2LinearAllocation> mAllocation;
    uint8_t *mBase;
    size_t mSize;
    C2Error mError;
    C2Fence mFence;
};

C2LinearBlock::C2LinearBlock(std::shared_ptr<C2LinearAllocation> alloc)
    : C2Block1D(alloc),
      mImpl(new Impl(alloc)) {}

C2LinearBlock::C2LinearBlock(std::shared_ptr<C2LinearAllocation> alloc, size_t offset, size_t size)
    : C2Block1D(alloc, offset, size),
      mImpl(new Impl(alloc)) {}

C2Acquirable<C2WriteView> C2LinearBlock::map() {
    mImpl->map(capacity());
    if (mImpl->base() == nullptr) {
        C2DefaultWriteView view(mImpl->error());
        return C2AcquirableWriteView(mImpl->error(), mImpl->fence(), view);
    }
    C2DefaultWriteView view(this, mImpl->base());
    view.setOffset_be(offset());
    view.setSize_be(size());
    return C2AcquirableWriteView(mImpl->error(), mImpl->fence(), view);
}

C2ConstLinearBlock C2LinearBlock::share(size_t offset, size_t size, C2Fence fence) {
    return mImpl->share(offset, size, fence);
}

C2DefaultBlockAllocator::C2DefaultBlockAllocator(
        const std::shared_ptr<C2Allocator> &allocator)
  : mAllocator(allocator) {}

C2Error C2DefaultBlockAllocator::allocateLinearBlock(
        uint32_t capacity,
        C2MemoryUsage usage,
        std::shared_ptr<C2LinearBlock> *block /* nonnull */) {
    block->reset();

    std::shared_ptr<C2LinearAllocation> alloc;
    C2Error err = mAllocator->allocateLinearBuffer(capacity, usage, &alloc);
    if (err != C2_OK) {
        return err;
    }

    block->reset(new C2DefaultLinearBlock(alloc));

    return C2_OK;
}

} // namespace android