/**
 * keyvalues.h
 *
 * Provides a function to load a "BuildManifest" from a .vdf file
 * using Tyti's VDF parser. We store the results into a list of
 * VPKKeyValues_t. This replicates the original "KeyValues" usage.
 */

#ifndef KEYVALUES_H
#define KEYVALUES_H
#include <atomic>

#include <string>
#include <vector>
#include "packedstore.h"
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
// We can include Tyti's VDF parser. E.g. if you have "tyti_vdf_parser.h"
#include "tyti_vdf_parser.h"

// ------------------------------------------------------------------
// LoadKeyValuesManifest:
//  Expects a top-level object "BuildManifest" with multiple children.
//   Each child’s name = local filesystem path
//   Contains fields:
//     "preloadSize"
//     "loadFlags"
//     "textureFlags"
//     "useCompression"
//     "deDuplicate"
// ------------------------------------------------------------------
bool LoadKeyValuesManifest(const std::string& vdfPath, std::vector<VPKKeyValues_t>& outList);

class ThreadPool {
public:
    ThreadPool(size_t numThreads)
        : stop(false), tasksInProgress(0)
    {
        for (size_t i = 0; i < numThreads; i++)
        {
            workers.emplace_back([this](){
                while (true)
                {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queueMutex);
                        condition.wait(lock, [this](){ return stop || !tasks.empty(); });
                        if (stop && tasks.empty())
                            return;
                        task = std::move(tasks.front());
                        tasks.pop();
                        tasksInProgress++;
                    }
                    task();
                    tasksInProgress--;
                    waitCondition.notify_all();
                }
            });
        }
    }

    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers)
            worker.join();
    }

    // Enqueue a task into the pool.
    void enqueue(std::function<void()> task)
    {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            tasks.push(std::move(task));
        }
        condition.notify_one();
    }

    // Block until all tasks have completed.
    void wait()
    {
        std::unique_lock<std::mutex> lock(waitMutex);
        waitCondition.wait(lock, [this](){
            std::unique_lock<std::mutex> lock(queueMutex);
            return tasks.empty() && tasksInProgress.load() == 0;
        });
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;

    std::mutex waitMutex;
    std::condition_variable waitCondition;
    std::atomic<int> tasksInProgress;
};

#endif // KEYVALUES_H
