#include <chrono>

#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "ex_actor/internal/scheduler/all.h"  // IWYU pragma: keep

struct Obj {
  int c;
  unsigned int d;
};

struct TestOffset {
  int a;
  char b;
  Obj obj;
  unsigned int e;
};

struct TestActor {
  void Run() {
    if (running) {
      throw std::runtime_error("Running in multiple threads");
    }
    running = true;
    std::this_thread::sleep_for(std::chrono::microseconds(1));
    running = false;
  }

  bool running = false;
};

TEST(StressTest, ActorShouldOnlyBeExecutedInOneThread) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());
  auto [actor] = stdexec::sync_wait(registry.Spawn<TestActor>()).value();
  std::vector<std::jthread> threads;
  threads.reserve(10);
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([actor]() {
      for (int j = 0; j < 10; ++j) {
        ex_actor::ex::sync_wait(actor.Send<&TestActor::Run>());
      }
    });
  }
}

#include <cstddef>

constexpr size_t off_a = offsetof(TestOffset, a);      // 0
constexpr size_t off_b = offsetof(TestOffset, b);      // 4
constexpr size_t off_c = offsetof(TestOffset, obj.c);  // 8
constexpr size_t off_d = offsetof(TestOffset, obj.d);  // 12
constexpr size_t off_e = offsetof(TestOffset, e);      // 16

// 如果你只需要在编译期使用类型
template <typename T, size_t Offset>
struct TypeOffset {
  using Type = T;
  static constexpr size_t kOffset = Offset;
};

// 使用
using A_Info = TypeOffset<int, off_a>;
using B_Info = TypeOffset<char, off_b>;
using C_Info = TypeOffset<int, off_c>;
using D_Info = TypeOffset<unsigned int, off_d>;
using E_Info = TypeOffset<unsigned int, off_e>;

template <typename T>
using InfoType_t = typename T::Type;

template <typename T>
constexpr size_t InfoOffset_v = T::kOffset;


TEST(StressTest, ActorShouldWorkWithOffset) {

  TestOffset offset;
  offset.a = 1;
  offset.b = 'a';
  offset.obj.c = 2;
  offset.obj.d = 3;
  offset.e = 4;

  char buf[sizeof(TestOffset)];
  memcpy(buf, &offset, sizeof(TestOffset));

  // 从buf中读取c
  InfoType_t<C_Info> c = *reinterpret_cast<InfoType_t<C_Info>*>(buf + InfoOffset_v<C_Info>);
  EXPECT_EQ(c, 2);

  // 从buf中读取d
  InfoType_t<D_Info> d = *reinterpret_cast<InfoType_t<D_Info>*>(buf + InfoOffset_v<D_Info>);
  EXPECT_EQ(d, 3);

  // 从buf中读取a
  InfoType_t<A_Info> a = *reinterpret_cast<InfoType_t<A_Info>*>(buf + InfoOffset_v<A_Info>);
  EXPECT_EQ(a, 1);

  // 从buf中读取e
  InfoType_t<E_Info> e = *reinterpret_cast<InfoType_t<E_Info>*>(buf + InfoOffset_v<E_Info>);
  EXPECT_EQ(e, 4);

}