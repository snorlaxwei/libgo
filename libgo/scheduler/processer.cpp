#include "processer.h"
#include "scheduler.h"
#include "../common/error.h"
#include "../common/clock.h"
#include <assert.h>
#include "ref.h"

namespace co {

int Processer::s_check_ = 0;

Processer::Processer(Scheduler * scheduler, int id)
    : scheduler_(scheduler), id_(id), stop_(scheduler->stop_)
{
}

Processer* & Processer::GetCurrentProcesser()
{
    static thread_local Processer *proc = nullptr;
    return proc;
}

Scheduler* Processer::GetCurrentScheduler()
{
    auto proc = GetCurrentProcesser();
    return proc ? proc->scheduler_ : nullptr;
}

void Processer::AddTask(Task *tk)
{
    DebugPrint(dbg_task | dbg_scheduler, "task(%s) add into proc(%u)(%p)", tk->DebugInfo(), id_, (void*)this);
    newQueue_.push(tk);
    newQueue_.AssertLink();

    OnAddTask();
}

void Processer::AddTask(SList<Task> && slist)
{
    DebugPrint(dbg_scheduler, "task(num=%d) add into proc(%u)", (int)slist.size(), id_);
    newQueue_.push(std::move(slist));
    newQueue_.AssertLink();

    OnAddTask();
}

void Processer::OnAddTask()
{
    if (IsWaiting()) {
        waiting_ = false;
        NotifyCondition();
    }
}

void Processer::Process()
{
    GetCurrentProcesser() = this;

    bool & isStop = *stop_;

    while (!isStop)
    {
        runnableQueue_.front(runningTask_);

        if (!runningTask_) {
            if (AddNewTasks())
                runnableQueue_.front(runningTask_);

            if (!runningTask_) {
                WaitCondition();
                AddNewTasks();
                continue;
            }
        }

#if ENABLE_DEBUGGER
        DebugPrint(dbg_scheduler, "Run [Proc(%d) QueueSize:%lu] --------------------------", id_, RunnableSize());
#endif

        addNewQuota_ = 1;
        while (runningTask_ && !isStop) {
            runningTask_->state_ = TaskState::runnable;
            runningTask_->proc_ = this;

#if ENABLE_DEBUGGER
            DebugPrint(dbg_switch, "enter task(%s)", runningTask_->DebugInfo());
            if (Listener::GetTaskListener())
                Listener::GetTaskListener()->onSwapIn(runningTask_->id_);
#endif

            ++switchCount_;

            runningTask_->SwapIn();

#if ENABLE_DEBUGGER
            DebugPrint(dbg_switch, "leave task(%s) state=%d", runningTask_->DebugInfo(), (int)runningTask_->state_);
#endif

            switch (runningTask_->state_) {
                case TaskState::runnable:
                    {
                        std::unique_lock<TaskQueue::lock_t> lock(runnableQueue_.LockRef());
                        auto next = (Task*)runningTask_->next;
                        if (next) {
                            runningTask_ = next;
                            runningTask_->check_ = runnableQueue_.check_;
                            break;
                        }

                        if (addNewQuota_ < 1 || newQueue_.emptyUnsafe()) {
                            runningTask_ = nullptr;
                        } else {
                            lock.unlock();
                            if (AddNewTasks()) {
                                runnableQueue_.next(runningTask_, runningTask_);
                                -- addNewQuota_;
                            } else {
                                std::unique_lock<TaskQueue::lock_t> lock2(runnableQueue_.LockRef());
                                runningTask_ = nullptr;
                            }
                        }

                    }
                    break;

                case TaskState::block:
                    {
                        std::unique_lock<TaskQueue::lock_t> lock(runnableQueue_.LockRef());
                        runningTask_ = nextTask_;
                        nextTask_ = nullptr;
                    }
                    break;

                case TaskState::done:
                default:
                    {
                        runnableQueue_.next(runningTask_, nextTask_);
                        if (!nextTask_ && addNewQuota_ > 0) {
                            if (AddNewTasks()) {
                                runnableQueue_.next(runningTask_, nextTask_);
                                -- addNewQuota_;
                            }
                        }

                        DebugPrint(dbg_task, "task(%s) done.", runningTask_->DebugInfo());
                        runnableQueue_.erase(runningTask_);
                        if (gcQueue_.size() > 16)
                            GC();
                        gcQueue_.push(runningTask_);
                        if (runningTask_->eptr_) {
                            std::exception_ptr ep = runningTask_->eptr_;
                            std::rethrow_exception(ep);
                        }

                        std::unique_lock<TaskQueue::lock_t> lock(runnableQueue_.LockRef());
                        runningTask_ = nextTask_;
                        nextTask_ = nullptr;
                    }
                    break;
            }
        }
    }
}

Task* Processer::GetCurrentTask()
{
    auto proc = GetCurrentProcesser();
    return proc ? proc->runningTask_ : nullptr;
}

bool Processer::IsCoroutine()
{
    return !!GetCurrentTask();
}

std::size_t Processer::RunnableSize()
{
    return runnableQueue_.size() + newQueue_.size();
}

void Processer::NotifyCondition()
{
    cv_.notify_all();
}

void Processer::WaitCondition()
{
    GC();
    std::unique_lock<std::mutex> lock(cvMutex_);
    waiting_ = true;
    cv_.wait_for(lock, std::chrono::milliseconds(100));
    waiting_ = false;
}

void Processer::GC()
{
    auto list = gcQueue_.pop_all();
    for (Task & tk : list) {
        tk.DecrementRef();
    }
    list.clear();
}

bool Processer::AddNewTasks()
{
    if (newQueue_.emptyUnsafe()) return false;

    runnableQueue_.push(newQueue_.pop_all());
    newQueue_.AssertLink();
    return true;
}

bool Processer::IsBlocking()
{
    if (!markSwitch_ || markSwitch_ != switchCount_) return false;
    return NowMicrosecond() > markTick_ + CoroutineOptions::getInstance().cycle_timeout_us;
}

void Processer::Mark()
{
    if (runningTask_ && markSwitch_ != switchCount_) {
        markSwitch_ = switchCount_;
        markTick_ = NowMicrosecond();
    }
}

int64_t Processer::NowMicrosecond()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(FastSteadyClock::now().time_since_epoch()).count();
}

