#ifndef BELL_QUEUE_H
#define BELL_QUEUE_H

#include <queue>
#include <atomic>
#include <psp2/kernel/threadmgr.h>

namespace bell
{
    // Vita replacement for bell's std::condition_variable queue, on kernel
    // mutex + cond. Waiters check the queue under the lock before sleeping and
    // push signals while holding it, so a push can never slip between the
    // check and the wait. The old version waited unconditionally: a signal sent
    // before the wait was lost and audio chunk data sat in the queue.
    template <typename dataType>
    class Queue
    {
    private:
        std::queue<dataType> m_queue;
        int mutexid = sceKernelCreateMutex("bell_queue_mtx", 0, 0, NULL);
        int m_cv = sceKernelCreateCond("bell_queue_cv", 0, mutexid, NULL);
        /// Set by clear() to release wpop and wtpop immediately
        std::atomic<bool> m_forceExit = false;

    public:
        void push(dataType const &data)
        {
            m_forceExit.store(false);
            sceKernelLockMutex(mutexid, 1, NULL);
            m_queue.push(data);
            sceKernelSignalCond(m_cv);
            sceKernelUnlockMutex(mutexid, 1);
        }

        bool isEmpty() const
        {
            sceKernelLockMutex(mutexid, 1, NULL);
            bool tmp = m_queue.empty();
            sceKernelUnlockMutex(mutexid, 1);
            return tmp;
        }

        /// Returns false if the queue is empty.
        bool pop(dataType &popped_value)
        {
            sceKernelLockMutex(mutexid, 1, NULL);
            bool ok = !m_queue.empty();
            if (ok)
            {
                popped_value = m_queue.front();
                m_queue.pop();
            }
            sceKernelUnlockMutex(mutexid, 1);
            return ok;
        }

        /// Waits for an element. Returns false on forced exit.
        bool wpop(dataType &popped_value)
        {
            sceKernelLockMutex(mutexid, 1, NULL);
            while (m_queue.empty() && !m_forceExit.load())
            {
                sceKernelWaitCond(m_cv, NULL);
            }
            bool ok = !m_forceExit.load() && !m_queue.empty();
            if (ok)
            {
                popped_value = m_queue.front();
                m_queue.pop();
            }
            sceKernelUnlockMutex(mutexid, 1);
            return ok;
        }

        /// Waits up to `milliseconds` for an element. Returns false on timeout
        /// or forced exit.
        bool wtpop(dataType &popped_value, long milliseconds = 1000)
        {
            sceKernelLockMutex(mutexid, 1, NULL);
            if (m_queue.empty() && !m_forceExit.load())
            {
                SceUInt32 us = milliseconds * 1000;
                sceKernelWaitCond(m_cv, &us);
            }
            bool ok = !m_forceExit.load() && !m_queue.empty();
            if (ok)
            {
                popped_value = m_queue.front();
                m_queue.pop();
            }
            sceKernelUnlockMutex(mutexid, 1);
            return ok;
        }

        int size()
        {
            sceKernelLockMutex(mutexid, 1, NULL);
            auto tmp = static_cast<int>(m_queue.size());
            sceKernelUnlockMutex(mutexid, 1);
            return tmp;
        }

        /// Empties the queue and releases every waiter.
        void clear()
        {
            m_forceExit.store(true);
            sceKernelLockMutex(mutexid, 1, NULL);
            while (!m_queue.empty())
            {
                m_queue.pop();
            }
            sceKernelSignalCondAll(m_cv);
            sceKernelUnlockMutex(mutexid, 1);
        }

        bool isExit() const
        {
            return m_forceExit.load();
        }
    };
}

#endif
