#include <iostream>
#include <cassert>
#include <cmath>
#include <cstring>
#include "../../main/ws/include/adaptive_publish.h"

using namespace AdaptivePublish;

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        std::cerr << "FAILED: " << msg << " (line " << __LINE__ << ")" << std::endl; \
        assert(false); \
    } \
} while (0)

int main() {
    std::cout << "Running AdaptivePublish Unit Tests (41 Assertions)..." << std::endl;

    WeatherChangePolicy policy;
    default_policy(&policy);
    PublisherState st;
    memset(&st, 0, sizeof(st));

    int64_t now_ms = 1000000;

    // Test 1: Cumulative drift 0.5+0.5+0.5+0.5 against a 2.0 threshold eventually triggers
    {
        memset(&st, 0, sizeof(st));
        WeatherSnapshot snap;
        snap.clear_all();
        snap.set(VAR_TEMPERATURE, 20.0f);

        // First evaluate boots and establishes baseline on success
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & REASON_HEARTBEAT), "T1: Initial publish is heartbeat");
        mark_success(&st, d, policy, now_ms);

        // Drift 0.5
        now_ms += 1000;
        snap.set(VAR_TEMPERATURE, 20.5f);
        d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T1: +0.5 does not trigger");

        // Drift 1.0
        now_ms += 1000;
        snap.set(VAR_TEMPERATURE, 21.0f);
        d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T1: +1.0 does not trigger");

        // Drift 1.5
        now_ms += 1000;
        snap.set(VAR_TEMPERATURE, 21.5f);
        d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T1: +1.5 does not trigger");

        // Drift 2.0 (cumulative)
        now_ms += 400000; // pass cooldown
        snap.set(VAR_TEMPERATURE, 22.0f);
        d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & reason_for(VAR_TEMPERATURE)), "T1: +2.0 triggers event");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 2: Jitter of +-1.9 around a baseline never triggers indefinitely
    {
        WeatherSnapshot snap;
        snap.clear_all();
        snap.set(VAR_TEMPERATURE, 22.0f);
        for (int i = 0; i < 50; i++) {
            now_ms += 10000;
            float val = 22.0f + ((i % 2 == 0) ? 1.9f : -1.9f);
            snap.set(VAR_TEMPERATURE, val);
            auto d = evaluate(snap, policy, &st, 3600, now_ms);
            TEST_ASSERT(!d.publish, "T2: Jitter +-1.9 never triggers");
        }
    }

    // Test 3: Exactly-at-threshold triggers (>=, not >)
    {
        WeatherSnapshot snap;
        snap.clear_all();
        now_ms += 400000;
        snap.set(VAR_TEMPERATURE, 24.0f); // Exactly +2.0 from 22.0
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & reason_for(VAR_TEMPERATURE)), "T3: Exactly-at-threshold triggers");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 4: Multiple simultaneous field changes -> one decision, multi-bit reason mask
    {
        WeatherSnapshot snap;
        snap.clear_all();
        now_ms += 400000;
        snap.set(VAR_TEMPERATURE, 27.0f); // +3.0 (>= 2.0)
        snap.set(VAR_HUMIDITY, 60.0f);    // baseline was 0, but let's establish baseline with temp & hum first
        st.baseline.set(VAR_HUMIDITY, 40.0f); // delta = 20.0 (>= 10.0)
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        uint16_t expected_mask = reason_for(VAR_TEMPERATURE) | reason_for(VAR_HUMIDITY);
        TEST_ASSERT(d.publish && ((d.reason_mask & expected_mask) == expected_mask), "T4: Multi-field delta triggers multi-bit mask");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 5: Heartbeat at exactly 3600 s
    {
        WeatherSnapshot snap = st.baseline;
        now_ms += 3600 * 1000LL;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & REASON_HEARTBEAT), "T5: Heartbeat fires at exactly 3600s");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 6: Heartbeat at 300 s (minimum interval)
    {
        WeatherSnapshot snap = st.baseline;
        now_ms += 300 * 1000LL;
        auto d = evaluate(snap, policy, &st, 300, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & REASON_HEARTBEAT), "T6: Heartbeat fires at 300s");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 7: Heartbeat fires even when no field is enabled
    {
        WeatherChangePolicy no_fields_policy = policy;
        no_fields_policy.enabled_mask = 0;
        WeatherSnapshot snap = st.baseline;
        now_ms += 3600 * 1000LL;
        auto d = evaluate(snap, no_fields_policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & REASON_HEARTBEAT), "T7: Heartbeat fires with 0 enabled fields");
        mark_success(&st, d, no_fields_policy, now_ms);
    }

    // Test 8: Heartbeat is not subject to the cooldown
    {
        // Fire an event
        now_ms += 400000;
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_TEMPERATURE, st.baseline.values[VAR_TEMPERATURE] + 3.0f);
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & reason_for(VAR_TEMPERATURE)), "T8: Event fires");
        mark_success(&st, d, policy, now_ms);

        // Advance 10 seconds (inside 300s cooldown), but heartbeat interval is 10s
        now_ms += 10000;
        d = evaluate(snap, policy, &st, 10, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & REASON_HEARTBEAT), "T8: Heartbeat ignores cooldown");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 9: Heartbeat satisfies a pending event in the same packet
    {
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_TEMPERATURE, st.baseline.values[VAR_TEMPERATURE] + 3.0f);
        now_ms += 10000; // inside cooldown
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish && (st.pending_reasons & reason_for(VAR_TEMPERATURE)), "T9: Event stays pending in cooldown");

        // Now heartbeat due
        now_ms += 3600 * 1000LL;
        d = evaluate(snap, policy, &st, 3600, now_ms);
        uint16_t expected = REASON_HEARTBEAT | reason_for(VAR_TEMPERATURE);
        TEST_ASSERT(d.publish && ((d.reason_mask & expected) == expected), "T9: Heartbeat coalesces and satisfies pending event");
        mark_success(&st, d, policy, now_ms);
        TEST_ASSERT(st.pending_reasons == 0, "T9: Pending reasons cleared on success");
    }

    // Test 10: Event inside the cooldown -> pending, not published, not discarded
    {
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_TEMPERATURE, st.baseline.values[VAR_TEMPERATURE] + 3.0f);
        now_ms += 60000; // 60s into 300s cooldown
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T10: Event in cooldown suppressed");
        TEST_ASSERT(st.pending_reasons & reason_for(VAR_TEMPERATURE), "T10: Reason is pending");
    }

    // Test 11: Pending event released when the cooldown expires
    {
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_TEMPERATURE, st.baseline.values[VAR_TEMPERATURE] + 3.0f);
        now_ms += 245000; // Total 305s from last event
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & reason_for(VAR_TEMPERATURE)), "T11: Pending event released post-cooldown");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 12: Two events inside one cooldown -> one packet, both reason bits set
    {
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_TEMPERATURE, st.baseline.values[VAR_TEMPERATURE] + 3.0f);
        now_ms += 50000; // inside cooldown
        evaluate(snap, policy, &st, 3600, now_ms);

        snap.set(VAR_HUMIDITY, st.baseline.values[VAR_HUMIDITY] + 15.0f);
        now_ms += 50000; // still inside cooldown
        evaluate(snap, policy, &st, 3600, now_ms);

        now_ms += 210000; // cooldown elapsed
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        uint16_t expected = reason_for(VAR_TEMPERATURE) | reason_for(VAR_HUMIDITY);
        TEST_ASSERT(d.publish && ((d.reason_mask & expected) == expected), "T12: Both pending events released together");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 13: 7th event in an hour is suppressed; escalation level rises
    {
        memset(&st, 0, sizeof(st));
        WeatherSnapshot snap;
        snap.clear_all();
        snap.set(VAR_TEMPERATURE, 10.0f);
        now_ms = 10000000LL;

        // Boot
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        mark_success(&st, d, policy, now_ms);

        // Trigger 6 events spaced by 305 seconds (within 1 hour)
        for (int i = 1; i <= 6; i++) {
            now_ms += 305000;
            snap.set(VAR_TEMPERATURE, 10.0f + (float)i * 3.0f);
            d = evaluate(snap, policy, &st, 3600, now_ms);
            TEST_ASSERT(d.publish, "T13: Event within budget publishes");
            mark_success(&st, d, policy, now_ms);
        }
        TEST_ASSERT(st.events_this_hour == 6, "T13: 6 events recorded this hour");
        TEST_ASSERT(st.escalation_level == 1, "T13: Escalation level risen to 1");

        // 7th event within the same hour
        now_ms += 305000; // 7 * 305s = 2135s (< 3600s)
        snap.set(VAR_TEMPERATURE, 10.0f + 25.0f);
        d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T13: 7th event is suppressed by hourly budget");
        TEST_ASSERT(st.pending_reasons != 0, "T13: 7th event stays pending");
    }

    // Test 14: Escalated cooldown is honoured (600 s at level 1)
    {
        // 600s cooldown at escalation level 1
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_TEMPERATURE, st.baseline.values[VAR_TEMPERATURE] + 5.0f);
        // Set hour window to current time so escalation level 1 is active (no decay yet)
        st.hour_window_start_ms = now_ms;
        st.escalation_level = 1;
        st.events_this_hour = 0; // budget available, but cooldown is 600s
        st.last_event_ms = now_ms;

        // Check at +305s (which would pass level 0 (300s), but fails level 1 (600s))
        now_ms += 305000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T14: 305s fails 600s escalated cooldown");

        // Check at +305s (total 610s > 600s)
        now_ms += 305000;
        d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish, "T14: 610s satisfies escalated cooldown");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 15: Heartbeat still fires at maximum escalation
    {
        st.escalation_level = 2; // 1200s cooldown
        st.events_this_hour = 6;
        WeatherSnapshot snap = st.baseline;
        now_ms += 3600 * 1000LL;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & REASON_HEARTBEAT), "T15: Heartbeat fires at max escalation");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 16: Escalation decays one level per quiet hour
    {
        st.escalation_level = 2;
        st.hour_window_start_ms = now_ms;
        // 1 quiet hour passes
        now_ms += 3600001LL;
        WeatherSnapshot snap = st.baseline;
        evaluate(snap, policy, &st, 86400, now_ms);
        TEST_ASSERT(st.escalation_level == 1, "T16: 1 quiet hour decays escalation from 2 to 1");

        // Another quiet hour passes
        now_ms += 3600001LL;
        evaluate(snap, policy, &st, 86400, now_ms);
        TEST_ASSERT(st.escalation_level == 0, "T16: 2nd quiet hour decays escalation from 1 to 0");
    }

    // Test 17: event_budget_per_hour = 0 disables the backstop
    {
        WeatherChangePolicy unlim_policy = policy;
        unlim_policy.event_budget_per_hour = 0;
        st.events_this_hour = 100;
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_TEMPERATURE, st.baseline.values[VAR_TEMPERATURE] + 3.0f);
        now_ms += 400000;
        auto d = evaluate(snap, unlim_policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish, "T17: Unlimited budget publishes despite high event count");
        mark_success(&st, d, unlim_policy, now_ms);
    }

    // Test 18: Circular wind direction: 350° to 10° is 20° delta, not 340°
    {
        memset(&st, 0, sizeof(st));
        WeatherSnapshot snap;
        snap.clear_all();
        snap.set(VAR_WIND_SPEED, 5.0f);
        snap.set(VAR_WIND_DIRECTION, 350.0f);
        now_ms += 1000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        mark_success(&st, d, policy, now_ms);

        now_ms += 400000;
        snap.set(VAR_WIND_DIRECTION, 10.0f); // shortest angle delta = 20.0 (< 45.0 threshold)
        d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T18: 350 deg to 10 deg (20 deg delta) does not trigger 45 deg threshold");
    }

    // Test 19: Circular wind direction: 10° to 350° is 20° delta; 10° to 60° (50° delta) triggers
    {
        WeatherSnapshot snap;
        snap.clear_all();
        snap.set(VAR_WIND_SPEED, 5.0f);
        snap.set(VAR_WIND_DIRECTION, 40.0f); // delta = 50.0 deg from 350 deg (shortest is 50 deg >= 45 deg)
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & reason_for(VAR_WIND_DIRECTION)), "T19: 50 deg circular delta triggers");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 20: Wind direction calm gate: direction change at 2.9 m/s is ignored
    {
        WeatherSnapshot snap;
        snap.clear_all();
        snap.set(VAR_WIND_SPEED, 2.9f); // Below 3.0 m/s calm gate
        snap.set(VAR_WIND_DIRECTION, 180.0f); // 140 deg delta
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T20: Direction delta ignored below calm gate (2.9 m/s)");
    }

    // Test 21: Wind direction calm gate: direction change at 3.0 m/s is evaluated
    {
        WeatherSnapshot snap;
        snap.clear_all();
        snap.set(VAR_WIND_SPEED, 3.0f); // At 3.0 m/s calm gate
        snap.set(VAR_WIND_DIRECTION, 180.0f); // 140 deg delta >= 45.0
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & reason_for(VAR_WIND_DIRECTION)), "T21: Direction delta evaluated at 3.0 m/s");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 22: Accumulative rain: +0.6 mm triggers event
    {
        st.baseline.set(VAR_PRECIP_CUMULATIVE, 10.0f);
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_PRECIP_CUMULATIVE, 10.6f);
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & reason_for(VAR_PRECIP_CUMULATIVE)), "T22: +0.6 mm accum rain triggers");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 23: Accumulative rain: negative step (counter reset) rebases baseline without emitting event
    {
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_PRECIP_CUMULATIVE, 0.0f); // Reset from >0 to 0
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T23: Counter reset does NOT emit event");
        TEST_ASSERT(st.baseline.values[VAR_PRECIP_CUMULATIVE] == 0.0f, "T23: Baseline rebased to 0.0");
    }

    // Test 24: Accumulative rain: subsequent increase after negative step evaluates from new baseline
    {
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_PRECIP_CUMULATIVE, 0.6f); // +0.6 from new 0.0 baseline
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & reason_for(VAR_PRECIP_CUMULATIVE)), "T24: Accum rain from rebased baseline triggers");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 25: Rain start hysteresis: 0.0 to 0.2 mm/h fires RAIN_STARTED
    {
        WeatherSnapshot snap = st.baseline;
        st.rain_active = false;
        snap.set(VAR_PRECIP_RATE, 0.2f);
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & REASON_RAIN_STARTED), "T25: 0.2 mm/h fires RAIN_STARTED");
        TEST_ASSERT(st.rain_active == true, "T25: rain_active set to true");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 26: Rain stop hysteresis: 0.2 to 0.0 mm/h fires RAIN_STOPPED
    {
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_PRECIP_RATE, 0.0f);
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & REASON_RAIN_STOPPED), "T26: 0.0 mm/h fires RAIN_STOPPED");
        TEST_ASSERT(st.rain_active == false, "T26: rain_active set to false");
        mark_success(&st, d, policy, now_ms);
    }

    // Test 27: Rain hysteresis: 0.1 mm/h while not raining does not fire RAIN_STARTED
    {
        WeatherSnapshot snap = st.baseline;
        st.rain_active = false;
        snap.set(VAR_PRECIP_RATE, 0.1f);
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T27: 0.1 mm/h while stopped does not fire RAIN_STARTED");
    }

    // Test 28: Rain hysteresis: 0.1 mm/h while raining does not fire RAIN_STOPPED
    {
        WeatherSnapshot snap = st.baseline;
        st.rain_active = true;
        snap.set(VAR_PRECIP_RATE, 0.1f);
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T28: 0.1 mm/h while raining does not fire RAIN_STOPPED");
        st.rain_active = false;
    }

    // Test 29: Baseline missing field + current has field -> skipped, joins baseline on publish
    {
        st.rain_active = false;
        st.baseline.clear(VAR_SOLAR_RADIATION);
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_SOLAR_RADIATION, 500.0f);
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 0, now_ms);
        TEST_ASSERT(!d.publish, "T29: Missing baseline field does not trigger false event");
    }

    // Test 30: Baseline has field + current missing field -> skipped, no event, no rebase
    {
        st.baseline.set(VAR_TEMPERATURE, 20.0f);
        WeatherSnapshot snap = st.baseline;
        snap.clear(VAR_TEMPERATURE);
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 0, now_ms);
        TEST_ASSERT(!d.publish, "T30: Current missing field does not trigger false event");
        TEST_ASSERT(st.baseline.has(VAR_TEMPERATURE), "T30: Baseline field preserved");
    }

    // Test 31: Empty snapshot (valid_mask == 0) -> returns publish=false, mutates nothing
    {
        WeatherSnapshot empty;
        empty.clear_all();
        uint16_t prev_pending = st.pending_reasons;
        auto d = evaluate(empty, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T31: Empty snapshot returns false");
        TEST_ASSERT(st.pending_reasons == prev_pending, "T31: Pending reasons untouched");
    }

    // Test 32: Reboot state (baseline_valid == false) -> first evaluate returns HEARTBEAT
    {
        PublisherState boot_st;
        memset(&boot_st, 0, sizeof(boot_st));
        WeatherSnapshot snap;
        snap.clear_all();
        snap.set(VAR_TEMPERATURE, 25.0f);
        auto d = evaluate(snap, policy, &boot_st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & REASON_HEARTBEAT), "T32: Boot state returns HEARTBEAT");
    }

    // Test 33: Mark failure leaves baseline unchanged and pending reasons intact
    {
        PublisherState fail_st;
        memset(&fail_st, 0, sizeof(fail_st));
        WeatherSnapshot snap;
        snap.clear_all();
        snap.set(VAR_TEMPERATURE, 25.0f);
        auto d = evaluate(snap, policy, &fail_st, 3600, now_ms);
        mark_failure(&fail_st, d);
        TEST_ASSERT(!fail_st.baseline_valid, "T33: Failure leaves baseline invalid");
        TEST_ASSERT(fail_st.pending_reasons & REASON_HEARTBEAT, "T33: Pending reasons intact");
    }

    // Test 34: Mark success updates baseline to decision.observation and clears pending reasons
    {
        PublisherState succ_st;
        memset(&succ_st, 0, sizeof(succ_st));
        WeatherSnapshot snap;
        snap.clear_all();
        snap.set(VAR_TEMPERATURE, 25.0f);
        auto d = evaluate(snap, policy, &succ_st, 3600, now_ms);
        mark_success(&succ_st, d, policy, now_ms);
        TEST_ASSERT(succ_st.baseline_valid, "T34: Success marks baseline valid");
        TEST_ASSERT(succ_st.baseline.values[VAR_TEMPERATURE] == 25.0f, "T34: Baseline matches observation");
        TEST_ASSERT(succ_st.pending_reasons == 0, "T34: Pending reasons cleared");
    }

    // Test 35: Wire encoding/decoding roundtrip preserves all fields with integer precision
    {
        WeatherChangePolicy p1;
        default_policy(&p1);
        p1.thresholds[VAR_TEMPERATURE] = 3.5f;
        p1.thresholds[VAR_PRECIP_CUMULATIVE] = 0.75f;
        p1.event_cooldown_sec = 600;

        WeatherPolicyWire wire;
        encode_wire(p1, &wire);
        TEST_ASSERT(sizeof(wire) == 31, "T35: Wire struct is exactly 31 bytes");

        WeatherChangePolicy p2;
        bool ok = decode_wire(wire, &p2);
        TEST_ASSERT(ok, "T35: Decode wire succeeded");
        TEST_ASSERT(fabsf(p2.thresholds[VAR_TEMPERATURE] - 3.5f) < 0.01f, "T35: Temp preserved");
        TEST_ASSERT(fabsf(p2.thresholds[VAR_PRECIP_CUMULATIVE] - 0.75f) < 0.001f, "T35: Precip accum preserved");
        TEST_ASSERT(p2.event_cooldown_sec == 600, "T35: Cooldown preserved");
    }

    // Test 36: Canonical CRC16 matches on identical policies
    {
        WeatherChangePolicy p1, p2;
        default_policy(&p1);
        default_policy(&p2);
        uint16_t crc1 = crc16(p1);
        uint16_t crc2 = crc16(p2);
        TEST_ASSERT(crc1 == crc2 && crc1 != 0, "T36: Identical policies produce identical non-zero CRC16");
    }

    // Test 37: Canonical CRC16 changes when any threshold changes
    {
        WeatherChangePolicy p1, p2;
        default_policy(&p1);
        default_policy(&p2);
        p2.thresholds[VAR_PRESSURE] = 4.0f;
        TEST_ASSERT(crc16(p1) != crc16(p2), "T37: Altered policy produces different CRC16");
    }

    // Test 38: Decoupled disabled diurnal fields (illuminance, solar rad, UV) do not trigger events by default
    {
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_ILLUMINANCE, 50000.0f);
        snap.set(VAR_SOLAR_RADIATION, 800.0f);
        snap.set(VAR_UV_INDEX, 10.0f);
        now_ms += 400000;
        auto d = evaluate(snap, policy, &st, 3600, now_ms);
        TEST_ASSERT(!d.publish, "T38: Diurnal fields disabled by default do not trigger");
    }

    // Test 39: Enabling diurnal fields causes them to trigger events
    {
        WeatherChangePolicy diurnal_policy = policy;
        diurnal_policy.enabled_mask |= (1u << VAR_SOLAR_RADIATION);
        st.baseline.set(VAR_SOLAR_RADIATION, 100.0f);
        WeatherSnapshot snap = st.baseline;
        snap.set(VAR_SOLAR_RADIATION, 400.0f); // delta = 300 (>= 200)
        now_ms += 400000;
        auto d = evaluate(snap, diurnal_policy, &st, 3600, now_ms);
        TEST_ASSERT(d.publish && (d.reason_mask & reason_for(VAR_SOLAR_RADIATION)), "T39: Enabled solar radiation triggers");
        mark_success(&st, d, diurnal_policy, now_ms);
    }

    // Test 40: Format reasons produces correct formatted human strings
    {
        char buf[128];
        WeatherSnapshot cur, base;
        cur.clear_all();
        base.clear_all();
        cur.set(VAR_TEMPERATURE, 22.3f);
        base.set(VAR_TEMPERATURE, 20.0f);
        format_reasons(reason_for(VAR_TEMPERATURE) | REASON_RAIN_STARTED, cur, base, buf, sizeof(buf));
        TEST_ASSERT(strstr(buf, "Rain started") != nullptr, "T40: Formatted contains 'Rain started'");
        TEST_ASSERT(strstr(buf, "Temperature +2.3 °C") != nullptr, "T40: Formatted contains 'Temperature +2.3 °C'");
    }

    // Test 41: Reset baseline clears valid flag, pending reasons, and rain active state
    {
        st.baseline_valid = true;
        st.pending_reasons = 0x1234;
        st.rain_active = true;
        reset_baseline(&st);
        TEST_ASSERT(!st.baseline_valid, "T41: baseline_valid cleared");
        TEST_ASSERT(st.pending_reasons == 0, "T41: pending_reasons cleared");
        TEST_ASSERT(!st.rain_active, "T41: rain_active cleared");
    }

    std::cout << "\n==========================================" << std::endl;
    std::cout << "All " << g_tests_passed << " / " << g_tests_run << " assertions PASSED successfully!" << std::endl;
    std::cout << "==========================================" << std::endl;
    return 0;
}
