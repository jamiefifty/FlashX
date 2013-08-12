#ifndef __IO_INTERFACE_H__
#define __IO_INTERFACE_H__

#include <stdlib.h>

#include <vector>

#include "exception.h"
#include "common.h"
#include "concurrency.h"

class io_request;

class callback
{
public:
	virtual int invoke(io_request *reqs[], int num) = 0;
};

class io_status
{
	long status: 8;
	long priv_data: 56;
public:
	io_status() {
		status = 0;
		priv_data = 0;
	}

	io_status(int status) {
		this->status = status;
		priv_data = 0;
	}

	void set_priv_data(long data) {
		priv_data = data;
	}

	long get_priv_data() const {
		return priv_data;
	}

	io_status &operator=(int status) {
		this->status = status;
		return *this;
	}

	bool operator==(int status) {
		return this->status == status;
	}
};

enum
{
	IO_OK,
	IO_PENDING = -1,
	IO_FAIL = -2,
	IO_UNSUPPORTED = -3,
};

/**
 * The interface for all IO classes.
 */
class io_interface
{
	int node_id;
	// This is an index for locating this IO object in a global table.
	int io_idx;
	static atomic_integer io_counter;
public:
	io_interface(int node_id) {
		this->node_id = node_id;
		this->io_idx = io_counter.inc(1) - 1;
	}

	virtual ~io_interface() { }

	/* When a thread begins, this method will be called. */
	virtual int init() {
		return 0;
	}

	int get_io_idx() const {
		return io_idx;
	}

	/**
	 * set the callback if the class supports the asynchronous fashion.
	 * If the class doesn't support async IO, return false.
	 */
	virtual bool set_callback(callback *cb) {
		return false;
	}

	virtual callback *get_callback() {
		return NULL;
	}

	virtual bool support_aio() {
		return false;
	}

	virtual void cleanup() {
	}

	/**
	 * The asynchronous IO interface
	 */

	/**
	 * The main interface to send requests.
	 */
	virtual void access(io_request *requests, int num, io_status *status = NULL) {
		throw unsupported_exception();
	}
	/**
	 * When requests are passed to the access method, an IO layer may buffer
	 * the requests. This method guarantees that all requests are flushed to
	 * the underlying devices.
	 */
	virtual void flush_requests() {
	}
	/**
	 * This method waits for at least the specified number of requests currently
	 * being sent by the access method to complete.
	 */
	virtual void wait4complete() {
	}

	/**
	 * The synchronous IO interface.
	 */
	virtual io_status access(char *, off_t, ssize_t, int) {
		return IO_UNSUPPORTED;
	}

	virtual void print_stat(int nthreads) {
	}

	virtual io_interface *clone() const {
		return NULL;
	}

	int get_node_id() const {
		return node_id;
	}
};

/**
 * The interface of creating IOs to access a file.
 */
class file_io_factory
{
	// The name of the file.
	const std::string name;
public:
	file_io_factory(const std::string _name): name(_name) {
	}

	const std::string &get_name() const {
		return name;
	}

	virtual io_interface *create_io(int node_id) = 0;
	virtual void destroy_io(io_interface *) = 0;
};

class RAID_config;
class cache_config;

std::vector<io_interface *> create_ios(const RAID_config &raid_conf,
		cache_config *cache_conf, const std::vector<int> &node_id_array,
		int nthreads, int access_option, long size, bool preload);
io_interface *allocate_io(const std::string &file_name, int node_id);
void release_io(io_interface *io);

file_io_factory *create_io_factory(const RAID_config &raid_conf,
		const std::vector<int> &node_id_array, const int access_option,
		const int io_depth = 0, const cache_config *cache_conf = NULL);

io_interface *get_io(int idx);

enum {
	READ_ACCESS,
	DIRECT_ACCESS,
	AIO_ACCESS,
	REMOTE_ACCESS,
	GLOBAL_CACHE_ACCESS,
	PART_GLOBAL_ACCESS,
};

void init_io_system(const RAID_config &raid_conf,
		const std::vector<int> &node_id_array);

// This interface is used for debugging.
void print_io_thread_stat();

#endif
