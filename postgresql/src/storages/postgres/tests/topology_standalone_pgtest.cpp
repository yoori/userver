#include <gtest/gtest.h>

#include <storages/postgres/detail/connection.hpp>
#include <storages/postgres/detail/topology/standalone.hpp>
#include <userver/error_injection/settings.hpp>
#include <userver/storages/postgres/exceptions.hpp>

#include <storages/postgres/tests/util_pgtest.hpp>

namespace pg = storages::postgres;

class Standalone : public PostgreSQLBase,
                   public ::testing::WithParamInterface<pg::DsnList> {};

UTEST_P(Standalone, Smoke) {
  const auto& dsns = GetParam();
  if (dsns.size() != 1) return;

  pg::detail::topology::Standalone sa(
      GetTaskProcessor(), dsns, pg::TopologySettings{kMaxTestWaitTime},
      pg::ConnectionSettings{}, GetTestCmdCtls(), testsuite::PostgresControl{},
      error_injection::Settings{});

  auto hosts = sa.GetDsnIndicesByType();
  EXPECT_EQ(1, hosts->count(pg::ClusterHostType::kMaster));
  EXPECT_EQ(0, hosts->count(pg::ClusterHostType::kSlave));

  auto alive = sa.GetAliveDsnIndices();
  EXPECT_EQ(1, alive->size());
}

INSTANTIATE_UTEST_SUITE_P(PostgreTopology, Standalone,
                          ::testing::ValuesIn(GetDsnListsFromEnv()),
                          DsnListToString);