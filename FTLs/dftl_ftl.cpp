/* Copyright 2011 Matias Bjørling */

/* dftp_ftl.cpp  */

/* FlashSim is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version. */

/* FlashSim is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. */

/* You should have received a copy of the GNU General Public License
 * along with FlashSim.  If not, see <http://www.gnu.org/licenses/>. */

/****************************************************************************/

/* Implementation of the DFTL described in the paper
 * "DFTL: A Flasg Translation Layer Employing Demand-based Selective Caching og Page-level Address Mappings"
 *
 * Global Mapping Table GMT
 * Global Translation Directory GTD (Maintained in memory)
 * Cached Mapping Table CMT (Uses LRU to pick victim)
 *
 * Dlpn/Dppn Data Logical/Physical Page Number
 * Mlpn/Mppn Translation Logical/Physical Page Number
 */


#include <new>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include <queue>
#include <iostream>
#include "../ssd.h"

using namespace ssd;

FtlImpl_Dftl::MPage::MPage()
{
	this->vpn = -1;
	this->ppn = -1;
	this->create_ts = -1;
	this->modified_ts = -1;
}


FtlImpl_Dftl::FtlImpl_Dftl(Controller &controller):
	FtlParent(controller)
{
	// Trivial assumption checks
	if (sizeof(int) != 4) assert("integer is not 4 bytes");

	addressPerPage = 0;
	addressSize = 32;

	currentDataPage = -1;
	currentTranslationPage = -1;

	// Detect required number of bits for logical address size
	//for (int size = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE * 4; size > 0; addressSize++) size /= 2;

	// Find required number of bits for block size
	addressPerPage = (PAGE_SIZE/ceil(addressSize / 8.0)); // 8 bits per byte.

	printf("Total required bits for representation: Address size: %i Total per page: %i \n", addressSize, addressPerPage);

	totalCMTentries = CACHE_DFTL_LIMIT * addressPerPage;
	printf("Number of elements in Cached Mapping Table (CMT): %i\n", totalCMTentries);

	// Initialise block mapping table.
	uint ssdSize = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE * BLOCK_SIZE;

	trans_map = new MPage[ssdSize];

	printf("Total size to map: %uKB\n", ssdSize * PAGE_SIZE / 1024);
	printf("Using DFTL.\n");
	return;
}


void FtlImpl_Dftl::select_victim_entry(FtlImpl_Dftl::MPage &mpage)
{
	double evictPageTime = trans_map[cmt.begin()->first].modified_ts;
	long evictPage = cmt.begin()->first;

	std::map<long, bool>::iterator i = cmt.begin();
	while (i != cmt.end())
	{
		if (evictPageTime >= trans_map[cmt.begin()->first].modified_ts)
			evictPage = i->first;

		++i;
	}

	mpage = trans_map[evictPage];
}

void FtlImpl_Dftl::remove_victims(FtlImpl_Dftl::MPage &mpage)
{

}

void FtlImpl_Dftl::consult_GTD(long dlpn, Event &event)
{
	// TODO: Add translation page counter

	// Lookup mapping page
	if (trans_map[dlpn].ppn == -1)
		trans_map[dlpn].vpn = dlpn;

	// Simulate that we goto translation map and read the mapping page.
	Event *readEvent = new Event(READ, event.get_logical_address(), 1, event.get_start_time());
	readEvent->set_address(Address(trans_map[dlpn].ppn, PAGE));
	readEvent->set_noop(true);

	Event *lastEvent = event.get_last_event(event);
	lastEvent->set_next(*readEvent);

	controller.stats.numFTLRead++;
}

void FtlImpl_Dftl::reset_MPage(FtlImpl_Dftl::MPage &mpage)
{
	mpage.create_ts = -1;
	mpage.modified_ts = -1;
	mpage.ppn = -1;
}

bool FtlImpl_Dftl::lookup_CMT(long dlpn, Event &event)
{
	if (cmt.find(dlpn) == cmt.end())
		return false;

	event.incr_time_taken(RAM_READ_DELAY);
	controller.stats.numMemoryRead++;

	return true;
}

long FtlImpl_Dftl::get_free_translation_page()
{
	if (currentTranslationPage == -1 || currentTranslationPage % BLOCK_SIZE == BLOCK_SIZE -1)
		currentTranslationPage = manager.get_free_block(LOG).get_linear_address();
	else
		currentTranslationPage++;

	return currentTranslationPage;
}

long FtlImpl_Dftl::get_free_data_page()
{
	if (currentDataPage == -1 || currentDataPage % BLOCK_SIZE == BLOCK_SIZE -1)
		currentDataPage = manager.get_free_block(DATA).get_linear_address();
	else
		currentDataPage++;

	return currentDataPage;
}

FtlImpl_Dftl::~FtlImpl_Dftl(void)
{
	delete trans_map;

	return;
}

void FtlImpl_Dftl::resolve_mapping(Event &event, bool isWrite)
{
	uint dlpn = event.get_logical_address();
	/* 1. Lookup in CMT if the mapping exist
	 * 2. If, then serve
	 * 3. If not, then goto GDT, lookup page
	 * 4. If CMT full, evict a page
	 * 5. Add mapping to CMT
	 */
	if (lookup_CMT(event.get_logical_address(), event))
	{
		controller.stats.numCacheHits++;

		if (isWrite)
		{
			trans_map[dlpn].modified_ts = event.get_start_time();

			// Inform the ssd model that it should invalidate the previous page.
			Address killAddress = Address(trans_map[dlpn].ppn, PAGE);
			event.set_replace_address(killAddress);
		}
	} else {
		controller.stats.numCacheFaults++;
		consult_GTD(dlpn, event);

		if (isWrite)
		{
			trans_map[dlpn].create_ts = event.get_start_time();
			trans_map[dlpn].modified_ts = event.get_start_time();
		}

		if (cmt.size() == totalCMTentries)
		{
			// Find page to evict
			MPage evictPage;
			select_victim_entry(evictPage);

			if (evictPage.create_ts != evictPage.modified_ts)
			{
				// Evict page
				// Inform the ssd model that it should invalidate the previous page.
				Address killAddress = Address(evictPage.ppn, PAGE);
				event.set_replace_address(killAddress);
			}

			// Remove page from cache.
			reset_MPage(evictPage);
			cmt.erase(evictPage.vpn);
		}

		cmt[dlpn] = true;
	}
}

enum status FtlImpl_Dftl::read(Event &event)
{
	uint dlpn = event.get_logical_address();

	resolve_mapping(event, false);

	event.set_address(Address(trans_map[dlpn].ppn, PAGE));

	controller.stats.numFTLRead++;

	if (controller.issue(event) == FAILURE)
		return FAILURE;

	event.consolidate_metaevent(event);

	return SUCCESS;
}


enum status FtlImpl_Dftl::write(Event &event)
{
	uint dlpn = event.get_logical_address();

	resolve_mapping(event, true);

	// Get next available data page
	trans_map[dlpn].ppn = get_free_data_page();

	event.set_address(Address(trans_map[dlpn].ppn, PAGE));

	controller.stats.numFTLWrite++;

	if (controller.issue(event) == FAILURE)
		return FAILURE;

	event.consolidate_metaevent(event);

	return SUCCESS;
}
