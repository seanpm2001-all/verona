// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <random>
#include <test/harness.h>
#include <test/opt.h>
#include <verona.h>

using namespace snmalloc;
using namespace verona::rt;

struct A : public VCown<A>
{};

void test_runtime_pause(SystematicTestHarness* harness, size_t pauses)
{
  schedule_lambda([harness, pauses]() {
    auto a = new A;
    Scheduler::add_external_event_source();
    auto pauses_ = pauses;
    harness->external_thread([pauses_, a]() mutable {
      Systematic::cout() << "Started external thread" << Systematic::endl;
      std::mt19937 rng;
      rng.seed(1);
      std::uniform_int_distribution<> dist(1, 1000);
      for (size_t i = 1; i <= pauses_; i++)
      {
        auto pause_time = std::chrono::milliseconds(dist(rng));
        std::this_thread::sleep_for(pause_time);
        Systematic::cout() << "Scheduling Message" << Systematic::endl;
        schedule_lambda(a, [i]() {
          Systematic::cout() << "running message " << i << std::endl;
        });
      }
      schedule_lambda(a, [a]() { Cown::release(ThreadAlloc::get(), a); });

      schedule_lambda([]() {
        Systematic::cout() << "Remove external event source" << std::endl;
        Scheduler::remove_external_event_source();
      });

      Systematic::cout() << "External thread exiting" << Systematic::endl;
    });
  });
}

int main(int argc, char** argv)
{
  SystematicTestHarness harness(argc, argv);

  size_t pauses = harness.opt.is<size_t>("--pauses", 3);

  harness.run(test_runtime_pause, &harness, pauses);
  return 0;
}
