#include "thread_pool.hpp"

#include <iostream>

int main() {
    thread_pool pool(2);
    auto result = pool.submit([](int a, int b) { return a + b; }, 3, 4);
    const int value = result.get();
    std::cout << value << '\n';
    return value == 7 ? 0 : 1;
} // 析构时排空队列并等待全部 worker 退出。