SList<Task> Processer::Steal(std::size_t n)
{
    if (n > 0) {
        // steal some
        newQueue_.AssertLink();
        auto slist = newQueue_.pop_back(n);
        newQueue_.AssertLink();
        if (slist.size() >= n)
            return slist;

        std::unique_lock<TaskQueue::lock_t> lock(runnableQueue_.LockRef());
        bool pushRunningTask = false, pushNextTask = false;
        if (runningTask_)
            pushRunningTask = runnableQueue_.eraseWithoutLock(runningTask_, true) || slist.erase(runningTask_, newQueue_.check_);
        if (nextTask_)
            pushNextTask = runnableQueue_.eraseWithoutLock(nextTask_, true) || slist.erase(nextTask_, newQueue_.check_);
        auto slist2 = runnableQueue_.pop_backWithoutLock(n - slist.size());
        if (pushRunningTask)
            runnableQueue_.pushWithoutLock(runningTask_);
        if (pushNextTask)
            runnableQueue_.pushWithoutLock(nextTask_);
        lock.unlock();

        slist2.append(std::move(slist));
        if (!slist2.empty())
            DebugPrint(dbg_scheduler, "Proc(%d).Stealed = %d", id_, (int)slist2.size());
        return slist2;
    } else {
        // steal all
        newQueue_.AssertLink();
        auto slist = newQueue_.pop_all();
        newQueue_.AssertLink();

        std::unique_lock<TaskQueue::lock_t> lock(runnableQueue_.LockRef());
        bool pushRunningTask = false, pushNextTask = false;
        if (runningTask_)
            pushRunningTask = runnableQueue_.eraseWithoutLock(runningTask_, true) || slist.erase(runningTask_, newQueue_.check_);
        if (nextTask_)
            pushNextTask = runnableQueue_.eraseWithoutLock(nextTask_, true) || slist.erase(nextTask_, newQueue_.check_);
        auto slist2 = runnableQueue_.pop_allWithoutLock();
        if (pushRunningTask)
            runnableQueue_.pushWithoutLock(runningTask_);
        if (pushNextTask)
            runnableQueue_.pushWithoutLock(nextTask_);
        lock.unlock();

        slist2.append(std::move(slist));
        if (!slist2.empty())
            DebugPrint(dbg_scheduler, "Proc(%d).Stealed all = %d", id_, (int)slist2.size());
        return slist2;
    }
}

