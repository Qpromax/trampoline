//
// Created by Qpromax on 2025/9/27.
//

#include <type_traits>
#include <utility>
#include <memory>
#include <vector>
#include <stdexcept>
#include <cassert>
#include <iostream>

// 值类别枚举
enum class ValueCategory {
    LValue, // 左值
    RValue // 右值
};

// 栈元素元数据
struct ElementMetadata {
    ValueCategory category;
    bool aboutToDestroy;

    ElementMetadata(ValueCategory c = ValueCategory::LValue, bool d = false)
        : category(c), aboutToDestroy(d){}
};

// 类型擦除操作表
struct TypeOperations {
    void (*destructor)(void*);

    void (*moveConstruct)(void* dest, void* src);

    void* (*getAddress)(void* storage);
};

// 小对象优化存储（16字节缓冲区）
union Storage {
    void* dynamic;
    alignas(16) char buffer[16];

    Storage() noexcept : dynamic(nullptr)
    {
    }

    // 移动构造函数
    Storage(Storage&& other) noexcept
    {
        if (this != &other)
        {
            dynamic = other.dynamic;
            other.dynamic = nullptr;
        }
    }

    // 移动赋值运算符
    Storage& operator=(Storage&& other) noexcept
    {
        if (this != &other)
        {
            dynamic = other.dynamic;
            other.dynamic = nullptr;
        }
        return *this;
    }

    // 禁用拷贝
    Storage(const Storage&) = delete;

    Storage& operator=(const Storage&) = delete;
};

// 栈元素实现
class StackElementImpl {
public:
    // 默认构造函数（创建空元素）
    StackElementImpl() : meta_(), ops_(nullptr){}

    // 带参数的构造函数
    template<typename T>
    StackElementImpl(T&& value, ElementMetadata meta)
        : meta_(meta), ops_(&operationsFor<std::decay_t<T> >)
    {
        construct(std::forward<T>(value));
    }

    // 移动构造函数
    StackElementImpl(StackElementImpl&& other) noexcept
        : meta_(other.meta_), ops_(other.ops_), storage_(std::move(other.storage_))
    {
        other.ops_ = nullptr; // 防止双重释放
    }

    // 移动赋值运算符
    StackElementImpl& operator=(StackElementImpl&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            meta_ = other.meta_;
            ops_ = other.ops_;
            storage_ = std::move(other.storage_);
            other.ops_ = nullptr;
        }
        return *this;
    }

    // 析构函数
    ~StackElementImpl()
    {
        destroy();
    }

    // 禁用拷贝
    StackElementImpl(const StackElementImpl&) = delete;

    StackElementImpl& operator=(const StackElementImpl&) = delete;

    const ElementMetadata& metadata() const noexcept { return meta_; }

    void* data() noexcept
    {
        return ops_ ? ops_->getAddress(&storage_) : nullptr;
    }

    const void* data() const noexcept
    {
        return ops_ ? ops_->getAddress(const_cast<Storage*>(&storage_)) : nullptr;
    }

    // 检查是否有效
    bool valid() const noexcept
    {
        return ops_ != nullptr;
    }

private:
    template<typename T>
    static TypeOperations operationsFor;

    // 构造元素
    template<typename T>
    void construct(T&& value)
    {
        using RawType = std::decay_t<T>;

        if constexpr (sizeof(RawType) <= sizeof(Storage) &&
                      alignof(RawType) <= alignof(Storage) &&
                      std::is_nothrow_move_constructible_v<RawType>)
        {
            // 小对象优化：直接在缓冲区构造
            new(&storage_.buffer) RawType(std::forward<T>(value));
        } else
        {
            // 大对象：动态分配
            storage_.dynamic = new RawType(std::forward<T>(value));
        }
    }

    // 销毁元素
    void destroy()
    {
        if (ops_)
        {
            ops_->destructor(&storage_);
            ops_ = nullptr;
        }
    }

    ElementMetadata meta_;
    const TypeOperations* ops_;
    Storage storage_;
};

