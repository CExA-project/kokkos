//@HEADER
// ************************************************************************
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
// Part of Kokkos, under the Apache License v2.0 with LLVM Exceptions.
// See https://kokkos.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//@HEADER

#ifndef TESTLOCALDEEPCOPY_HPP
#define TESTLOCALDEEPCOPY_HPP

#include <gtest/gtest.h>

#include <Kokkos_Core.hpp>

namespace Test {

template <typename ViewType>
bool view_check_equals(const ViewType& lhs, const ViewType& rhs) {
  int result = 1;

  auto reducer = Kokkos::LAnd<int>(result);
  Kokkos::parallel_reduce(
      "view check equals", lhs.span(),
      KOKKOS_LAMBDA(int i, int& local_result) {
        local_result = (lhs.data()[i] == rhs.data()[i]) && local_result;
      },
      reducer);
  return (result);
}

template <typename ViewType>
void view_init(ViewType& view) {
  Kokkos::parallel_for(
      "initialize array", view.span(),
      KOKKOS_LAMBDA(int i) { view.data()[i] = i; });
}

// Helper function to create a std::array filled with a given value
template <typename T, size_t Rank>
constexpr auto make_array(T value) -> std::array<T, Rank> {
  std::array<T, Rank> a;
  for (auto& x : a) x = value;
  return a;
}

// Create a view with a given label and dimensions
template <typename ViewType, unsigned Rank = unsigned(ViewType::rank)>
ViewType view_create(std::string label, const int N) {
  const auto dimensions =
      std::tuple_cat(std::make_tuple(label), make_array<int, Rank>(N));
  return std::make_from_tuple<ViewType>(dimensions);
}

// Extract a subview from a view to run our tests
template <typename ViewType, typename Bounds,
          unsigned Rank = unsigned(ViewType::rank)>
KOKKOS_INLINE_FUNCTION auto extract_subview(ViewType& src, int lid,
                                            Bounds bounds) {
  static_assert(Rank > 1, "Rank must be greater than 1");
  const auto dimensions =
      std::tuple_cat(std::make_tuple(src, lid, bounds),
                     make_array<Kokkos::ALL_t, Rank - 2>(Kokkos::ALL));
  return std::apply([](auto&&... xs) { return Kokkos::subview(xs...); },
                    dimensions);
}

// Helper class to run local_deep_copy test
template <typename ViewType, typename ExecSpace>
class TestLocalDeepCopyRank {
  using team_policy = Kokkos::TeamPolicy<ExecSpace>;
  using member_type = typename Kokkos::TeamPolicy<ExecSpace>::member_type;

 public:
  TestLocalDeepCopyRank(const int N) : N(N) {
    A = view_create<ViewType>("A", N);
    B = view_create<ViewType>("B", N);

    // Initialize A matrix.
    view_init(A);
  }

  void run_team_policy() {
    test_local_deepcopy_thread();
    reset_b();
    test_local_deepcopy();
    reset_b();
    test_local_deepcopy_scalar();
  }

  void run_range_policy() {
    test_local_deepcopy_range();
    reset_b();
    test_local_deepcopy_scalar_range();
  }

 private:
  void test_local_deepcopy_thread() {
    // Test local_deep_copy_thread
    // Each thread copies a subview of A into B
    Kokkos::parallel_for(
        team_policy(N, Kokkos::AUTO),
        KOKKOS_LAMBDA(const member_type& teamMember) {
          int lid =
              teamMember.league_rank();  // returns a number between 0 and N

          // Compute the number of units of work per thread
          auto thread_number = teamMember.league_size();
          auto unitsOfWork   = N / thread_number;
          if (N % thread_number) {
            unitsOfWork += 1;
          }
          auto numberOfBatches = N / unitsOfWork;

          Kokkos::parallel_for(
              Kokkos::TeamThreadRange(teamMember, numberOfBatches),
              [=](const int indexWithinBatch) {
                const int idx = indexWithinBatch;

                auto start = idx * unitsOfWork;
                auto stop  = (idx + 1) * unitsOfWork;
                stop       = Kokkos::clamp(stop, 0, N);
                auto subSrc =
                    extract_subview(A, lid, Kokkos::make_pair(start, stop));
                auto subDst =
                    extract_subview(B, lid, Kokkos::make_pair(start, stop));
                Kokkos::Experimental::local_deep_copy_thread(teamMember, subDst,
                                                             subSrc);
                // No wait for local_deep_copy_thread
              });
        });

    ASSERT_TRUE(view_check_equals(A, B));
  }

  void test_local_deepcopy() {
    // Deep Copy
    Kokkos::parallel_for(
        team_policy(N, Kokkos::AUTO),
        KOKKOS_LAMBDA(const member_type& teamMember) {
          int lid =
              teamMember.league_rank();  // returns a number between 0 and N
          auto subSrc = extract_subview(A, lid, Kokkos::ALL);
          auto subDst = extract_subview(B, lid, Kokkos::ALL);
          Kokkos::Experimental::local_deep_copy(teamMember, subDst, subSrc);
        });

    ASSERT_TRUE(view_check_equals(A, B));
  }

