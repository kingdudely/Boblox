// refcount.h — intrusive reference counting for engine objects.
//
// Instances form a tree (parent holds children strongly, children hold the
// parent WEAKLY), so no cycles; Lua userdata hold strong refs. Single-
// threaded like the rest of the runtime — no atomics.
#pragma once

#include <utility>

namespace rbx {

class RefCounted {
  public:
    RefCounted() = default;
    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;
    virtual ~RefCounted() = default;

    void add_ref() const {
        ++refcount_;
    }
    void release() const {
        if (--refcount_ == 0)
            delete this;
    }

  private:
    mutable int refcount_ = 0;
};

// Strong pointer for RefCounted types (intrusive).
template <typename T>
class Ref {
  public:
    Ref() = default;
    Ref(std::nullptr_t) {
    }
    Ref(T* p) : ptr_(p) { // NOLINT(google-explicit-constructor) — like shared_ptr
        if (ptr_)
            ptr_->add_ref();
    }
    Ref(const Ref& o) : ptr_(o.ptr_) {
        if (ptr_)
            ptr_->add_ref();
    }
    Ref(Ref&& o) noexcept : ptr_(o.ptr_) {
        o.ptr_ = nullptr;
    }
    ~Ref() {
        reset();
    }

    Ref& operator=(const Ref& o) {
        if (this != &o) {
            if (o.ptr_)
                o.ptr_->add_ref();
            reset_Keep();
            ptr_ = o.ptr_;
        }
        return *this;
    }
    Ref& operator=(Ref&& o) noexcept {
        if (this != &o) {
            reset_Keep();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    void reset() {
        reset_Keep();
    }
    T* get() const {
        return ptr_;
    }
    T* operator->() const {
        return ptr_;
    }
    T& operator*() const {
        return *ptr_;
    }
    explicit operator bool() const {
        return ptr_ != nullptr;
    }
    bool operator==(const Ref& o) const {
        return ptr_ == o.ptr_;
    }

  private:
    void reset_Keep() {
        if (ptr_) {
            ptr_->release();
            ptr_ = nullptr;
        }
    }
    T* ptr_ = nullptr;
};

} // namespace rbx
