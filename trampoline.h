// Created by Qpromax on 2025/9/27.
// trampoline.h
// Header-only high-performance heterogeneous stack with SOO and optimized moves
// Requires C++17 (C++20 recommended)

#ifndef TRAMPOLINE_H
#define TRAMPOLINE_H

#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>
#include <optional>
#include <functional>
#include <stdexcept>
#include <new>
#include <cstring>

namespace Trampoline {
    struct bad_trampoline_cast : std::bad_cast {
        const char* what() const noexcept override { return "bad Trampoline cast"; }
    };

    inline constexpr std::size_t DefaultBufferSize = 16;
    inline constexpr std::size_t DefaultBufferAlign = alignof(std::max_align_t);

    template<std::size_t BufferBytes = DefaultBufferSize, std::size_t Align = DefaultBufferAlign>
    class Trampoline {
        static_assert(BufferBytes >= sizeof(void*), "Buffer must be at least pointer-sized");

    public:
        Trampoline() = default;
        ~Trampoline() = default;

        Trampoline(const Trampoline&) = delete;
        Trampoline& operator=(const Trampoline&) = delete;

        Trampoline(Trampoline&&) noexcept = default;
        Trampoline& operator=(Trampoline&&) noexcept = default;

        // emplace: construct in-place
        template<typename T, typename... Args>
        void emplace(Args&&... args)
        {
            elements_.push_back(Element::template create<T>(std::forward<Args>(args)...));
        }

        // push: copy/move a value
        template<typename T>
        void push(T&& value)
        {
            emplace<std::decay_t<T> >(std::forward<T>(value));
        }

        void pop()
        {
            if (elements_.empty()) throw std::out_of_range("Trampoline::pop on empty");
            elements_.pop_back();
        }

        template<typename T>
        T& top()
        {
            if (elements_.empty()) throw std::out_of_range("Trampoline::top on empty");
            Element& e = elements_.back();
            if (e.type_idx != std::type_index(typeid(T))) throw bad_trampoline_cast();
            return *reinterpret_cast<T*>(e.getAddress());
        }

        template<typename T>
        const T& top() const
        {
            if (elements_.empty()) throw std::out_of_range("Trampoline::top on empty");
            const Element& e = elements_.back();
            if (e.type_idx != std::type_index(typeid(T))) throw bad_trampoline_cast();
            return *reinterpret_cast<const T*>(e.getAddress());
        }

        template<typename T>
        std::optional<std::reference_wrapper<T> > try_top()
        {
            if (elements_.empty()) return std::nullopt;
            Element& e = elements_.back();
            if (e.type_idx != std::type_index(typeid(T))) return std::nullopt;
            return std::ref(*reinterpret_cast<T*>(e.getAddress()));
        }

        bool empty() const noexcept { return elements_.empty(); }
        std::size_t size() const noexcept { return elements_.size(); }
        void reserve(std::size_t n) { elements_.reserve(n); }

    private:
        union Buffer {
            alignas(Align) unsigned char bytes[BufferBytes];
            void* as_ptr;
        };

        struct Element {
            Buffer buffer{};
            void* heap_ptr{nullptr};
            std::type_index type_idx{typeid(void)};
            using Destructor = void(*)(Buffer*) noexcept;
            using MoveConstruct = void(*)(Buffer* dest, Buffer* src) noexcept;
            using GetAddress = void*(*)(Buffer*) noexcept;
            Destructor destructor{nullptr};
            MoveConstruct moveConstruct{nullptr};
            GetAddress getAddressFn{nullptr};

            Element() noexcept = default;

            // static helper to create a new element
            template<typename T, typename... Args>
            static Element create(Args&&... args)
            {
                Element e;
                e.type_idx = std::type_index(typeid(T));

                constexpr bool fitsBuffer = sizeof(T) <= BufferBytes && alignof(T) <= Align;
                if constexpr (fitsBuffer)
                {
                    void* dest = static_cast<void*>(e.buffer.bytes);
                    new(dest) T(std::forward<Args>(args)...);

                    // destructor
                    if constexpr (std::is_trivially_destructible_v<T>)
                    {
                        e.destructor = nullptr;
                    } else
                    {
                        e.destructor = [](Buffer* b) noexcept {
                            T* p = std::launder(reinterpret_cast<T*>(b->bytes));
                            p->~T();
                        };
                    }

                    // move construct
                    if constexpr (std::is_trivially_move_constructible_v<T>)
                    {
                        e.moveConstruct = [](Buffer* d, Buffer* s) noexcept {
                            std::memcpy(d->bytes, s->bytes, sizeof(T));
                        };
                    } else
                    {
                        e.moveConstruct = [](Buffer* d, Buffer* s) noexcept {
                            T* src = std::launder(reinterpret_cast<T*>(s->bytes));
                            void* dest = static_cast<void*>(d->bytes);
                            new(dest) T(std::move(*src));
                            src->~T();
                        };
                    }

                    e.getAddressFn = [](Buffer* b) noexcept -> void* {
                        return static_cast<void*>(b->bytes);
                    };
                } else
                {
                    // large object: heap
                    e.heap_ptr = new T(std::forward<Args>(args)...);
                    e.buffer.as_ptr = e.heap_ptr;
                    e.destructor = [](Buffer* b) noexcept {
                        delete reinterpret_cast<T*>(b->as_ptr);
                    };
                    e.moveConstruct = [](Buffer* d, Buffer* s) noexcept {
                        d->as_ptr = s->as_ptr;
                        s->as_ptr = nullptr;
                    };
                    e.getAddressFn = [](Buffer* b) noexcept -> void* {
                        return b->as_ptr;
                    };
                }
                return e;
            }

            ~Element()
            {
                if (destructor) destructor(&buffer);
            }

            Element(Element&& other) noexcept
                : buffer(other.buffer),
                heap_ptr(other.heap_ptr),
                type_idx(other.type_idx),
                destructor(other.destructor),
                moveConstruct(other.moveConstruct),
                getAddressFn(other.getAddressFn)
            {
                if (other.moveConstruct) other.moveConstruct(&buffer, &other.buffer);
                other.destructor = nullptr;
                other.moveConstruct = nullptr;
                other.getAddressFn = nullptr;
                other.type_idx = std::type_index(typeid(void));
                other.heap_ptr = nullptr;
            }

            Element& operator=(Element&& other) noexcept
            {
                if (this != &other) {
                    if (destructor) destructor(&buffer);

                    buffer = other.buffer;
                    heap_ptr = other.heap_ptr;
                    type_idx = other.type_idx;
                    destructor = other.destructor;
                    moveConstruct = other.moveConstruct;
                    getAddressFn = other.getAddressFn;

                    if (other.moveConstruct) other.moveConstruct(&buffer, &other.buffer);

                    other.destructor = nullptr;
                    other.moveConstruct = nullptr;
                    other.getAddressFn = nullptr;
                    other.type_idx = std::type_index(typeid(void));
                    other.heap_ptr = nullptr;
                }
                return *this;
            }

            Element(const Element&) = delete;

            Element& operator=(const Element&) = delete;

            void* getAddress() noexcept { return getAddressFn(&buffer); }
            const void* getAddress() const noexcept { return getAddressFn(const_cast<Buffer*>(&buffer)); }
        };

        std::vector<Element> elements_;
    };
} // namespace Trampoline

#endif // TRAMPOLINE_H