  void test_local_deepcopy_range() {
    // Deep Copy
    Kokkos::parallel_for(
        Kokkos::RangePolicy<ExecSpace>(0, N), KOKKOS_LAMBDA(const int& lid) {
          auto subSrc = extract_subview(A, lid, Kokkos::ALL);
          auto subDst = extract_subview(B, lid, Kokkos::ALL);
          Kokkos::Experimental::local_deep_copy(subDst, subSrc);
        });

    ASSERT_TRUE(view_check_equals(A, B));
  }

  void test_local_deepcopy_scalar() {
    Kokkos::parallel_for(
        team_policy(N, Kokkos::AUTO),
        KOKKOS_LAMBDA(const member_type& teamMember) {
          int lid =
              teamMember.league_rank();  // returns a number between 0 and N
          auto subDst = extract_subview(B, lid, Kokkos::ALL);
          Kokkos::Experimental::local_deep_copy(teamMember, subDst, fill_value);
        });

    ASSERT_TRUE(check_sum());
  }

  void test_local_deepcopy_scalar_range() {
    Kokkos::parallel_for(
        Kokkos::RangePolicy<ExecSpace>(0, N), KOKKOS_LAMBDA(const int& lid) {
          auto subDst = extract_subview(B, lid, Kokkos::ALL);
          Kokkos::Experimental::local_deep_copy(subDst, fill_value);
        });

    double sum_all = 0.0;
    Kokkos::parallel_reduce(
        "Check B", B.span(),
        KOKKOS_LAMBDA(int i, double& lsum) { lsum += B.data()[i]; },
        Kokkos::Sum<double>(sum_all));

    ASSERT_TRUE(check_sum());
  }

  void reset_b() { Kokkos::deep_copy(B, 0); }

  bool check_sum() {
    double sum_all = 0;
    Kokkos::parallel_reduce(
        "Check B", B.span(),
        KOKKOS_LAMBDA(int i, double& lsum) { lsum += B.data()[i]; },
        Kokkos::Sum<double>(sum_all));

    auto correct_sum = fill_value;
    for (auto i = 0; i < ViewType::rank; i++) {
      correct_sum *= N;
    }

    return sum_all == correct_sum;
  }

