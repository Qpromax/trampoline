基本用法示例
```
#include <iostream>
#include "trampoline.h"

using namespace Trampoline;

int main() {
    Stack<> stack;

    // push: 压栈任意类型
    stack.push(42);                    // int
    stack.push(std::string("hello"));  // std::string

    // emplace: 原地构造对象
    stack.emplace<std::pair<int,std::string>>(1, "one");

    // top<T>: 类型安全访问
    std::string& str = stack.top<std::string>();
    std::cout << "Top string: " << str << "\n";

    // try_top<T>: 无异常安全访问
    if (auto maybeInt = stack.try_top<int>()) {
        maybeInt->get() += 1;
    }

    // pop: 弹栈
    stack.pop();

    // empty / size
    if (!stack.empty()) std::cout << "Stack size: " << stack.size() << "\n";

    return 0;
}
```