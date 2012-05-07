/*
 * ssd_page_hotness_measurer.cpp
 *
 *  Created on: May 5, 2012
 *      Author: niv
 */

#include "ssd.h"
#include <cmath>

using namespace ssd;

#define INTERVAL_LENGTH 1000
#define WEIGHT 0.5


Page_Hotness_Measurer::Page_Hotness_Measurer()
	:	write_current_count(),
		write_moving_average(NUMBER_OF_ADDRESSABLE_BLOCKS * BLOCK_SIZE, 0),
		read_current_count(),
		read_moving_average(NUMBER_OF_ADDRESSABLE_BLOCKS * BLOCK_SIZE, 0),
		current_interval(0),
		num_wcrh_pages_per_die(SSD_SIZE, std::vector<uint>(PACKAGE_SIZE, 0)),
		num_wcrc_pages_per_die(SSD_SIZE, std::vector<uint>(PACKAGE_SIZE, 0)),
		current_reads_per_die(SSD_SIZE, std::vector<uint>(PACKAGE_SIZE, 0)),
		average_reads_per_die(SSD_SIZE, std::vector<double>(PACKAGE_SIZE, 0))
{}

Page_Hotness_Measurer::~Page_Hotness_Measurer(void) {}

enum write_hotness Page_Hotness_Measurer::get_write_hotness(unsigned long page_address) const {
	return write_moving_average[page_address] >= average_write_hotness ? WRITE_HOT : WRITE_COLD;
}

enum read_hotness Page_Hotness_Measurer::get_read_hotness(unsigned long page_address) const {
	return read_moving_average[page_address] >= average_read_hotness ? READ_HOT : READ_COLD;
}

Address Page_Hotness_Measurer::get_die_with_least_wcrh() const {
	uint package;
	uint die;
	double min = PLANE_SIZE * BLOCK_SIZE;
	for (uint i = 0; i < SSD_SIZE; i++) {
		for (uint j = 0; j < PACKAGE_SIZE; j++) {
			if (min < num_wcrh_pages_per_die[i][j]) {
				min = num_wcrh_pages_per_die[i][j];
				package = i;
				die = j;
			}
		}
	}
	return Address(package, die, 0,0,0, DIE);
}

Address Page_Hotness_Measurer::get_die_with_least_wcrc() const {
	uint package;
	uint die;
	double min = PLANE_SIZE * BLOCK_SIZE;
	for (uint i = 0; i < SSD_SIZE; i++) {
		for (uint j = 0; j < PACKAGE_SIZE; j++) {
			if (min < num_wcrc_pages_per_die[i][j]) {
				min = num_wcrc_pages_per_die[i][j];
				package = i;
				die = j;
			}
		}
	}
	return Address(package, die, 0,0,0, DIE);
}

void Page_Hotness_Measurer::register_event(Event const& event) {
	enum event_type type = event.get_event_type();
	assert(type == WRITE || type == READ_COMMAND);
	double time = event.get_start_time() + event.get_time_taken();
	check_if_new_interval(time);
	ulong page_address = event.get_address().get_linear_address();
	if (type == WRITE) {
		write_current_count[page_address]++;
	} else if (type == READ_COMMAND) {
		current_reads_per_die[event.get_address().package][event.get_address().die]++;
		read_current_count[page_address]++;
	}
}

void Page_Hotness_Measurer::check_if_new_interval(double time) {
	int how_many_intervals_into_the_future = trunc((time - current_interval * INTERVAL_LENGTH) / INTERVAL_LENGTH);
	assert(how_many_intervals_into_the_future >= 0);
	if (how_many_intervals_into_the_future == 0) {
		return;
	}

	average_write_hotness = 0;
	average_read_hotness = 0;
	double p = pow(WEIGHT, how_many_intervals_into_the_future - 1);
	for( uint addr = 0; addr < write_moving_average.size(); addr++  )
	{
	    uint count = write_current_count[addr];
	    write_moving_average[addr] = write_moving_average[addr] * WEIGHT + count * (1 - WEIGHT);
	    write_moving_average[addr] *= p;
	    average_write_hotness += write_moving_average[addr];

	    count = read_current_count[addr];
	    read_current_count[addr] = read_current_count[addr] * WEIGHT + count * (1 - WEIGHT);
	    read_current_count[addr] *= p;
	    average_read_hotness += read_current_count[addr];
	}
	average_write_hotness /= write_moving_average.size();
	average_read_hotness /= write_moving_average.size();

	for (uint i = 0; i < SSD_SIZE; i++) {
		for (uint j = 0; j < PACKAGE_SIZE; j++) {
			average_reads_per_die[i][j] = average_reads_per_die[i][j] * WEIGHT + current_reads_per_die[i][j] * (1 - WEIGHT);
			current_reads_per_die[i][j] = 0;
			num_wcrc_pages_per_die[i][j] = 0;
			num_wcrh_pages_per_die[i][j] = 0;
		}
	}

	for( uint addr = 0; addr < write_moving_average.size(); addr++  )
	{
		if (get_write_hotness(addr) == WRITE_COLD) {
			Address a = Address(addr, PAGE);
			if (get_read_hotness(addr) == READ_COLD) {
				num_wcrc_pages_per_die[a.package][a.die]++;
			} else {
				num_wcrh_pages_per_die[a.package][a.die]++;
			}
		}
	}


}
