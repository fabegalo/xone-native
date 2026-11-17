// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace xboxone::era {
// Reference 1401bf790/1401bf9e0/1401c11c0: publish, consume in order, drain before destruction.
class GraphicsCallStream {
public:
    GraphicsCallStream():worker_([this] {run();}) {}
    GraphicsCallStream(const GraphicsCallStream&)=delete;
    GraphicsCallStream& operator=(const GraphicsCallStream&)=delete;
    ~GraphicsCallStream() {
        {
            std::lock_guard lock(mutex_);
            closing_=true;
        }
        ready_.notify_all();space_.notify_all();
        worker_.join();
    }
    bool on_worker() const {return std::this_thread::get_id()==worker_.get_id();}
    template<class F> auto submit(F&& operation) -> std::future<std::invoke_result_t<F>> {
        using Result=std::invoke_result_t<F>;
        std::packaged_task<Result()> task(std::forward<F>(operation));
        auto result=task.get_future();
        std::packaged_task<void()> call([task=std::move(task)]() mutable {task();});
        {
            std::unique_lock lock(mutex_);
            space_.wait(lock,[this] {return closing_ || calls_.size()<256;});
            if(closing_)throw std::runtime_error("graphics stream is closing");
            calls_.push_back(std::move(call));
        }
        ready_.notify_one();
        return result;
    }
    template<class F> auto invoke(F&& operation) -> std::invoke_result_t<F> {
        if(on_worker())return std::forward<F>(operation)();
        return submit(std::forward<F>(operation)).get();
    }
private:
    void run() {
        for(;;) {
            std::packaged_task<void()> call;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock,[this] {return closing_ || !calls_.empty();});
                if(calls_.empty())return;
                call=std::move(calls_.front());calls_.pop_front();
            }
            space_.notify_one();
            call();
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_,space_;
    std::deque<std::packaged_task<void()>> calls_;
    bool closing_=false;
    std::thread worker_;
};
}
