#include <Runtime/Runtime.h>
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <fc/log/logger.hpp>
#include <iostream>
#include <sstream>

#include "eosio.system_tester.hpp"

inline constexpr int64_t rentbw_frac  = 1000000000000000ll; // 1.0 = 10^15
inline constexpr int64_t stake_weight = 1000000000000ll;    // 10^12

struct rentbw_config_resource {
   int64_t        current_weight_ratio = {};
   int64_t        target_weight_ratio  = {};
   int64_t        assumed_stake_weight = {};
   time_point_sec target_timestamp     = {};
   double         exponent             = {};
   uint32_t       decay_secs           = {};
   asset          target_price         = asset{};
};
FC_REFLECT(rentbw_config_resource,                                                             //
           (current_weight_ratio)(target_weight_ratio)(assumed_stake_weight)(target_timestamp) //
           (exponent)(decay_secs)(target_price))

struct rentbw_config {
   rentbw_config_resource net            = {};
   rentbw_config_resource cpu            = {};
   uint32_t               rent_days      = {};
   asset                  min_rent_price = asset{};
};
FC_REFLECT(rentbw_config, (net)(cpu)(rent_days)(min_rent_price))

struct rentbw_state_resource {
   uint8_t        version;
   int64_t        weight;
   int64_t        weight_ratio;
   int64_t        assumed_stake_weight;
   int64_t        initial_weight_ratio;
   int64_t        target_weight_ratio;
   time_point_sec initial_timestamp;
   time_point_sec target_timestamp;
   double         exponent;
   uint32_t       decay_secs;
   asset          target_price;
   int64_t        utilization;
   int64_t        adjusted_utilization;
   time_point_sec utilization_timestamp;
};
FC_REFLECT(rentbw_state_resource,                                                                           //
           (version)(weight)(weight_ratio)(assumed_stake_weight)(initial_weight_ratio)(target_weight_ratio) //
           (initial_timestamp)(target_timestamp)(exponent)(decay_secs)(target_price)(utilization)           //
           (adjusted_utilization)(utilization_timestamp))

struct rentbw_state {
   uint8_t               version;
   rentbw_state_resource net;
   rentbw_state_resource cpu;
   uint32_t              rent_days;
   asset                 min_rent_price;
};
FC_REFLECT(rentbw_state, (version)(net)(cpu)(rent_days)(min_rent_price))

using namespace eosio_system;

struct rentbw_tester : eosio_system_tester {

   rentbw_tester() { create_accounts_with_resources({ N(eosio.reserv) }); }

   template <typename F>
   rentbw_config make_config(F f) {
      rentbw_config config;

      config.net.current_weight_ratio = rentbw_frac;
      config.net.target_weight_ratio  = rentbw_frac / 100;
      config.net.assumed_stake_weight = stake_weight;
      config.net.target_timestamp     = control->head_block_time() + fc::days(100);
      config.net.exponent             = 2;
      config.net.decay_secs           = fc::days(1).to_seconds();
      config.net.target_price         = asset::from_string("1000000.0000 TST");

      config.cpu.current_weight_ratio = rentbw_frac;
      config.cpu.target_weight_ratio  = rentbw_frac / 100;
      config.cpu.assumed_stake_weight = stake_weight;
      config.cpu.target_timestamp     = control->head_block_time() + fc::days(100);
      config.cpu.exponent             = 2;
      config.cpu.decay_secs           = fc::days(1).to_seconds();
      config.cpu.target_price         = asset::from_string("1000000.0000 TST");

      config.rent_days      = 30;
      config.min_rent_price = asset::from_string("1.0000 TST");

      f(config);
      return config;
   }

   rentbw_config make_config() {
      return make_config([](auto&) {});
   }

   template <typename F>
   rentbw_config make_default_config(F f) {
      rentbw_config config;
      f(config);
      return config;
   }

   action_result configbw(const rentbw_config& config) {
      return push_action(config::system_account_name, N(configrentbw), mvo()("args", config));
   }

   action_result rentbwexec(name user, uint16_t max) {
      return push_action(user, N(rentbwexec), mvo()("user", user)("max", max));
   }

   rentbw_state get_state() {
      vector<char> data = get_row_by_account(config::system_account_name, {}, N(rent.state), N(rent.state));
      return fc::raw::unpack<rentbw_state>(data);
   }
};

template <typename A, typename B, typename D>
bool near(A a, B b, D delta) {
   if (abs(a - b) <= delta)
      return true;
   elog("near: ${a} ${b}", ("a", a)("b", b));
   return false;
}

BOOST_AUTO_TEST_SUITE(eosio_system_rentbw_tests)

