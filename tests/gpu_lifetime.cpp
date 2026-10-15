#include "../mangohud-next/server/metrics/gpu/gpu.hpp"
#include "../mangohud-next/server/metrics/gpu/ownership.hpp"

#include <condition_variable>
#include <stdexcept>

struct LifetimeState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool worker_finished = false;
    bool destroyed = false;
    bool destroyed_while_polling = false;
};

class LifetimeGPU : public GPU {
    struct Sensor {
        LifetimeState& state;
        std::unique_ptr<int> value = std::make_unique<int>(42);
        ~Sensor() {
            std::lock_guard lock(state.mutex);
            state.destroyed_while_polling = !state.worker_finished;
            state.destroyed = true;
        }
    } sensor;

public:
    explicit LifetimeGPU(LifetimeState& state)
        : GPU("renderD128", "0000:00:02.0", 0x8086, 0, "test-gpu"), sensor{state} {}

    void pre_poll_overrides() override {
        int* value = sensor.value.get();
        {
            std::lock_guard lock(sensor.state.mutex);
            sensor.state.entered = true;
            sensor.state.changed.notify_all();
        }
        // The production owner must signal stop and join before Sensor dies.
        while (!stop_thread)
            std::this_thread::sleep_for(1ms);
        if (*value != 42)
            std::terminate();
        std::lock_guard lock(sensor.state.mutex);
        sensor.state.worker_finished = true;
    }
};

static void wait_for_poll(LifetimeState& state)
{
    std::unique_lock lock(state.mutex);
    if (!state.changed.wait_for(lock, 2s, [&] { return state.entered; }))
        throw std::runtime_error("GPU worker did not enter polling");
}

static void verify(const LifetimeState& state)
{
    if (!state.destroyed || !state.worker_finished || state.destroyed_while_polling)
        throw std::runtime_error("GPU sensor destroyed before polling worker joined");
}

int main()
{
    for (unsigned repeat = 0; repeat < 10; ++repeat) {
        LifetimeState unique_state;
        auto unique = make_unique_polling_gpu<GPU, LifetimeGPU>(unique_state);
        unique->start_thread_worker();
        wait_for_poll(unique_state);
        unique.reset();
        verify(unique_state);

        LifetimeState shared_state;
        std::shared_ptr<GPU> shared = make_shared_polling_gpu<LifetimeGPU>(shared_state);
        shared->request_polling();
        wait_for_poll(shared_state);
        auto copy = shared;
        shared.reset();
        if (shared_state.destroyed)
            throw std::runtime_error("GPU destroyed while another owner remains");
        copy.reset();
        verify(shared_state);
    }
    std::cout << "20 GPU owner lifecycles passed (legacy unique + Next shared)\n";
}
