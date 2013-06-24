/*++++----------------------------------------------
 * ssd_os.cpp
 *
 *  Created on: Jul 20, 2012
 *      Author: niv
 */

#include "../ssd.h"
using namespace ssd;

OperatingSystem::OperatingSystem()
	: ssd(new Ssd()),
	  events(NULL),
	  last_dispatched_event_minimal_finish_time(1),
	  threads(),
	  NUM_WRITES_TO_STOP_AFTER(UNDEFINED),
	  num_writes_completed(0),
	  time_of_last_event_completed(1),
	  counter_for_user(1),
	  time_of_experiment_start(UNDEFINED),
	  idle_time(0),
	  time(0),
	  MAX_OUTSTANDING_IOS_PER_THREAD(16)
{
	ssd->set_operating_system(this);
	//assert(MAX_SSD_QUEUE_SIZE >= SSD_SIZE * PACKAGE_SIZE);
}

void OperatingSystem::set_threads(vector<Thread*> new_threads) {
	for (uint i = 0; i < threads.size(); i++) {
		delete threads[i];
	}
	if (events != NULL) {
		delete events;
	}
	num_writes_completed = 0;
	threads.clear();
	assert(new_threads.size() > 0);
	events = new Pending_Events(new_threads.size());
	for (uint i = 0; i < new_threads.size(); i++) {
		Thread* t = new_threads[i];
		threads.push_back(t);
		t->init(this, time);
		get_next_ios(i);
	}
}

OperatingSystem::~OperatingSystem() {
	delete ssd;
	for (uint i = 0; i < threads.size(); i++) {
		Thread* t = threads[i];
		if (t != NULL) {
			delete t;
		}
	}
	threads.clear();
	delete events;
}

OperatingSystem::Pending_Events::Pending_Events(int num_threads)
	:	event_queues(num_threads, deque<Event*>()),
		num_pending_events(0) {}

OperatingSystem::Pending_Events::~Pending_Events() {
	for (uint i = 0; i < event_queues.size(); i++) {
		deque<Event*> queue = event_queues[i];
		while (!queue.empty()) {
			Event* e = queue.front();
			queue.pop_front();
			delete e;
		}
	}
	event_queues.clear();
}

Event* OperatingSystem::Pending_Events::pop(int i) {
	deque<Event*>& queue = event_queues[i];
	Event* event = queue.front();
	if (event != NULL) {
		queue.pop_front();
		num_pending_events--;
	}
	return event;
}

void OperatingSystem::Pending_Events::append(int i, Event* event) {
	deque<Event*>& queue = event_queues[i];
	queue.push_back(event);
	num_pending_events++;
}

Event* OperatingSystem::Pending_Events::peek(int i) {
	deque<Event*>& queue = event_queues[i];
	return queue.size() > 0 ? queue.front() : NULL;
}

void OperatingSystem::run() {
	const int idle_limit = 5000000;
	bool finished_experiment, still_more_work;
	do {
		int thread_id = pick_unlocked_event_with_shortest_start_time();
		bool no_pending_event = thread_id == UNDEFINED;
		bool queue_is_full = currently_executing_ios.size() >= MAX_SSD_QUEUE_SIZE;
		if (no_pending_event || queue_is_full) {
			if (idle_time > 100000 && idle_time % 100000 == 0) {
				printf("Idle for %f seconds. No_pending_event=%d  Queue_is_full=%d\n", (double) idle_time / 1000000, no_pending_event, queue_is_full);
			}
			if (idle_time >= idle_limit) {
				printf("Idle time limit reached\nRunning IOs:");
				for (set<uint>::iterator it = currently_executing_ios.begin(); it != currently_executing_ios.end(); it++) {
					printf("%d ", *it);
				}
				printf("\n");
				throw;
			}
			ssd->progress_since_os_is_waiting();
			idle_time++;
		}
		else {
			dispatch_event(thread_id);
		}

		if ((double)num_writes_completed / NUM_WRITES_TO_STOP_AFTER > (double)counter_for_user / 10.0) {
			printf("finished %d%%.\t\tNum writes completed:  %d \n", counter_for_user * 10, num_writes_completed);
			if (counter_for_user == 9) {
				//PRINT_LEVEL = 1;
				//VisualTracer::get_instance()->print_horizontally(10000);
				//VisualTracer::get_instance()->print_horizontally_with_breaks_last(10000);
			}
			counter_for_user++;
		}

		finished_experiment = NUM_WRITES_TO_STOP_AFTER != UNDEFINED && NUM_WRITES_TO_STOP_AFTER <= num_writes_completed;
		still_more_work = currently_executing_ios.size() > 0 || events->get_num_pending_events() > 0;
		//printf("num_writes   %d\n", num_writes_completed);
	} while (!finished_experiment && still_more_work);

	for (uint i = 0; i < threads.size(); i++) {
		threads[i]->set_finished();
	}
}

int OperatingSystem::pick_unlocked_event_with_shortest_start_time() {
	double soonest_time = numeric_limits<double>::max();
	int thread_id = UNDEFINED;
	for (int i = 0; i < events->size(); i++) {
		Event* e = events->peek(i);
		if (e != NULL && e->get_start_time() < soonest_time && !is_LBA_locked(e->get_logical_address()) ) {
			soonest_time = e->get_start_time();
			thread_id = i;
		}
	}
	return thread_id;
}

