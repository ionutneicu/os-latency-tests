//============================================================================
// Name        : os-realtime-tests.cpp
// Author      : 
// Version     :
// Copyright   : Your copyright notice
// Description : Hello World in C++, Ansi-style
//============================================================================

#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <functional>
#include "ascii-histogram.h"
using namespace std;

using sys_clock_t = std::chrono::high_resolution_clock;

struct thread_data_t
{
	std::chrono::time_point<sys_clock_t> thread_about_to_be_created_time;  // right before thread_created
	std::chrono::time_point<sys_clock_t> thread_entered_time;              // right after thread function entered
	std::chrono::time_point<sys_clock_t> thread_about_to_exit_time;		   // right before thread function exited
	std::chrono::time_point<sys_clock_t> thread_join_time;                 // right after thread joined
};

// number of iteration / measurements per each tests
static constexpr size_t NUM_LOOPS = 1000;

// test collected data
using test_data = std::vector< thread_data_t >;

// allocate all elements to avoid memory allocation interferences
// the data will be overwritten on each test
test_data m_data( NUM_LOOPS );


// sort of thread that does nothing, used just to measure
// thread creation latency and thread joining latency
void thread_function_empty( thread_data_t& thread_data )
{
	thread_data.thread_entered_time = sys_clock_t::now();
	thread_data.thread_about_to_exit_time = sys_clock_t::now();
}

// a thread function that calls another std::function with different argument
template<class T>
void thread_function( thread_data_t& thread_data, std::function< void( T& arg )>& func, T& arg  )
{
	thread_data.thread_entered_time = sys_clock_t::now();
	func(arg);
	thread_data.thread_about_to_exit_time = sys_clock_t::now();
}



template <class progress_t>
void init_progress( progress_t& p, unsigned int max_loops )
{
	p.m_max_value     = max_loops;
	p.m_prev_progress = -1;
}

template <class progress_t>
void print_progress( const std::string& pre, progress_t& p )
{
	unsigned int percent = p.m_current_value * 100 / p.m_max_value;
	if( percent != p.m_prev_progress )
	{
		p.m_prev_progress = percent;
		std::cout << "\r" << pre << percent << " %";
		std::cout.flush();
	}
}


template <typename  element_t>
void dump_hist_data(const std::vector<element_t>& container)
{
	std::cout << "nr_crt;duration" << std::endl;
	int i = 0;
	for( auto element : container )
	{
		std::cout << ++i << "," << std::to_string(element) << std::endl;
	}
	std::cout << std::endl;
}


struct progress_t
{
	unsigned int 	m_prev_progress;
	unsigned int	m_max_value;
	unsigned int 	m_current_value;
};

void test_thread_creation()
{
		// Test thread creation
		cout << "Running thread creation/join benchmark" << std::endl;

		progress_t p;
		init_progress( p,  NUM_LOOPS);
		for( p.m_current_value = 0; p.m_current_value < NUM_LOOPS; ++p.m_current_value )
		{
			print_progress("progress:", p);
			thread_data_t thread_data;
			m_data[p.m_current_value].thread_about_to_be_created_time = sys_clock_t::now();
			std::thread th = std::thread( thread_function_empty, std::ref(m_data[p.m_current_value]));
			th.join();
			m_data[p.m_current_value].thread_join_time = sys_clock_t::now();
		}
		print_progress("progress:", p);

		std::vector<int> hist_data;
		std::transform( m_data.begin(),
				        m_data.end(),
						std::back_inserter( hist_data ),
						[]( thread_data_t& current ){ return ( std::chrono::duration_cast<std::chrono::nanoseconds>
														(current.thread_entered_time - current.thread_about_to_be_created_time)).count(); } );

		draw_ascii_hystogram( hist_data, "Thread startup latency" );
		std::cout << "-------------------------------------------------------------------------------------------------------------------------------------" << std::endl;
		hist_data.clear();
		std::transform( m_data.begin(),
				        m_data.end(),
						std::back_inserter( hist_data ),
						[]( thread_data_t& current ){ return std::chrono::duration_cast< std::chrono::nanoseconds>(
															 current.thread_join_time - current.thread_about_to_exit_time).count(); } );
		draw_ascii_hystogram( hist_data, "Thread join latency" );
		std::cout << "-------------------------------------------------------------------------------------------------------------------------------------" << std::endl;
}

