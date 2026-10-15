#pragma once

#include <memory>
#include <utility>

// GPU's base destructor runs after derived sensor/fdinfo members have gone.
// Join the polling thread before entering any derived destructor instead.
template<class T>
struct PollingGPUDeleter {
    void operator()(T* gpu) const
    {
        if (gpu) {
            gpu->stop_polling();
            delete gpu;
        }
    }
};

template<class Base, class Derived, class... Args>
std::unique_ptr<Base, PollingGPUDeleter<Base>> make_unique_polling_gpu(Args&&... args)
{
    return std::unique_ptr<Base, PollingGPUDeleter<Base>>(
        new Derived(std::forward<Args>(args)...));
}

template<class Derived, class... Args>
std::shared_ptr<Derived> make_shared_polling_gpu(Args&&... args)
{
    return std::shared_ptr<Derived>(new Derived(std::forward<Args>(args)...),
                                    PollingGPUDeleter<Derived>{});
}
