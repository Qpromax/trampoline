// Created by Qpromax on 2025/9/27.
// trampoline.h
// Header-only high-performance heterogeneous stack with SOO and optimized moves
// Requires C++17 (recommended C++20).

#ifndef TRAMPOLINE_H
#define TRAMPOLINE_H

#include <type_traits>
#include <utility>
#include <vector>
#include <stdexcept>
#include <new>
#include <cstddef>
#include <cstring>
#include <functional>
#include <typeindex>
#include <optional>


namespace Trampoline {
    struct bad_trampoline_cast : public std::bad_cast {
        const char* what() const noexcept override { return "bad Trampoline cast"; }
    };

    // Default small buffer size (in bytes)
    inline constexpr std::size_t DefaultBufferSize = 16;

    // Template parameters:
    // BufferBytes: size of in-place buffer per element (default 16)
    // Align: alignment for the in-place buffer (default alignas(max_align_t))
    template<std::size_t BufferBytes = DefaultBufferSize, std::size_t Align = alignof(std::max_align_t)>
    class Trampoline {
        static_assert(BufferBytes >= sizeof(void*), "Buffer must be at least pointer-sized");

    public:
        Trampoline() = default;
        ~Trampoline() = default;

        Trampoline(const Trampoline&) = delete;
        Trampoline& operator=(const Trampoline&) = delete;

        Trampoline(Trampoline&&) noexcept = default;
        Trampoline& operator=(Trampoline&&) noexcept = default;

        // emplace: construct a T in-place in a new stack element
        template<typename T, typename... Args>
        void emplace(Args&&... args)
        {
            elements_.push_back(Element::template create<T>(std::forward<Args>(args)...));
        }

        // push: copy/move a value into the stack
        template<typename T>
        void push(T&& value)
        {
            emplace<std::decay_t<T> >(std::forward<T>(value));
        }

        // pop: remove top element
        void pop()
        {
            if (elements_.empty()) throw std::out_of_range("Trampoline::pop on empty");
            elements_.pop_back();
        }

        // topRaw: get void* pointer to the top object (or nullptr if empty)
        void* topRaw()
        {
            if (elements_.empty()) throw std::out_of_range("Trampoline::topRaw on empty");
            return elements_.back().getAddress();
        }

        const void* topRaw() const
        {
            if (elements_.empty()) throw std::out_of_range("Trampoline::topRaw on empty");
            return elements_.back().getAddress();
        }

        // top<T>: type-safe access to the top element (throws bad_trampoline_cast on mismatch)
        template<typename T>
        T& top()
        {
            if (elements_.empty()) throw std::out_of_range("Trampoline::top on empty");
            const auto& e = elements_.back();
            if (e.type() != std::type_index(typeid(T))) throw bad_trampoline_cast();
            return *reinterpret_cast<T*>(e.getAddress());
        }

        template<typename T>
        const T& top() const
        {
            if (elements_.empty()) throw std::out_of_range("Trampoline::top on empty");
            const auto& e = elements_.back();
            if (e.type() != std::type_index(typeid(T))) throw bad_trampoline_cast();
            return *reinterpret_cast<const T*>(e.getAddress());
        }

        // try_top<T>: returns optional reference (no exception)
        template<typename T>
        std::optional<std::reference_wrapper<T> > try_top()
        {
            if (elements_.empty()) return std::nullopt;
            auto& e = elements_.back();
            if (e.type() != std::type_index(typeid(T))) return std::nullopt;
            return std::optional<std::reference_wrapper<T> >{std::ref(*reinterpret_cast<T*>(e.getAddress()))};
        }

        // check empty / size
        bool empty() const noexcept { return elements_.empty(); }
        std::size_t size() const noexcept { return elements_.size(); }

        // reserve underlying storage
        void reserve(std::size_t n) { elements_.reserve(n); }

    private:
        // Internal storage for small-object optimization
        union Buffer {
            alignas(Align) unsigned char bytes[BufferBytes];
            void* as_ptr; // to ensure pointer-sized alignment operations are valid
        };

        // Element: type-erased holder that stores either object in Buffer or on heap
        struct Element {
            using Destructor = void(*)(Buffer*) noexcept;
            using MoveConstruct = void(*)(Buffer* dest, Buffer* src) noexcept;
            using GetAddress = void*(*)(Buffer*) noexcept;

            Buffer buffer{};
            void* heap_ptr{nullptr}; // for large objects
            Destructor destructor{nullptr};
            MoveConstruct moveConstruct{nullptr};
            GetAddress getAddressFn{nullptr};
            std::type_index type_idx{typeid(void)}; // runtime type info

            Element() noexcept = default;