void test_mutex_awake_latency()
{

		std::cout << "Run mutex unlock latency" << std::endl;
		std::function<void( std::mutex&)> lambda  = [] ( std::mutex& m )
													 {
														std::this_thread::sleep_for(std::chrono::milliseconds(10));
													 	m.unlock();
													 };
		progress_t p;
		init_progress( p,  NUM_LOOPS);
		for( p.m_current_value = 0; p.m_current_value < NUM_LOOPS; ++p.m_current_value )
		{
			print_progress("progress:", p);
			thread_data_t thread_data;
			std::mutex m;
			m_data[p.m_current_value].thread_about_to_be_created_time = sys_clock_t::now();
			m.lock();
			std::thread th = std::thread(thread_function<std::mutex>,
										 std::ref(m_data[p.m_current_value]),
										 std::ref(lambda),
										 std::ref(m) );
			m.lock();
			m_data[p.m_current_value].thread_join_time = sys_clock_t::now();
			th.join();
			m.unlock();
		}
		print_progress("progress:", p);
		std::vector<unsigned int> hist;

		std::transform( m_data.begin(),
						m_data.end(),
						std::back_inserter( hist ),
						[]( thread_data_t& current ){
													   return std::chrono::duration_cast< std::chrono::nanoseconds>(
															current.thread_join_time - current.thread_about_to_exit_time).count();
											});
		draw_ascii_hystogram(hist, "Mutex wakeup latency");

		std::cout << "-------------------------------------------------------------------------------------------------------------------------------------" << std::endl;

}

void test_cv_awake_latency()
{
	std::cout << "Run condition variable unlock latency" << std::endl;


		std::function<void( std::condition_variable&)> lambda2  = [] ( std::condition_variable& cv )
													 {
														std::this_thread::sleep_for(std::chrono::milliseconds(10));
													 	cv.notify_one();
													 };

		progress_t p;
		init_progress( p,  NUM_LOOPS);
		for( p.m_current_value = 0; p.m_current_value < NUM_LOOPS; ++p.m_current_value )
		{
			print_progress("progress:", p);
			thread_data_t thread_data;
			std::mutex m;
			std::condition_variable cv;
			std::unique_lock<std::mutex> lock(m);
			m_data[p.m_current_value].thread_about_to_be_created_time = sys_clock_t::now();
			std::thread th = std::thread(thread_function<std::condition_variable>,
										 std::ref(m_data[p.m_current_value]),
										 std::ref(lambda2),
										 std::ref(cv) );
			cv.wait(lock);
			m_data[p.m_current_value].thread_join_time = sys_clock_t::now();
			th.join();
			m.unlock();
		}
		print_progress("progress:", p);
		std::chrono::nanoseconds avg_cv_wakep_latency  = std::accumulate( m_data.begin(),
											  m_data.end(),
											  std::chrono::nanoseconds(0),
											  []( 	std::chrono::nanoseconds& prev, thread_data_t& current ){
													auto dur = std::chrono::duration_cast< std::chrono::nanoseconds>(
																current.thread_join_time - current.thread_about_to_exit_time);
													return dur + prev;
												}) / m_data.size();

	    std::cout << "average condition variable notify latency : " << std::to_string( avg_cv_wakep_latency ) << std::endl;

	    std::cout << "-------------------------------------------------------------------------------------------------------------------------------------" << std::endl;

}

