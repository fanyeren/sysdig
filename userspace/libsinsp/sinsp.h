/*
Copyright (C) 2013-2014 Draios inc.

This file is part of sysdig.

sysdig is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

sysdig is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with sysdig.  If not, see <http://www.gnu.org/licenses/>.
*/

/*!
	\mainpage libsinsp documentation

	\section Introduction

	libsinsp is a system inspection library written in C++ and implementing high level
	functionlity like:
	- live capture control (start/stop/pause...)
	- event capture from file or the live OS
	- OS state reconstruction. By parsing /proc and inspecting the live event stream,
	libsinsp is capable of mirroring the OS process state and putting context around
	key OS primitives like process IDs and file descriptors. That way, these primitives
	can be treated like programs, files, connections and users.
	- parsing of OS events and conversion of events into human-readable strings
	- event filtering

	This manual includes the following sections:
	- \ref inspector
	- \ref event
	- \ref dump
	- \ref filter
	- \ref state
*/

#pragma once
#ifdef _WIN32
#pragma warning(disable: 4251)
#endif

#ifdef _WIN32
#define SINSP_PUBLIC __declspec(dllexport)
#include <Ws2tcpip.h>
#else
#define SINSP_PUBLIC
#include <arpa/inet.h>
#endif

#define __STDC_FORMAT_MACROS

#include <string>
#include <unordered_map>
#include <map>
#include <queue>
#include <vector>
#include <set>

using namespace std;

#include <scap.h>
#include "settings.h"
#include "logger.h"
#include "event.h"
#include "filter.h"
#include "dumper.h"
#include "stats.h"
#include "ifinfo.h"
#include "chisel.h"
#include "container.h"

#ifndef VISIBILITY_PRIVATE
#define VISIBILITY_PRIVATE private:
#endif

#define ONE_SECOND_IN_NS 1000000000LL

//
// Protocol decoder callback type
//
typedef enum sinsp_pd_callback_type
{
	CT_OPEN,
	CT_CONNECT,
	CT_READ,
	CT_WRITE,
	CT_TUPLE_CHANGE,
}sinsp_pd_callback_type;

#include "tuples.h"
#include "fdinfo.h"
#include "threadinfo.h"
#include "ifinfo.h"
#include "eventformatter.h"

class sinsp_partial_transaction;
class sinsp_parser;
class sinsp_analyzer;
class sinsp_filter;
class cycle_writer;
class sinsp_protodecoder;

vector<string> sinsp_split(const string &s, char delim);

/*!
  \brief Information about a group of filter/formatting fields.
*/
class filter_check_info
{
public:
	enum flags
	{
		FL_NONE =   0,
		FL_WORKS_ON_THREAD_TABLE = (1 << 0),	///< This filter check class supports filtering incomplete events that contain only valid thread info and FD info.
	};

	filter_check_info()
	{
		m_flags = 0;
	}

	string m_name; ///< Field class name.
	int32_t m_nfields; ///< Number of fields in this field group.
	const filtercheck_field_info* m_fields; ///< Array containing m_nfields field descriptions.
	uint32_t m_flags;
};

/*!
  \brief sinsp library exception.
*/
struct sinsp_exception : std::exception
{
	sinsp_exception()
	{
	}

	~sinsp_exception() throw()
	{
	}

	sinsp_exception(string error_str)
	{
		m_error_str = error_str;
	}

	char const* what() const throw()
	{
		return m_error_str.c_str();
	}

	string m_error_str;
};

/*!
  \brief sinsp library exception.
*/
struct sinsp_capture_interrupt_exception : sinsp_exception
{
};

/*!
  \brief The deafult way an event is converted to string by the library
*/
//#define DEFAULT_OUTPUT_STR "*%evt.num %evt.time %evt.cpu %proc.name (%thread.tid) %evt.dir %evt.type %evt.args"
#define DEFAULT_OUTPUT_STR "*%evt.time %evt.cpu %proc.name (%thread.tid) %evt.dir %evt.type %evt.args"

/** @defgroup inspector Main library
 @{
*/