            // create a new element for type T
            template<typename T, typename... Args>
            static Element create(Args&&... args)
            {
                Element e;
                e.type_idx = std::type_index(typeid(T));

                constexpr bool fitsBuffer = sizeof(T) <= BufferBytes && alignof(T) <= Align;
                if constexpr (fitsBuffer)
                {
                    // choose fastest safe move strategy for T
                    void* dest = static_cast<void*>(e.buffer.bytes);
                    new(dest) T(std::forward<Args>(args)...);

                    if constexpr (std::is_trivially_destructible_v<T>)
                    {
                        e.destructor = [](Buffer*) noexcept {
                            /* nothing */
                        };
                    } else
                    {
                        e.destructor = [](Buffer* b) noexcept {
                            T* p = std::launder(reinterpret_cast<T*>(b->bytes));
                            p->~T();
                        };
                    }

                    if constexpr (std::is_trivially_copyable_v<T>)
                    {
                        e.moveConstruct = [](Buffer* d, Buffer* s) noexcept {
                            std::memcpy(d->bytes, s->bytes, sizeof(T));
                        };
                    } else if constexpr (std::is_nothrow_move_constructible_v<T> || std::is_nothrow_move_assignable_v<
                                             T>)
                    {
                        e.moveConstruct = [](Buffer* d, Buffer* s) noexcept {
                            T* src = std::launder(reinterpret_cast<T*>(s->bytes));
                            void* dest = static_cast<void*>(d->bytes);
                            new(dest) T(std::move(*src));
                            // destroy source to avoid double-destruction
                            src->~T();
                        };
                    } else
                    {
                        // fallback to copy-construct if move might throw
                        e.moveConstruct = [](Buffer* d, Buffer* s) noexcept {
                            T* src = std::launder(reinterpret_cast<T*>(s->bytes));
                            void* dest = static_cast<void*>(d->bytes);
                            new(dest) T(*src);
                            src->~T();
                        };
                    }

                    e.getAddressFn = [](Buffer* b) noexcept -> void* {
                        return static_cast<void*>(b->bytes);
                    };
                } else
                {
                    // heap-allocated large object
                    e.heap_ptr = new T(std::forward<Args>(args)...);
                    e.destructor = [](Buffer* b) noexcept {
                        delete reinterpret_cast<T*>(b->as_ptr);
                    };
                    e.moveConstruct = [](Buffer* d, Buffer* s) noexcept {
                        // move pointer ownership
                        d->as_ptr = s->as_ptr;
                        s->as_ptr = nullptr;
                    };
                    e.getAddressFn = [](Buffer* b) noexcept -> void* {
                        return reinterpret_cast<void*>(b->as_ptr);
                    };
                    // store pointer in the union area for convenience
                    e.buffer.as_ptr = e.heap_ptr;
                }

                // wrap destructor to use buffer pointer semantics (heap case uses buffer.as_ptr)
                if (!e.destructor)
                {
                    e.destructor = [](Buffer*) noexcept {
                        /* no-op */
                    };
                }
                if (!e.moveConstruct)
                {
                    e.moveConstruct = [](Buffer* d, Buffer* s) noexcept {
                        /* no-op */
                    };
                }
                if (!e.getAddressFn)
                {
                    e.getAddressFn = [](Buffer* b) noexcept -> void* { return static_cast<void*>(b->bytes); };
                }

                return e;
            }

            ~Element()
            {
                if (destructor) destructor(&buffer);
            }

            // move constructor for Element (used by vector during growth)
            Element(Element&& other) noexcept
            {
                type_idx = other.type_idx;
                destructor = other.destructor;
                moveConstruct = other.moveConstruct;
                getAddressFn = other.getAddressFn;
                if (moveConstruct)
                {
                    moveConstruct(&buffer, &other.buffer);
                }
                // ensure other won't double-destroy
                other.destructor = nullptr;
                other.moveConstruct = nullptr;
                other.getAddressFn = nullptr;
                other.type_idx = std::type_index(typeid(void));
            }

            Element& operator=(Element&& other) noexcept
            {
                if (this != &other)
                {
                    if (destructor) destructor(&buffer);
                    type_idx = other.type_idx;
                    destructor = other.destructor;
                    moveConstruct = other.moveConstruct;
                    getAddressFn = other.getAddressFn;
                    if (moveConstruct) moveConstruct(&buffer, &other.buffer);
                    other.destructor = nullptr;
                    other.moveConstruct = nullptr;
                    other.getAddressFn = nullptr;
                    other.type_idx = std::type_index(typeid(void));
                }
                return *this;
            }

            // deleted copy
            Element(const Element&) = delete;

            Element& operator=(const Element&) = delete;

            // helpers
            void* getAddress() noexcept { return getAddressFn(&buffer); }
            const void* getAddress() const noexcept { return getAddressFn(const_cast<Buffer*>(&buffer)); }
            std::type_index type() const noexcept { return type_idx; }
        };

        std::vector<Element> elements_;
    };
}
#endif // TRAMPOLINE_H
