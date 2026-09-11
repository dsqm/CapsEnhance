#ifndef COMPTR_H
#define COMPTR_H

#include <windows.h>

/* ================= 最小 COM 智能指针（只做"RAII 释放"一件事） =================
   为什么不引 ATL 的 CComPtr / WRL 的 ComPtr：本项目只要"出作用域就 Release"，
   为此拉一个几千行、还带自己异常/断言配置的头不值当。这里 40 行足够。

   **只包"拥有的引用"**：GetXXX / CreateXXX / AddRef 拿到的那些。
   **绝不能包借来的指针** —— 函数参数、遍历拿到的父元素都只借用。历史上正是
   有人把调用方传进 `FindDocElement` 的 `chain[0]` 一起 Release 了，引用计数
   归零、对象被销毁，随后调用方的 Release 读到已释放内存里的 vtable →
   `c0000005`。用 ComPtr 之后这条规则变成可读的：**包起来的 = 我们负责放的**。

   ctor 语义是"**接管**"（不 AddRef），与 WRL 的 Attach 一致 —— 因为这些指针
   拿到手时那份引用本来就已经归我们；再 AddRef 一次就是泄漏。
   要"再取一份引用"（共享）用拷贝构造 / 赋值，那里会 AddRef。 */
template <class T>
class ComPtr {
public:
    ComPtr() : p_(NULL) {}
    /* 接管一份已有引用。explicit：避免 raw -> ComPtr 的隐式转换满天飞 */
    explicit ComPtr(T* p) : p_(p) {}
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->lpVtbl->AddRef(p_); }
    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) { T* t = o.p_; if (t) t->lpVtbl->AddRef(t); reset(); p_ = t; }
        return *this;
    }
    ~ComPtr() { reset(); }

    T*  get() const { return p_; }
    T*  operator->() const { return p_; }
    /* 交给 GetXXX / CreateXXX 填的 out 参数（旧值先放掉，避免覆盖即泄漏） */
    T** put() { reset(); return &p_; }
    explicit operator bool() const { return p_ != NULL; }

    void reset() {
        if (p_) { p_->lpVtbl->Release(p_); p_ = NULL; }
    }
    /* 放弃所有权，把裸指针交给别人（转交引用时用） */
    T* detach() { T* t = p_; p_ = NULL; return t; }

private:
    T* p_;
};

#endif