/*!
  \brief System inspector class.
  This is the library entry point class. The functionality it exports includes:
  - live capture control (start/stop/pause...)
  - trace file management
  - event retrieval
  - setting capture filters
*/
class SINSP_PUBLIC sinsp
{
public:
	sinsp();
	~sinsp();

	/*!
	  \brief Start a live event capture.

	  \param timeout_ms the optional read timeout, i.e. the time after which a
	  call to \ref next() returns even if no events are available.

	  @throws a sinsp_exception containing the error string is thrown in case
	   of failure.
	*/
	void open(uint32_t timeout_ms = SCAP_TIMEOUT_MS);

	/*!
	  \brief Start an event capture from a trace file.

	  \param filename the trace file name.

	  @throws a sinsp_exception containing the error string is thrown in case
	   of failure.
	*/
	void open(string filename);

	/*!
	  \brief Ends a capture and release all resources.
	*/
	void close();

	/*!
	  \brief Get the next event from the open capture source

	  \param evt a \ref sinsp_evt pointer that will be initialized to point to
	  the next available event.

	  \return SCAP_SUCCESS if the call is succesful and pevent and pcpuid contain
	   valid data. SCAP_TIMEOUT in case the read timeout expired and no event is
	   available. SCAP_EOF when the end of an offline capture is reached.
	   On Failure, SCAP_FAILURE is returned and getlasterr() can be used to
	   obtain the cause of the error.

	  \note: the returned event can be considered valid only until the next
	   call to \ref next()
	*/
	int32_t next(OUT sinsp_evt** evt);

	/*!
	  \brief Get the number of events that have been captured and processed
	   since the call to \ref open()

	  \return the number of captured events.
	*/
	uint64_t get_num_events();

	/*!
	  \brief Set the capture snaplen, i.e. the maximum size an event
	  parameter can reach before the driver starts truncating it.

	  \param snaplen the snaplen for this capture instance, in bytes.

	  \note This function can only be called for live captures.
	  \note By default, the driver captures the first 80 bytes of the
	  buffers coming from events like read, write, send, recv, etc.
	  If you're not interested in payloads, smaller values will save
	  capture buffer space and make capture files smaller.
	  Conversely, big values should be used with care because they can
	  easily generate huge capture files.

	  @throws a sinsp_exception containing the error string is thrown in case
	   of failure.
	*/
	void set_snaplen(uint32_t snaplen);

	/*!
	  \brief Determine if this inspector is going to load user tables on
	  startup.

	  \param import_users if true, no user tables will be created for 
	  this capture. This also means that no user or group info will be 
	  written to the tracefile by the -w flag. The user/group tables are 
	  necessary to use filter fields like user.name or group.name. However, 
	  creating them can increase sysdig's startup time. Moreover, they contain
	  information that could be privacy sensitive.

	  \note default behavior is import_users=true.

	  @throws a sinsp_exception containing the error string is thrown in case
	   of failure.
	*/
	void set_import_users(bool import_users);

	/*!
	  \brief temporarily pauses event capture.

	  \note This function can only be called for live captures.
	*/
	void stop_capture();

	/*!
	  \brief Restarts an event capture that had been paused with
	   \ref stop_capture().

	  \note This function can only be called for live captures.
	*/
	void start_capture();

#ifdef HAS_FILTERING
	/*!
	  \brief Compiles and installs the given capture filter.

	  \param filter the filter string. Refer to the filtering language
	   section in the sysdig website for information about the filtering
	   syntax.

	  @throws a sinsp_exception containing the error string is thrown in case
	   the filter is invalid.
	*/
	void set_filter(const string& filter);

	/*!
	  \brief Return the filter set for this capture.

	  \return the filter previously set with \ref set_filter(), or an empty 
	   string if no filter has been set yet.
	*/
	const string get_filter();
#endif

	/*!
	  \brief This method can be used to specify a function to collect the library
	   log messages.

	  \param cb the target function that will receive the log messages.
	*/
	void set_log_callback(sinsp_logger_callback cb);