// 类型操作表特化
template<typename T>
TypeOperations StackElementImpl::operationsFor = {
    // 析构函数
    [](void* storage) {
        Storage* s = static_cast<Storage*>(storage);
        if constexpr (sizeof(T) <= sizeof(Storage) &&
                      alignof(T) <= alignof(Storage) &&
                      std::is_nothrow_move_constructible_v<T>)
        {
            reinterpret_cast<T*>(s->buffer)->~T();
        } else
        {
            delete static_cast<T*>(s->dynamic);
        }
    },
    // 移动构造
    [](void* dest, void* src) {
        Storage* d = static_cast<Storage*>(dest);
        Storage* s = static_cast<Storage*>(src);

        if constexpr (sizeof(T) <= sizeof(Storage) &&
                      alignof(T) <= alignof(Storage) &&
                      std::is_nothrow_move_constructible_v<T>)
        {
            new(d->buffer) T(std::move(*reinterpret_cast<T*>(s->buffer)));
        } else
        {
            d->dynamic = s->dynamic;
            s->dynamic = nullptr;
        }
    },
    // 获取数据指针
    [](void* storage) -> void* {
        Storage* s = static_cast<Storage*>(storage);
        if constexpr (sizeof(T) <= sizeof(Storage) &&
                      alignof(T) <= alignof(Storage) &&
                      std::is_nothrow_move_constructible_v<T>)
        {
            return s->buffer;
        } else
        {
            return s->dynamic;
        }
    }
};

// 高性能通用栈
class HighPerfStack {
public:
    HighPerfStack() = default;

    // 移动构造
    HighPerfStack(HighPerfStack&& other) noexcept
        : stack_(std::move(other.stack_))
    {
    }

    // 移动赋值
    HighPerfStack& operator=(HighPerfStack&& other) noexcept
    {
        stack_ = std::move(other.stack_);
        return *this;
    }

    // 禁用拷贝
    HighPerfStack(const HighPerfStack&) = delete;

    HighPerfStack& operator=(const HighPerfStack&) = delete;

    ~HighPerfStack() = default;

    // 压栈（完美转发）
    template<typename T>
    void push(T&& value, bool aboutToDestroy = false)
    {
        ElementMetadata meta;
        if constexpr (std::is_lvalue_reference_v<decltype(value)>)
        {
            meta.category = ValueCategory::LValue;
        } else
        {
            meta.category = ValueCategory::RValue;
        }
        meta.aboutToDestroy = aboutToDestroy;

        // 直接在vector中构造元素
        stack_.emplace_back(std::forward<T>(value), meta);
    }

    // 弹出元素
    void pop()
    {
        if (stack_.empty())
        {
            throw std::out_of_range("Stack is empty");
        }
        stack_.pop_back();
    }

    // 访问栈顶元素（指定类型）
    template<typename T>
    auto top() -> decltype(auto)
    {
        if (stack_.empty())
        {
            throw std::out_of_range("Stack is empty");
        }
        return getAs<T>(stack_.back());
    }

    // 访问栈顶元素（指定类型，const）
    template<typename T>
    auto top() const -> decltype(auto)
    {
        if (stack_.empty())
        {
            throw std::out_of_range("Stack is empty");
        }
        return getAs<T>(stack_.back());
    }

    // 获取元数据
    const ElementMetadata& topMetadata() const
    {
        if (stack_.empty())
        {
            throw std::out_of_range("Stack is empty");
        }
        return stack_.back().metadata();
    }

    // 检查是否为空
    bool empty() const noexcept
    {
        return stack_.empty();
    }

    // 获取大小
    size_t size() const noexcept
    {
        return stack_.size();
    }

    // 预分配内存
    void reserve(size_t capacity)
    {
        stack_.reserve(capacity);
    }

private:
    // 类型安全的访问
    template<typename T, typename Element>
    static auto getAs(Element& element) -> decltype(auto)
    {
        using RawType = std::decay_t<T>;
        void* data = element.data();

        if (!data)
        {
            throw std::runtime_error("Invalid stack element");
        }

        if constexpr (std::is_lvalue_reference_v<T>)
        {
            // 左值引用
            return static_cast<std::add_lvalue_reference_t<RawType>>(
                *static_cast<RawType*>(data)
            );
        } else if constexpr (std::is_rvalue_reference_v<T>)
        {
            // 右值引用
            return static_cast<std::add_rvalue_reference_t<RawType>>(
                std::move(*static_cast<RawType*>(data))
            );
        } else
        {
            // 值类型
            return *static_cast<RawType*>(data);
        }
    }

    std::vector<StackElementImpl> stack_;
};