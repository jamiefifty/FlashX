#include <stdio.h>

#include "messaging.h"
#include "io_interface.h"
#include "slab_allocator.h"
#include "cache.h"

void *io_buf::get_buf() const
{
	if (is_page)
		return u.p->get_data();
	else
		return u.buf;
}

void io_buf::init(thread_safe_page *p)
{
	assert(p->get_ref() > 0);
	u.p = p;
	size = PAGE_SIZE;
	is_page = 1;
}

void io_request::init(char *buf, off_t off, ssize_t size,
		int access_method, io_interface *io, int node_id, int sync)
{
	assert(off <= MAX_FILE_SIZE);
	this->offset = off;
	this->io_addr = (long) io;
	if (is_extended_req()) {
		if (buf)
			add_buf(buf, size);
	}
	else if (payload_type == BASIC_REQ) {
		assert(size <= MAX_BUF_SIZE);
		this->buf_size = size;
		this->payload.buf_addr = buf;
	}
	else {
		assert(size <= MAX_BUF_SIZE);
		this->buf_size = size;
		this->payload.compute = NULL;
	}
	this->access_method = access_method & 0x1;
	// by default, a request is of high priority.
	this->high_prio = 1;
	assert(node_id <= MAX_NODE_ID);
	this->node_id = node_id;
	this->sync = sync;
}

int io_request::get_file_id() const
{
	assert(io_addr);
	int ret = get_io()->get_file_id();
	assert(ret >= 0);
	return ret;
}

void io_req_extension::add_io_buf(const io_buf &buf)
{
	if (num_bufs >= vec_capacity) {
		if (vec_pointer == embedded_vecs) {
			vec_capacity = MIN_NUM_ALLOC_IOVECS;
			vec_pointer = new io_buf[vec_capacity];
			memcpy(vec_pointer, embedded_vecs,
					sizeof(embedded_vecs[0]) * NUM_EMBEDDED_IOVECS);
		}
		else {
			vec_capacity *= 2;
			io_buf *tmp = new io_buf[vec_capacity];
			memcpy(tmp, vec_pointer,
					sizeof(vec_pointer[0]) * vec_capacity / 2);
			delete [] vec_pointer;
			vec_pointer = tmp;
		}
	}
	assert(num_bufs < vec_capacity);
	vec_pointer[num_bufs] = buf;
	num_bufs++;
}

void io_req_extension::add_buf(char *buf, int size, bool is_page)
{
	io_buf tmp;
	tmp.init(buf, size, is_page);
	add_io_buf(tmp);
}

void io_req_extension::add_buf_front(char *buf, int size, bool is_page)
{
	if (num_bufs >= vec_capacity) {
		if (vec_pointer == embedded_vecs) {
			vec_capacity = MIN_NUM_ALLOC_IOVECS;
			vec_pointer = new io_buf[vec_capacity];
			memcpy(vec_pointer + 1, embedded_vecs,
					sizeof(embedded_vecs[0]) * NUM_EMBEDDED_IOVECS);
		}
		else {
			vec_capacity *= 2;
			io_buf *tmp = new io_buf[vec_capacity];
			memcpy(tmp + 1, vec_pointer,
					sizeof(vec_pointer[0]) * vec_capacity / 2);
			delete [] vec_pointer;
			vec_pointer = tmp;
		}
	}
	else {
		memmove(vec_pointer + 1, vec_pointer,
				sizeof(vec_pointer[0]) * num_bufs);
	}
	assert(num_bufs < vec_capacity);
	vec_pointer[0].init((void *) buf, size, is_page);
	num_bufs++;
}

template<class T>
message<T>::message(slab_allocator *alloc, bool accept_inline)
{
	init();
	this->alloc = alloc;
	this->buf = alloc->alloc();
	assert(this->buf);
	this->accept_inline = accept_inline;
}

template<class T>
void message<T>::destroy()
{
	if (buf) {
		// we need to destroy the remaining objects in the message buffer
		// only when objects in the message aren't inline.
		if (!accept_inline) {
			while (has_next()) {
				T *obj = get_next_addr();
				obj->~T();
			}
		}
		alloc->free(buf);
	}
}

template<class T>
int message<T>::size() const
{
	return alloc->get_obj_size();
}

template<class T>
int thread_safe_msg_sender<T>::send_cached(T *msg, int num)
{
	int num_added = 0;
	// We expect the method is always successful.
	// so we try again and again until we succeed.
	while (true) {
		pthread_spin_lock(&_lock);
		int ret = buf.add(msg, num);
		pthread_spin_unlock(&_lock);
		msg += ret;
		num -= ret;
		num_added += ret;
		if (num == 0)
			return num_added;
		// If the buffer is full, we should flush the buffer.
		flush();
	}
}

// Send msgs to the destinatiion queue directly without caching.
template<class T>
int thread_safe_msg_sender<T>::send(T *msg, int num)
{
	// We should flush the msgs in the cache first.
	// but it won't flush all msgs in the cache.
	flush();

	int num_sent = 0;
	while (num > 0) {
		message<T> tmp(alloc, dest_queue->is_accept_inline());
		int ret = tmp.add(msg, num);
		msg += ret;
		num -= ret;
		num_sent += ret;

		// We need to make sure the message is added to the queue.
		// TODO we should add multiple messages together.
		while (dest_queue->add(&tmp, 1) <= 0);
	}

	return num_sent;
}

/**
 * these are to force to instantiate the templates
 * for io_request and io_reply.
 */
template class thread_safe_FIFO_queue<message<io_request> >;
template class thread_safe_FIFO_queue<message<io_reply> >;
template class blocking_FIFO_queue<message<io_request> >;
template class blocking_FIFO_queue<message<io_reply> >;
template class thread_safe_msg_sender<io_reply>;
template class message<io_request>;
template class message<io_reply>;

atomic_unsigned_integer io_req_extension::num_creates;