	/*!
	  \brief Specify the minimum severity of the messages that go into the logs
	   emitted by the library.
	*/
	void set_min_log_severity(sinsp_logger::severity sev);

	/*!
	  \brief Start writing the captured events to file.

	  \param dump_filename the destination trace file.

	  \param compress true to save the tracefile in a compressed format.

	  \note only the events that pass the capture filter set with \ref set_filter()
	   will be saved to disk.
	  \note this simplified dump interface allows only one dump per capture.
	   For more flexibility, refer to the \ref sinsp_dumper class, that can
	   also be combined with \ref sinsp_filter to filter what will go into
	   the file.

	  @throws a sinsp_exception containing the error string is thrown in case
	   of failure.
	*/
	void autodump_start(const string& dump_filename, bool compress);
 
 	/*!
	  \brief Cycles the file pointer to a new capture file
	*/
	void autodump_next_file();

	/*!
	  \brief Stops an event dump that was started with \ref autodump_start().

	  @throws a sinsp_exception containing the error string is thrown in case
	   of failure.
	*/
	void autodump_stop();

	/*!
	  \brief Populate the given vector with the full list of filter check fields
	   that this version of the library supports.
	*/
	static void get_filtercheck_fields_info(vector<const filter_check_info*>* list);

	bool has_metrics();

	/*!
	  \brief Return information about the machine generating the events.

	  \note this call works with file captures as well, because the machine
	   info is stored in the trace files. In that case, the returned
	   machine info is the one of the machine where the capture happened.
	*/
	const scap_machine_info* get_machine_info();

	/*!
	  \brief Look up a thread given its tid and return its information.

	  \param tid the ID of the thread. In case of multi-thread processes,
	   this corresponds to the PID.

	  \return the \ref sinsp_threadinfo object containing full thread information
	   and state.

	  \note if you are interested in a process' information, just give this
	  function with the PID of the process.

	  @throws a sinsp_exception containing the error string is thrown in case
	   of failure.
	*/
	sinsp_threadinfo* get_thread(int64_t tid);

	/*!
	  \brief Look up a thread given its tid and return its information,
	   and optionally go dig into proc if the thread is not in the thread table.

	  \param tid the ID of the thread. In case of multi-thread processes,
	   this corresponds to the PID.
	  \param query_os_if_not_found if true, the library will search for this
	   thread's information in proc, use the result to create a new thread
	   entry, and return the new entry.

	  \return the \ref sinsp_threadinfo object containing full thread information
	   and state.

	  \note if you are interested in a process' information, just give this
	  function with the PID of the process.

	  @throws a sinsp_exception containing the error string is thrown in case
	   of failure.
	*/
	sinsp_threadinfo* get_thread(int64_t tid, bool query_os_if_not_found, bool lookup_only);

	/*!
	  \brief Return the table with all the machine users.

	  \return a hash table with the user ID (UID) as the key and the user
	   information as the data.

	  \note this call works with file captures as well, because the user
	   table is stored in the trace files. In that case, the returned
	   user list is the one of the machine where the capture happened.
	*/
	const unordered_map<uint32_t, scap_userinfo*>* get_userlist();

	/*!
	  \brief Return the table with all the machine user groups.

	  \return a hash table with the group ID (GID) as the key and the group
	   information as the data.

	  \note this call works with file captures as well, because the group
	   table is stored in the trace files. In that case, the returned
	   user table is the one of the machine where the capture happened.
	*/
	const unordered_map<uint32_t, scap_groupinfo*>* get_grouplist();

	/*!
	  \brief Fill the given structure with statistics about the currently
	   open capture.

	  \note this call won't work on file captures.
	*/
	void get_capture_stats(scap_stats* stats);


#ifdef GATHER_INTERNAL_STATS
	sinsp_stats get_stats();
#endif

#ifdef HAS_ANALYZER
	sinsp_analyzer* m_analyzer;
#endif

	/*!
	  \brief Return the event and system call information tables.

	  This function exports the tables containing the information about the
	  events supported by the capture infrastructure and the available system calls.
	*/
	sinsp_evttables* get_event_info_tables();

