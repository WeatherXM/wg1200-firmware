#ifndef PACKET_RECEPTION_H
#define PACKET_RECEPTION_H

#include <stdint.h>
#include <string.h>

namespace PacketReception
{

/** Number of received station packets represented by the reception metric. */
const uint8_t COUNTER_WINDOW_SIZE = 4;

/**
 * Sliding window of packet counters received from the station.
 *
 * The station increments its counter only when it actually transmits. A gap
 * therefore represents packets lost over the air, while elapsed wall-clock
 * slots with a sequential next counter may be intentional adaptive silence.
 */
struct CounterWindow
{
	uint16_t counters[COUNTER_WINDOW_SIZE] = {};
	uint8_t count = 0;
	uint8_t next = 0;
};

inline void reset(CounterWindow *window)
{
	if(window == nullptr)
	{
		return;
	}
	memset(window, 0, sizeof(*window));
}

inline uint16_t newest_counter(const CounterWindow &window)
{
	uint8_t newest_index = window.next == 0
		? COUNTER_WINDOW_SIZE - 1
		: window.next - 1;
	return window.counters[newest_index];
}

/** Add one received counter, starting a fresh window after a station reset. */
inline void record(CounterWindow *window, uint16_t counter, bool station_reset = false)
{
	if(window == nullptr)
	{
		return;
	}

	// A lower counter is also treated as a reset defensively. This covers a
	// reboot/counter wrap even if the station-specific decoder did not flag it.
	if(station_reset || (window->count > 0 && counter < newest_counter(*window)))
	{
		reset(window);
	}

	window->counters[window->next] = counter;
	window->next = (window->next + 1) % COUNTER_WINDOW_SIZE;
	if(window->count < COUNTER_WINDOW_SIZE)
	{
		window->count++;
	}
}

/**
 * Return received / expected packets for the counter span in the window.
 * The result is clamped to 1..100 to retain the gateway's existing API.
 */
inline uint8_t percentage(const CounterWindow &window)
{
	if(window.count == 0)
	{
		return 0;
	}

	uint8_t oldest_index = window.count < COUNTER_WINDOW_SIZE ? 0 : window.next;
	uint16_t oldest = window.counters[oldest_index];
	uint16_t newest = newest_counter(window);
	uint32_t expected = newest >= oldest
		? (uint32_t)newest - (uint32_t)oldest + 1U
		: 1U;
	uint32_t reception = ((uint32_t)window.count * 100U) / expected;

	if(reception > 100U)
	{
		reception = 100U;
	}
	if(reception < 1U)
	{
		reception = 1U;
	}
	return (uint8_t)reception;
}

}

#endif
