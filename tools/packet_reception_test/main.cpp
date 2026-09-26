#include <cassert>
#include <iostream>
#include "../../main/ws/include/packet_reception.h"

using namespace PacketReception;

static void expect(uint8_t actual, uint8_t expected, const char *message)
{
	if(actual != expected)
	{
		std::cerr << "FAILED: " << message << " (expected "
			<< (unsigned int)expected << ", got " << (unsigned int)actual << ")\n";
		assert(false);
	}
}

int main()
{
	CounterWindow window = {};
	expect(percentage(window), 0, "empty window");

	// Adaptive silence does not advance the station counter. Timeouts do not
	// call record(), so the next sequential packet keeps reception at 100%.
	record(&window, 167);
	expect(percentage(window), 100, "first packet");
	expect(percentage(window), 100, "timeout leaves reception unchanged");
	record(&window, 168);
	expect(percentage(window), 100, "sequential counter after adaptive silence");

	reset(&window);
	record(&window, 38);
	record(&window, 40);
	expect(percentage(window), 66, "one transmitted packet lost");
	record(&window, 41);
	record(&window, 42);
	expect(percentage(window), 80, "gap retained while inside four-packet window");
	record(&window, 43);
	expect(percentage(window), 100, "gap ages out of four-packet window");

	record(&window, 2, true);
	expect(percentage(window), 100, "explicit reboot starts a fresh window");
	record(&window, 3);
	expect(percentage(window), 100, "post-reboot sequence");

	// A counter decrease is a defensive implicit reset as well.
	record(&window, 1);
	expect(percentage(window), 100, "counter decrease starts a fresh window");

	reset(&window);
	record(&window, 1);
	record(&window, 1000);
	expect(percentage(window), 1, "large loss is clamped to one percent");

	std::cout << "Packet reception tests passed\n";
	return 0;
}