	/*!
	  \brief get last library error.
	*/
	string getlasterr()
	{
		return m_lasterr;
	}

	/*!
	  \brief Add a new directory containing chisels.

	  \parame front_add if true, the chisel directory is added at the front of 
	   the search list and therefore gets priority.  

	  \note This function is not reentrant.
	*/
	void add_chisel_dir(string dirname, bool front_add);

	/*!
	  \brief Get the list of machine network interfaces.

	  \return Pointer to the iterface list manager.
	*/
	sinsp_network_interfaces* get_ifaddr_list();

	/*!
	  \brief Set the format used to render event data
	   buffer arguments.
	*/
	void set_buffer_format(sinsp_evt::param_fmt format);

	/*!
	  \brief Get the format used to render event data
	   buffer arguments.
	*/
	sinsp_evt::param_fmt get_buffer_format();

	/*!
	  \brief Returns true if the current capture is live.
	*/
	bool is_live();

	/*!
	  \brief Set the debugging mode of the inspector.

	  \param enable_debug when it is true and the current capture is live
	  the inspector filters out events about sysdig itself.
	*/
	void set_debug_mode(bool enable_debug);

	/*!
	  \brief Set the fatfile mode when writing events to file.

	  \note fatfile mode involves saving "hidden" events in the trace file 
	   that make it possible to preserve full state even when filters that
	   would drop state packets are used during the capture.
	*/
	void set_fatfile_dump_mode(bool enable_fatfile);

	/*!
	  \brief Sets the max length of event argument strings. 

	  \param len Max length after which an avent argument string is truncated.
	   0 means no limit. Use this to reduce verbosity when printing event info
	   on screen.
	*/
	void set_max_evt_output_len(uint32_t len);

	/*!
	  \brief Returns true if the debug mode is enabled.
	*/
	inline bool is_debug_enabled()
	{
		return m_isdebug_enabled;		
	}

        /*!
          \brief Set a flag indicating if the command line requested to show container information.

          \param set true if the command line arugment is set to show container information 
        */
        void set_print_container_data(bool print_container_data);


	/*!
	  \brief Returns true if the command line argument is set to show container information.
	*/
	inline bool is_print_container_data()
	{
		return m_print_container_data;
	}

	/*!
	  \brief Lets a filter plugin request a protocol decoder.

	  \param the name of the required decoder
	*/
	sinsp_protodecoder* require_protodecoder(string decoder_name);

	/*!
	  \brief Lets a filter plugin request a protocol decoder.

	  \param the name of the required decoder
	*/
	void protodecoder_register_reset(sinsp_protodecoder* dec);

	/*!
	  \brief If this is an offline capture, return the name of the file that is
	   being read, otherwise return an empty string.
	*/
	string get_input_filename()
	{
		return m_input_filename;
	}

	/*!
	  \brief XXX.
	*/
	double get_read_progress();

	//
	// Misc internal stuff
	//
	void stop_dropping_mode();
	void start_dropping_mode(uint32_t sampling_ratio);
	void on_new_entry_from_proc(void* context, int64_t tid, scap_threadinfo* tinfo, 
		scap_fdinfo* fdinfo, scap_t* newhandle);

	//
	// Allocates private state in the thread info class.
	// Returns the ID to use when retrieving the memory area.
	// Will fail if called after the capture starts.
	//
	uint32_t reserve_thread_memory(uint32_t size);

	sinsp_parser* get_parser();

	bool setup_cycle_writer(string base_file_name, int rollover_mb, int duration_seconds, int file_limit, bool do_cycle, bool compress);
	void import_ipv4_interface(const sinsp_ipv4_ifinfo& ifinfo);

VISIBILITY_PRIVATE

// Doxygen doesn't understand VISIBILITY_PRIVATE
#ifdef _DOXYGEN
private:
#endif

	void init();
	void import_thread_table();
	void import_ifaddr_list();
	void import_user_list();
	void add_protodecoders();

