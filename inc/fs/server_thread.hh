#pragma once

#include <deque>
#include <future>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace hscfs {

/* 能够接受的任务，函数签名为void()，自身状态应该使用闭包维护 */
using task_t = void();

/*
 * HSCFS文件系统层的后台服务线程
 * 能够执行其他模块(或自身)发送的任务请求
 */
class server_thread
{
public:
    server_thread() noexcept {
        exit_req = false;
    }

    void start()
    {
        th_handle = std::thread(&server_thread::thread_main, this);
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lg(mtx);
            exit_req = true;
        }
        cond.notify_all();
        th_handle.join();
    }

    void post_task(std::packaged_task<task_t> &&task)
    {
        bool need_wakeup;
        {
            std::lock_guard<std::mutex> lg(mtx);
            need_wakeup = task_queue.empty();
            task_queue.emplace_back(std::move(task));
        }
        if (need_wakeup)
            cond.notify_all();
    }

private:

    std::deque<std::packaged_task<task_t>> task_queue;
    bool exit_req;
    std::mutex mtx;  // 保护上两个成员的锁
    std::condition_variable cond;

    std::thread th_handle;

private:

    void thread_main()
    {
        std::unique_lock<std::mutex> mtx_lg(mtx);

        while (true)
        {
            /* 若后续需要服务线程周期性的回写或GC，则需将此处修改为wait_for，增加相应逻辑 */
            cond.wait(mtx_lg, [this]() {
                return !this->task_queue.empty() || this->exit_req;
            });

            if (task_queue.empty() && exit_req)  // 收到退出信号，且没有需要执行的任务，则退出
                break;
            
            /* 还有任务需要执行 */
            if (!task_queue.empty())  // 此处仍然增加判断(当前逻辑本不需要)，为后续wait_for做兼容
            {
                std::packaged_task<task_t> task = std::move(task_queue.front());
                task_queue.pop_front();
                task();
            }
        }
    }
};

} // namespace hscfs