void OperatingSystem::dispatch_event(int thread_id) {
	idle_time = 0;
	Event* event = events->pop(thread_id);
	if (event->get_start_time() < time) {
		event->incr_os_wait_time(time - event->get_start_time());
	}
	//printf("submitting:   " ); event->print();

	currently_executing_ios.insert(event->get_application_io_id());
	app_id_to_thread_id_mapping[event->get_application_io_id()] = thread_id;

	//Thread* dispatching_thread = threads[thread_id];
	//dispatching_thread->set_time(event->get_ssd_submission_time());

	double min_completion_time = get_event_minimal_completion_time(event);
	last_dispatched_event_minimal_finish_time = max(last_dispatched_event_minimal_finish_time, min_completion_time);

	lock(event, thread_id);

	//printf("dispatching:\t"); event->print();

	if (time_of_experiment_start == UNDEFINED && event->is_experiment_io()) {
		time_of_experiment_start = event->get_current_time();
	}

	ssd->submit(event);
}

void OperatingSystem::get_next_ios(int thread_id) {
	Thread* t = threads[thread_id];
	Event* e;
	do  {
		e = t->next();
		if (e != NULL) {
			if (e->get_start_time() < time) {
				e->set_start_time(time);
			}
			events->append(thread_id, e);
		}
	} while (e != NULL && events->get_num_pending_ios_for_thread(thread_id) < MAX_OUTSTANDING_IOS_PER_THREAD);
}

void OperatingSystem::setup_follow_up_threads(int thread_id, double time) {
	Thread* thread = threads[thread_id];
	vector<Thread*> follow_up_threads = thread->get_follow_up_threads();
	if (follow_up_threads.size() == 0) return;
	if (PRINT_LEVEL >= 1) printf("Switching to new follow up thread\n");

	threads[thread_id] = follow_up_threads[0];
	follow_up_threads[thread_id]->init(this, time);
	get_next_ios(thread_id);

	for (uint i = 1; i < follow_up_threads.size(); i++) {
		threads.push_back(follow_up_threads[i]);
		follow_up_threads[i]->init(this, time);
		events->push_back();
		get_next_ios(thread_id);
	}
	thread->get_follow_up_threads().clear();
	delete thread;
}

void OperatingSystem::register_event_completion(Event* event) {

	bool queue_was_full = currently_executing_ios.size() == MAX_SSD_QUEUE_SIZE;
	currently_executing_ios.erase(event->get_application_io_id());

	//printf("finished:\t"); event->print();
	//printf("queue size:\t%d\n", currently_executing_ios_counter);

	release_lock(event);

	long thread_id = app_id_to_thread_id_mapping[event->get_application_io_id()];
	Thread* thread = threads[thread_id];
	thread->register_event_completion(event);
	get_next_ios(thread_id);

	if (!event->get_noop() /*&& event->get_event_type() == WRITE*/ && event->is_experiment_io() && event->get_event_type() != TRIM) {
		num_writes_completed++;
	}

	if (thread->is_finished()) {
		setup_follow_up_threads(thread_id, event->get_current_time());
	}

	// we update the current time of all threads
	double new_time = queue_was_full ? event->get_current_time() : event->get_ssd_submission_time();
	time = max(time, new_time);
	update_thread_times(time);

	time_of_last_event_completed = max(time_of_last_event_completed, event->get_current_time());

	int thread_with_soonest_event = pick_unlocked_event_with_shortest_start_time();
	if (thread_with_soonest_event != UNDEFINED) {
		dispatch_event(thread_with_soonest_event);
	}

	delete event;
}

void OperatingSystem::update_thread_times(double time) {
	for (uint i = 0; i < threads.size(); i++) {
		Thread* t = threads[i];
		if (!t->is_finished() && t->get_time() < time) {
			t->set_time(time + 1);
		}
	}
}

void OperatingSystem::set_num_writes_to_stop_after(long num_writes) {
	NUM_WRITES_TO_STOP_AFTER = num_writes;
}

double OperatingSystem::get_event_minimal_completion_time(Event const*const event) const {
	double result = event->get_start_time();
	if (event->get_event_type() == WRITE) {
		result += 2 * BUS_CTRL_DELAY + BUS_DATA_DELAY + PAGE_WRITE_DELAY;
	}
	else if (event->get_event_type() == READ) {
		result += 2 * BUS_CTRL_DELAY + BUS_DATA_DELAY + PAGE_READ_DELAY;
	}
	return result;
}

void OperatingSystem::lock(Event* event, int thread_id) {
	if (event->is_flexible_read()) return;
	event_type type = event->get_event_type();
	long logical_address = event->get_logical_address();
	map<long, queue<uint> >& map = (type == READ || type == READ_TRANSFER) ? read_LBA_to_thread_id :
				type == WRITE ? write_LBA_to_thread_id : trim_LBA_to_thread_id;
	map[logical_address].push(thread_id);
}

void OperatingSystem::release_lock(Event* event) {
	event_type type = event->get_event_type();
	long logical_address = event->get_logical_address();
	map<long, queue<uint> >& map = (type == READ || type == READ_TRANSFER) ? read_LBA_to_thread_id :
			type == WRITE ? write_LBA_to_thread_id : trim_LBA_to_thread_id;
	if (map.count(logical_address) == 1) {
		map[logical_address].pop();
		if (map[logical_address].size() == 0) {
			map.erase(logical_address);
		}
	}
}

bool OperatingSystem::is_LBA_locked(ulong lba) {
	if (!OS_LOCK) {
		return false;
	} else {
		return read_LBA_to_thread_id.count(lba) > 0 || write_LBA_to_thread_id.count(lba) > 0 || trim_LBA_to_thread_id.count(lba) > 0;
	}
}

double OperatingSystem::get_experiment_runtime() const {
	return time_of_last_event_completed - time_of_experiment_start;
}

Flexible_Reader* OperatingSystem::create_flexible_reader(vector<Address_Range> ranges) {
	FtlParent* ftl = ssd->get_ftl();
	Flexible_Reader* reader = new Flexible_Reader(*ftl, ranges);
	return reader;
}