	void add_thread(const sinsp_threadinfo& ptinfo);
	void remove_thread(int64_t tid, bool force);
	//
	// Note: lookup_only should be used when the query for the thread is made
	//       not as a consequence of an event for that thread arriving, but for
	//       just for lookup reason. In that case, m_lastaccess_ts is not updated
	//       and m_last_tinfo is not set.
	//
	inline sinsp_threadinfo* find_thread(int64_t tid, bool lookup_only);
	// this is here for testing purposes only
	sinsp_threadinfo* find_thread_test(int64_t tid, bool lookup_only);
	bool remove_inactive_threads();

	scap_t* m_h;
	int64_t m_filesize;
	bool m_islive;
	string m_input_filename;
	bool m_isdebug_enabled;
	bool m_isfatfile_enabled;
	uint32_t m_max_evt_output_len;
	bool m_compress;
	sinsp_evt m_evt;
	string m_lasterr;
	int64_t m_tid_to_remove;
	int64_t m_tid_of_fd_to_remove;
	vector<int64_t>* m_fds_to_remove;
	uint64_t m_lastevent_ts;
	// the parsing engine
	sinsp_parser* m_parser;
	// the statistics analysis engine
	scap_dumper_t* m_dumper;
	const scap_machine_info* m_machine_info;
	uint32_t m_num_cpus;
	sinsp_thread_privatestate_manager m_thread_privatestate_manager;

	sinsp_network_interfaces* m_network_interfaces;

	sinsp_thread_manager* m_thread_manager;

	sinsp_container_manager m_container_manager;

        //
        // True if the command line argument is set to show container information
	// The deafult is false set within the constructor
        //
	bool m_print_container_data;

#ifdef HAS_FILTERING
	uint64_t m_firstevent_ts;
	sinsp_filter* m_filter;
	string m_filterstring;
#endif

	//
	// Internal stats
	//
#ifdef GATHER_INTERNAL_STATS
	sinsp_stats m_stats;
#endif
	uint32_t m_n_proc_lookups;
	uint32_t m_max_n_proc_lookups;
	uint32_t m_max_n_proc_socket_lookups;
#ifdef HAS_ANALYZER
	vector<uint64_t> m_tid_collisions;
#endif

	//
	// Saved snaplen
	//
	uint32_t m_snaplen;

	//
	// Some thread table limits
	//
	uint32_t m_max_thread_table_size;
	uint64_t m_thread_timeout_ns;
	uint64_t m_inactive_thread_scan_time_ns;

	//
	// Container limits
	//
	uint64_t m_inactive_container_scan_time_ns;

	//
	// How to render the data buffers
	//
	sinsp_evt::param_fmt m_buffer_format;

	//
	// User and group tables
	//
	bool m_import_users;
	unordered_map<uint32_t, scap_userinfo*> m_userlist;
	unordered_map<uint32_t, scap_groupinfo*> m_grouplist;

	//
	// The cycle-writer for files
	//
	cycle_writer* m_cycle_writer;
	bool m_write_cycling;

#ifdef SIMULATE_DROP_MODE
	//
	// Some dropping infrastructure
	//
	bool m_isdropping;
#endif

	//
	// Protocol decoding state
	//
	vector<sinsp_protodecoder*> m_decoders_reset_list;

	//
	// Meta event management
	//
	sinsp_evt m_meta_evt;
	char* m_meta_evt_buf;
	bool m_meta_evt_pending;

#if defined(HAS_CAPTURE)
	int64_t m_sysdig_pid;
#endif

	friend class sinsp_parser;
	friend class sinsp_analyzer;
	friend class sinsp_analyzer_parsers;
	friend class sinsp_evt;
	friend class sinsp_threadinfo;
	friend class sinsp_fdtable;
	friend class sinsp_thread_manager;
	friend class sinsp_container_manager;
	friend class sinsp_dumper;
	friend class sinsp_analyzer_fd_listener;
	friend class sinsp_chisel;
	friend class sinsp_protodecoder;
	friend class lua_cbacks;
	friend class sinsp_filter_check_container;
	friend class sinsp_worker;

	template<class TKey,class THash,class TCompare> friend class sinsp_connection_manager;
};

/*@}*/