BOOST_FIXTURE_TEST_CASE(config_tests, rentbw_tester) try {
   BOOST_REQUIRE_EQUAL("missing authority of eosio",
                       push_action(N(alice1111111), N(configrentbw), mvo()("args", make_config())));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("rentbw hasn't been initialized"), rentbwexec(N(alice1111111), 10));

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("rent_days must be > 0"),
                       configbw(make_config([&](auto& c) { c.rent_days = 0; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("min_rent_price doesn't match core symbol"), configbw(make_config([&](auto& c) {
                          c.min_rent_price = asset::from_string("1000000.000 TST");
                       })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("min_rent_price must be positive"),
                       configbw(make_config([&](auto& c) { c.min_rent_price = asset::from_string("0.0000 TST"); })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("min_rent_price must be positive"),
                       configbw(make_config([&](auto& c) { c.min_rent_price = asset::from_string("-1.0000 TST"); })));

   // net assertions
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("current_weight_ratio is too large"),
                       configbw(make_config([](auto& c) { c.net.current_weight_ratio = rentbw_frac + 1; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("weight can't grow over time"),
                       configbw(make_config([](auto& c) { c.net.target_weight_ratio = rentbw_frac + 1; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("assumed_stake_weight must be at least 1; a much larger value is recommended"),
                       configbw(make_config([](auto& c) { c.net.assumed_stake_weight = 0; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("target_timestamp must be in the future"),
                       configbw(make_config([&](auto& c) { c.net.target_timestamp = control->head_block_time(); })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("target_timestamp must be in the future"), configbw(make_config([&](auto& c) {
                          c.net.target_timestamp = control->head_block_time() - fc::seconds(1);
                       })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("exponent must be >= 1"),
                       configbw(make_config([&](auto& c) { c.net.exponent = .999; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("decay_secs must be >= 1"),
                       configbw(make_config([&](auto& c) { c.net.decay_secs = 0; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("target_price doesn't match core symbol"), configbw(make_config([&](auto& c) {
                          c.net.target_price = asset::from_string("1000000.000 TST");
                       })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("target_price must be positive"),
                       configbw(make_config([&](auto& c) { c.net.target_price = asset::from_string("0.0000 TST"); })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("target_price must be positive"),
                       configbw(make_config([&](auto& c) { c.net.target_price = asset::from_string("-1.0000 TST"); })));

   // cpu assertions
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("current_weight_ratio is too large"),
                       configbw(make_config([](auto& c) { c.cpu.current_weight_ratio = rentbw_frac + 1; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("weight can't grow over time"),
                       configbw(make_config([](auto& c) { c.cpu.target_weight_ratio = rentbw_frac + 1; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("assumed_stake_weight must be at least 1; a much larger value is recommended"),
                       configbw(make_config([](auto& c) { c.cpu.assumed_stake_weight = 0; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("target_timestamp must be in the future"),
                       configbw(make_config([&](auto& c) { c.cpu.target_timestamp = control->head_block_time(); })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("target_timestamp must be in the future"), configbw(make_config([&](auto& c) {
                          c.cpu.target_timestamp = control->head_block_time() - fc::seconds(1);
                       })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("exponent must be >= 1"),
                       configbw(make_config([&](auto& c) { c.cpu.exponent = .999; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("decay_secs must be >= 1"),
                       configbw(make_config([&](auto& c) { c.cpu.decay_secs = 0; })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("target_price doesn't match core symbol"), configbw(make_config([&](auto& c) {
                          c.cpu.target_price = asset::from_string("1000000.000 TST");
                       })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("target_price must be positive"),
                       configbw(make_config([&](auto& c) { c.cpu.target_price = asset::from_string("0.0000 TST"); })));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("target_price must be positive"),
                       configbw(make_config([&](auto& c) { c.cpu.target_price = asset::from_string("-1.0000 TST"); })));

   // TODO: "weight can't shrink below utilization"
} // config_tests
FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(weight_tests, rentbw_tester) try {
   produce_block();

   auto net_start  = (rentbw_frac * 11) / 100;
   auto net_target = (rentbw_frac * 1) / 100;
   auto cpu_start  = (rentbw_frac * 11) / 1000;
   auto cpu_target = (rentbw_frac * 1) / 1000;

   BOOST_REQUIRE_EQUAL("", configbw(make_config([&](rentbw_config& config) {
                          config.net.current_weight_ratio = net_start;
                          config.net.target_weight_ratio  = net_target;
                          config.net.assumed_stake_weight = stake_weight;
                          config.net.target_timestamp     = control->head_block_time() + fc::days(10);

                          config.cpu.current_weight_ratio = cpu_start;
                          config.cpu.target_weight_ratio  = cpu_target;
                          config.cpu.assumed_stake_weight = stake_weight;
                          config.cpu.target_timestamp     = control->head_block_time() + fc::days(20);
                       })));

   int64_t net;
   int64_t cpu;

   for (int i = 0; i <= 6; ++i) {
      if (i == 2) {
         // Leaves everything as-is, but may introduce slight rounding
         produce_block(fc::days(1) - fc::milliseconds(500));
         BOOST_REQUIRE_EQUAL("", configbw({}));
      } else if (i) {
         produce_block(fc::days(1) - fc::milliseconds(500));
         BOOST_REQUIRE_EQUAL("", rentbwexec(config::system_account_name, 10));
      }
      net = net_start + i * (net_target - net_start) / 10;
      cpu = cpu_start + i * (cpu_target - cpu_start) / 20;
      BOOST_REQUIRE(near(get_state().net.weight_ratio, net, 1));
      BOOST_REQUIRE(near(get_state().cpu.weight_ratio, cpu, 1));
   }

   // Extend transition time
   {
      int i = 7;
      produce_block(fc::days(1) - fc::milliseconds(500));
      BOOST_REQUIRE_EQUAL("", configbw(make_default_config([&](rentbw_config& config) {
                             config.net.target_timestamp = control->head_block_time() + fc::days(30);
                             config.cpu.target_timestamp = control->head_block_time() + fc::days(40);
                          })));
      net_start = net = net_start + i * (net_target - net_start) / 10;
      cpu_start = cpu = cpu_start + i * (cpu_target - cpu_start) / 20;
      BOOST_REQUIRE(near(get_state().net.weight_ratio, net, 1));
      BOOST_REQUIRE(near(get_state().cpu.weight_ratio, cpu, 1));
   }

   for (int i = 0; i <= 5; ++i) {
      if (i) {
         produce_block(fc::days(1) - fc::milliseconds(500));
         BOOST_REQUIRE_EQUAL("", rentbwexec(config::system_account_name, 10));
      }
      net = net_start + i * (net_target - net_start) / 30;
      cpu = cpu_start + i * (cpu_target - cpu_start) / 40;
      BOOST_REQUIRE(near(get_state().net.weight_ratio, net, 1));
      BOOST_REQUIRE(near(get_state().cpu.weight_ratio, cpu, 1));
   }

   // Change target, keep existing transition time
   {
      int i = 6;
      produce_block(fc::days(1) - fc::milliseconds(500));
      auto new_net_target = net_target / 10;
      auto new_cpu_target = cpu_target / 20;
      BOOST_REQUIRE_EQUAL("", configbw(make_default_config([&](rentbw_config& config) {
                             config.net.target_weight_ratio = new_net_target;
                             config.cpu.target_weight_ratio = new_cpu_target;
                          })));
      net_start = net = net_start + i * (net_target - net_start) / 30;
      cpu_start = cpu = cpu_start + i * (cpu_target - cpu_start) / 40;
      net_target      = new_net_target;
      cpu_target      = new_cpu_target;
      BOOST_REQUIRE(near(get_state().net.weight_ratio, net, 1));
      BOOST_REQUIRE(near(get_state().cpu.weight_ratio, cpu, 1));
   }

   for (int i = 0; i <= 10; ++i) {
      if (i) {
         produce_block(fc::days(1) - fc::milliseconds(500));
         BOOST_REQUIRE_EQUAL("", rentbwexec(config::system_account_name, 10));
      }
      net = net_start + i * (net_target - net_start) / (30 - 6);
      cpu = cpu_start + i * (cpu_target - cpu_start) / (40 - 6);
      BOOST_REQUIRE(near(get_state().net.weight_ratio, net, 1));
      BOOST_REQUIRE(near(get_state().cpu.weight_ratio, cpu, 1));
   }

   // Move transition time to immediate future
   {
      produce_block(fc::days(1) - fc::milliseconds(500));
      BOOST_REQUIRE_EQUAL("", configbw(make_default_config([&](rentbw_config& config) {
                             config.net.target_timestamp = control->head_block_time() + fc::milliseconds(1000);
                             config.cpu.target_timestamp = control->head_block_time() + fc::milliseconds(1000);
                          })));
      produce_blocks(2);
   }

   // Verify targets hold as time advances
   for (int i = 0; i <= 10; ++i) {
      BOOST_REQUIRE_EQUAL("", rentbwexec(config::system_account_name, 10));
      BOOST_REQUIRE(near(get_state().net.weight_ratio, net_target, 1));
      BOOST_REQUIRE(near(get_state().cpu.weight_ratio, cpu_target, 1));
      produce_block(fc::days(1));
   }

   // todo: verify calculated weight

} // weight_tests
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()