void test_malloc_latency( const size_t & block_size )
{
		    std::cout << "Run malloc latency, block size = " << block_size << " bytes" << std::endl;

			std::function<void( void **&)> thread_task  = [block_size] ( void**& p)
														 {
														 	*p = malloc(block_size);
														 };

			progress_t progress;
			init_progress( progress,  NUM_LOOPS);
			for( progress.m_current_value = 0; progress.m_current_value < NUM_LOOPS; ++progress.m_current_value )
			{
				print_progress("progress:", progress);
				thread_data_t thread_data;

				m_data[progress.m_current_value].thread_about_to_be_created_time = sys_clock_t::now();

				void* p;
				void **pp = &p;


				std::thread th = std::thread(thread_function<void **>,
											 std::ref(m_data[progress.m_current_value]),
											 std::ref(thread_task),
											 std::ref<void **>( pp ) );

				m_data[progress.m_current_value].thread_join_time = sys_clock_t::now();
				th.join();
				free(p);

			}
			print_progress("progress:",progress);
			std::chrono::nanoseconds avg_malloc_latency  = std::accumulate( m_data.begin(),
												  m_data.end(),
												  std::chrono::nanoseconds(0),
												  []( 	std::chrono::nanoseconds& prev, thread_data_t& current ){

														auto dur = std::chrono::duration_cast< std::chrono::nanoseconds>(
																	current.thread_about_to_exit_time - current.thread_entered_time);
														return dur + prev;
													} ) / m_data.size();

		    std::cout << "average malloc latency, block size = " << block_size << " : " << format_duration( avg_malloc_latency ) << std::endl;
		    std::cout << "-------------------------------------------------------------------------------------------------------------------------------------" << std::endl;
}



void test_free_latency( const size_t & block_size )
{
		    std::cout << "Run free latency, block size = " << block_size << " bytes" << std::endl;

			std::function<void( void *&)> lambda3  = [] ( void*& p)
														 {
														 	free(p);
														 };

			unsigned int percent = 0;
			unsigned int last_percent = 0;
			for( unsigned int i = 0; i < NUM_LOOPS; ++i )
			{
				percent = i*100/NUM_LOOPS;
				if( last_percent !=  percent )
				{
					std::cout << "\r progress: " << percent << "%";
					std::cout.flush();
					last_percent = percent;
				}
				thread_data_t thread_data;

				m_data[i].thread_about_to_be_created_time = sys_clock_t::now();


				void* p = malloc(block_size);


				std::thread th = std::thread(thread_function<void *>,
											 std::ref(m_data[i]),
											 std::ref(lambda3),
											 std::ref<void *>( p ) );

				m_data[i].thread_join_time = sys_clock_t::now();
				th.join();


			}
			std::cout << "\r progress: 100%" << std::endl;

			std::chrono::nanoseconds avg_malloc_latency  = std::accumulate( m_data.begin(),
												  m_data.end(),
												  std::chrono::nanoseconds(0),
												  []( 	std::chrono::nanoseconds& prev, thread_data_t& current ){

														auto dur = std::chrono::duration_cast< std::chrono::nanoseconds>(
																	current.thread_about_to_exit_time - current.thread_entered_time);
														return dur + prev;
													} ) / m_data.size();

		    std::cout << "average free latency, block size = " << block_size << " : " << format_duration( avg_malloc_latency ) << std::endl;
		    std::cout << "-------------------------------------------------------------------------------------------------------------------------------------" << std::endl;
}

int main() {
	cout << "Running task awakening latency tests" << endl;
	std::cout << "-------------------------------------------------------------------------------------------------------------------------------------" << std::endl;
	/*std::vector<int> v{0,1,2,1,2,3,3,3,4,5,4,5,6, 12,12,20};
	for( int i = 0; i < 55; ++i )
		v.push_back(3);
	draw_ascii_hystogram(v, "test", 80);
	return 0;*/
	test_thread_creation();
	test_mutex_awake_latency();
	test_cv_awake_latency();
	for( int i = 1; i <= 65536; i = i*2 )
	{
		test_malloc_latency(1024*i);
		test_free_latency(1024*i);
	}
	return 0;
}
