/*
 * ascii-histogram.h
 *
 *  Created on: Jun 7, 2022
 *      Author: ionut
 */

#ifndef ASCII_HISTOGRAM_H_
#define ASCII_HISTOGRAM_H_

#include <iostream>
#include <string>
#include <vector>
#include <chrono>

static constexpr size_t default_num_columns = 80;
static constexpr size_t default_num_rows = 50;


// format std::duration into string, choosing the appropiate unit
// for human readable results

template <class DurationType>
static std::string format_duration( DurationType& duration_ns )
{
	std::string unit;
	long unsigned int duration;
	auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>( duration_ns ).count();
	if( ns >= 10000 ) {
		auto us = std::chrono::duration_cast<std::chrono::microseconds>( duration_ns ).count();
		if( us >= 10000 ) {
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( duration_ns ).count();
			if( ms >= 10000 ) {
				duration = std::chrono::duration_cast<std::chrono::seconds>( duration_ns ).count();
				unit = "s";
			}
			else {
				duration = ms;
				unit = "ms";
			}
		}
		else{
			duration = us;
			unit = "us";
		}
	}
	else {
		duration = ns;
		unit = "ns";
	}
	std::ostringstream str;
	str << duration << " " << unit;
	return str.str();
}



namespace std{
	template <typename _CharT, typename _Traits>
	inline basic_ostream<_CharT, _Traits> &
	tab(basic_ostream<_CharT, _Traits> &__os) {
	  return __os.put(__os.widen('\t'));
	}

	template<typename _Rep>
	std::string to_string( const std::chrono::duration<_Rep> & value )
	{
		return format_duration(value);
	}

	std::string to_string( const std::chrono::nanoseconds& value )
	{
		return format_duration(value);
	}
}

template<typename _Rep >
int operator -( const std::chrono::duration<_Rep>& d1, const std::chrono::duration<_Rep>& d2 )
{
	return d1.count() - d2.count();
}





template <typename  element_t>
void draw_ascii_hystogram( const std::vector<element_t>& container,
						   std::string title,
						   size_t  num_columns = default_num_columns,
						   size_t  num_rows = default_num_rows,
						   std::string x_axis_title = "duration[ns]",
						   std::string y_axis_title = "frequency[times]")
{



	element_t max = *std::max_element( container.begin(), container.end() );
	element_t min = *std::min_element( container.begin(), container.end() );

	size_t num_buckets = max - min + 1;
	if( num_buckets > num_columns )
		num_buckets = num_columns;

	std::vector<size_t> num_occurences(num_buckets);
	std::fill( num_occurences.begin(), num_occurences.end(), 0 );

	element_t window_size = ( max - min )/ num_buckets;


	for( auto element : container )
	{
		int index =  ( element - min ) * ( num_buckets - 1 ) / ( max - min );
		++num_occurences[index];
	}
	size_t min_value = *std::min_element( num_occurences.begin(), num_occurences.end() );
	size_t max_value = *std::max_element( num_occurences.begin(), num_occurences.end() );

	std::cout << std::endl << std::endl;
	std::cout << "    " << y_axis_title << std::endl;
	std::cout << std::tab << "^" << std::endl;
    for( size_t row = 0; row < num_rows; ++ row )
    {

    	if( row == 0 )
    	{
    		std::cout << std::to_string(max_value) << std::tab << "|";
    	}
    	else
    		if( row == num_rows - 1)
    			std::cout << std::to_string(min_value) << std::tab << "|";
    		else
    			std::cout << std::tab << "|";
    	for( size_t col = 0; col < num_columns; ++ col )
    	{
    			auto value = num_occurences[ col * num_buckets / num_columns ];
				if( ( num_rows - row  ) <= ( value - min_value ) * num_rows / ( max_value - min_value ) )
					std::cout << "#" ;//static_cast<char>(250);
				else
					if( row == num_rows - 1 && value > 0 )
						std::cout << ".";
					else
						std::cout << " ";
    	}
    	std::cout << std::endl;
    }
    std::cout << std::tab << "+";
    for( size_t col = 0; col < num_columns; ++ col )
    {
    	std::cout << "-";
    }
    std::cout << "-> " << x_axis_title << std::endl;
    std::cout << std::tab << "|";
    for( size_t col = 0; col < num_columns-1; ++ col )
    {
    	std::cout << " ";
    }
    std::cout << "|" << std::endl;

    std::cout << std::tab << std::to_string(min);
    for( size_t col = 0; col < num_columns-1; ++ col )
    {
        	std::cout << " ";
    }
    std::cout << std::to_string(max) << std::endl<<std::endl;

    std::cout << std::tab;

    for( size_t col = 0; col < num_columns / 2 - title.length()/ 2; ++ col)
    	std::cout << " ";
    std::cout << title << std::endl;

    std::cout << "max = " << std::to_string(max) << std::endl;
    std::cout << "min = " << std::to_string(min) << std::endl;

    for( size_t col = 0; col < num_columns; ++ col )
    {
    	if( num_occurences[col] == max_value )
    	{
    		std::cout << "max occurencies " << std::to_string( min + col * window_size) << " - " << std::to_string( min + ( 1 + col ) * window_size ) << std::endl;
    	}
    }


}


#endif /* ASCII_HISTOGRAM_H_ */