Processer::SuspendEntry Processer::Suspend()
{
    Task* tk = GetCurrentTask();
    assert(tk);
    assert(tk->proc_);
    return tk->proc_->SuspendBySelf(tk);
}

Processer::SuspendEntry Processer::Suspend(FastSteadyClock::duration dur)
{
    SuspendEntry entry = Suspend();
    GetCurrentScheduler()->GetTimer().StartTimer(dur,
            [entry]() mutable {
                Processer::Wakeup(entry);
            });
    return entry;
}
Processer::SuspendEntry Processer::Suspend(FastSteadyClock::time_point timepoint)
{
    SuspendEntry entry = Suspend();
    GetCurrentScheduler()->GetTimer().StartTimer(timepoint,
            [entry]() mutable {
                Processer::Wakeup(entry);
            });
    return entry;
}

Processer::SuspendEntry Processer::SuspendBySelf(Task* tk)
{
    assert(tk == runningTask_);
    assert(tk->state_ == TaskState::runnable);

    tk->state_ = TaskState::block;
    uint64_t id = ++ TaskRefSuspendId(tk);

    runnableQueue_.next(runningTask_, nextTask_);
    if (!nextTask_ && addNewQuota_ > 0) {
        if (AddNewTasks()) {
            runnableQueue_.next(runningTask_, nextTask_);
            -- addNewQuota_;
        }
    }

    DebugPrint(dbg_suspend, "tk(%s) Suspend. nextTask(%s)", tk->DebugInfo(), nextTask_->DebugInfo());

    runnableQueue_.erase(runningTask_);
    waitQueue_.push(runningTask_);
    return SuspendEntry{ WeakPtr<Task>(tk), id };
}

bool Processer::IsExpire(SuspendEntry const& entry)
{
    IncursivePtr<Task> tkPtr = entry.tk_.lock();
    if (!tkPtr) return true;
    if (entry.id_ != TaskRefSuspendId(tkPtr.get())) return true;
    return false;
}

bool Processer::Wakeup(SuspendEntry const& entry)
{
    IncursivePtr<Task> tkPtr = entry.tk_.lock();
    if (!tkPtr) return false;

    auto proc = tkPtr->proc_;
    return proc ? proc->WakeupBySelf(tkPtr, entry.id_) : false;
}

bool Processer::WakeupBySelf(IncursivePtr<Task> const& tkPtr, uint64_t id)
{
    Task* tk = tkPtr.get();

    if (id != TaskRefSuspendId(tk)) return false;

    {
        std::unique_lock<TaskQueue::lock_t> lock(waitQueue_.LockRef());
        if (id != TaskRefSuspendId(tk)) return false;
        DebugPrint(dbg_suspend, "tk(%s) Wakeup. tk->state_ = %s", tk->DebugInfo(), GetTaskStateName(tk->state_));
        ++ TaskRefSuspendId(tk);
        bool ret = waitQueue_.eraseWithoutLock(tk, true);
        (void)ret;
        assert(ret);
    }

    runnableQueue_.push(tk);
    OnAddTask();
    return true;
}

} //namespace co