  int N;
  ViewType A;
  ViewType B;
  static constexpr typename ViewType::value_type fill_value = 20;
};

//-------------------------------------------------------------------------------------------------------------

#if defined(KOKKOS_ENABLE_CXX11_DISPATCH_LAMBDA)
TEST(TEST_CATEGORY, local_deepcopy_teampolicy_layoutleft) {
  using ExecSpace = TEST_EXECSPACE;
#if defined(KOKKOS_ENABLE_CUDA) && \
    defined(KOKKOS_COMPILER_NVHPC)  // FIXME_NVHPC 23.7
  if (std::is_same_v<ExecSpace, Kokkos::Cuda>)
    GTEST_SKIP()
        << "FIXME_NVHPC : Compiler bug affecting subviews of high rank Views";
#endif
  using Layout = Kokkos::LayoutLeft;

  {  // Rank-1
    auto test = TestLocalDeepCopyRank<Kokkos::View<double**, Layout, ExecSpace>,
                                      ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-2
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double***, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-3
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double****, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-4
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double*****, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-5
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double******, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-6
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double*******, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-7
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double********, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
}
//-------------------------------------------------------------------------------------------------------------
TEST(TEST_CATEGORY, local_deepcopy_rangepolicy_layoutleft) {
  using ExecSpace = TEST_EXECSPACE;
#if defined(KOKKOS_ENABLE_CUDA) && \
    defined(KOKKOS_COMPILER_NVHPC)  // FIXME_NVHPC 23.7
  if (std::is_same_v<ExecSpace, Kokkos::Cuda>)
    GTEST_SKIP()
        << "FIXME_NVHPC : Compiler bug affecting subviews of high rank Views";
#endif
  using Layout = Kokkos::LayoutLeft;

  {  // Rank-1
    auto test = TestLocalDeepCopyRank<Kokkos::View<double**, Layout, ExecSpace>,
                                      ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-2
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double***, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-3
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double****, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-4
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double*****, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-5
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double******, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-6
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double*******, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-7
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double********, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
}
//-------------------------------------------------------------------------------------------------------------
TEST(TEST_CATEGORY, local_deepcopy_teampolicy_layoutright) {
  using ExecSpace = TEST_EXECSPACE;
#if defined(KOKKOS_ENABLE_CUDA) && \
    defined(KOKKOS_COMPILER_NVHPC)  // FIXME_NVHPC 23.7
  if (std::is_same_v<ExecSpace, Kokkos::Cuda>)
    GTEST_SKIP()
        << "FIXME_NVHPC : Compiler bug affecting subviews of high rank Views";
#endif
  using Layout = Kokkos::LayoutRight;

  {  // Rank-1
    auto test = TestLocalDeepCopyRank<Kokkos::View<double**, Layout, ExecSpace>,
                                      ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-2
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double***, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-3
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double****, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-4
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double*****, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-5
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double******, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-6
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double*******, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
  {  // Rank-7
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double********, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_team_policy();
  }
}
//-------------------------------------------------------------------------------------------------------------
TEST(TEST_CATEGORY, local_deepcopy_rangepolicy_layoutright) {
  using ExecSpace = TEST_EXECSPACE;
#if defined(KOKKOS_ENABLE_CUDA) && \
    defined(KOKKOS_COMPILER_NVHPC)  // FIXME_NVHPC 23.7
  if (std::is_same_v<ExecSpace, Kokkos::Cuda>)
    GTEST_SKIP()
        << "FIXME_NVHPC : Compiler bug affecting subviews of high rank Views";
#endif
  using Layout = Kokkos::LayoutRight;

  {  // Rank-1
    auto test = TestLocalDeepCopyRank<Kokkos::View<double**, Layout, ExecSpace>,
                                      ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-2
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double***, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-3
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double****, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-4
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double*****, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-5
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double******, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-6
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double*******, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
  {  // Rank-7
    auto test =
        TestLocalDeepCopyRank<Kokkos::View<double********, Layout, ExecSpace>,
                              ExecSpace>(8);
    test.run_range_policy();
  }
}
#endif

namespace Impl {
template <typename T, typename SHMEMTYPE>
using ShMemView =
    Kokkos::View<T, Kokkos::LayoutRight, SHMEMTYPE, Kokkos::MemoryUnmanaged>;

struct DeepCopyScratchFunctor {
  DeepCopyScratchFunctor(
      Kokkos::View<double*, TEST_EXECSPACE::memory_space> check_view_1,
      Kokkos::View<double*, TEST_EXECSPACE::memory_space> check_view_2)
      : check_view_1_(check_view_1),
        check_view_2_(check_view_2),
        N_(check_view_1.extent(0)) {}

  KOKKOS_INLINE_FUNCTION void operator()(
      Kokkos::TeamPolicy<TEST_EXECSPACE,
                         Kokkos::Schedule<Kokkos::Dynamic>>::member_type team)
      const {
    using ShmemType = TEST_EXECSPACE::scratch_memory_space;
    auto shview =
        Impl::ShMemView<double**, ShmemType>(team.team_scratch(1), N_, 1);

    Kokkos::parallel_for(
        Kokkos::TeamThreadRange(team, N_), KOKKOS_LAMBDA(const size_t& index) {
          auto thread_shview = Kokkos::subview(shview, index, Kokkos::ALL());
          Kokkos::Experimental::local_deep_copy(thread_shview, index);
        });
    Kokkos::Experimental::local_deep_copy(
        team, check_view_1_, Kokkos::subview(shview, Kokkos::ALL(), 0));

    Kokkos::Experimental::local_deep_copy(team, shview, 6.);
    Kokkos::Experimental::local_deep_copy(
        team, check_view_2_, Kokkos::subview(shview, Kokkos::ALL(), 0));
  }

  Kokkos::View<double*, TEST_EXECSPACE::memory_space> check_view_1_;
  Kokkos::View<double*, TEST_EXECSPACE::memory_space> check_view_2_;
  int const N_;
};
}  // namespace Impl

TEST(TEST_CATEGORY, deep_copy_scratch) {
  using TestDeviceTeamPolicy = Kokkos::TeamPolicy<TEST_EXECSPACE>;

  const int N = 8;
  const int bytes_per_team =
      Impl::ShMemView<double**,
                      TEST_EXECSPACE::scratch_memory_space>::shmem_size(N, 1);

  TestDeviceTeamPolicy policy(1, Kokkos::AUTO);
  auto team_exec = policy.set_scratch_size(1, Kokkos::PerTeam(bytes_per_team));

  Kokkos::View<double*, TEST_EXECSPACE::memory_space> check_view_1("check_1",
                                                                   N);
  Kokkos::View<double*, TEST_EXECSPACE::memory_space> check_view_2("check_2",
                                                                   N);

  Kokkos::parallel_for(
      team_exec, Impl::DeepCopyScratchFunctor{check_view_1, check_view_2});
  auto host_copy_1 =
      Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), check_view_1);
  auto host_copy_2 =
      Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), check_view_2);

  for (unsigned int i = 0; i < N; ++i) {
    ASSERT_EQ(host_copy_1(i), i);
    ASSERT_EQ(host_copy_2(i), 6.0);
  }
}
}  // namespace Test

#endif  // TESTLOCALDEEPCOPY_HPP
