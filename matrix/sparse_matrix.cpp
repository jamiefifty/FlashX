/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashMatrix.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifdef PROFILER
#include <gperftools/profiler.h>
#endif

#include "sparse_matrix.h"
#include "matrix_io.h"
#include "matrix_config.h"
#include "hilbert_curve.h"
#include "mem_worker_thread.h"
#include "EM_dense_matrix.h"

namespace fm
{

matrix_config matrix_conf;

namespace detail
{

row_portions::row_portions()
{
	portion_size_log = 0;
	portion_mask = 0;
	num_cols = 0;
	entry_size = 0;
	tot_num_rows = 0;
}

row_portions::ptr row_portions::create(matrix_store::const_ptr mat)
{
	if (mat->is_wide()) {
		BOOST_LOG_TRIVIAL(error) << "the matrix needs to be tall-and-skinny";
		return row_portions::ptr();
	}

	row_portions::ptr ret(new row_portions());
	ret->portion_size_log = log2(mat->get_portion_size().first);
	ret->portion_mask = (1UL << ret->portion_size_log) - 1;
	ret->num_cols = mat->get_num_cols();
	ret->entry_size = mat->get_entry_size();
	ret->tot_num_rows = mat->get_num_rows();
	ret->portions.resize(mat->get_num_portions());
	ret->raw_portions.resize(mat->get_num_portions());
	for (size_t i = 0; i < ret->portions.size(); i++) {
		ret->portions[i] = mat->get_portion(i);
		if (ret->portions[i] == NULL) {
			BOOST_LOG_TRIVIAL(error) << "Can't get row portions";
			return row_portions::ptr();
		}
		ret->raw_portions[i] = ret->portions[i]->get_raw_arr();
		if (ret->raw_portions[i] == NULL) {
			BOOST_LOG_TRIVIAL(error)
				<< "Data in portions isn't stored contiguously";
			return row_portions::ptr();
		}
	}
	return ret;
}

const char *row_portions::get_rows(size_t start_row, size_t end_row) const
{
	size_t local_row_idx = start_row & portion_mask;
	size_t portion_idx = start_row >> portion_size_log;
	if (portion_idx != ((end_row - 1) >> portion_size_log)) {
		BOOST_LOG_TRIVIAL(error) << boost::format(
				"The required rows [%1%, %2%) aren't in the same portion")
			% start_row % end_row;
		return NULL;
	}
	else
		return raw_portions[portion_idx]
			+ local_row_idx * num_cols * entry_size;
}

/*
 * The minimum write I/O size (in bytes).
 */
static const size_t MIN_WRITE_SIZE = 4 * 1024 * 1024;

/*
 * This processes the blocks in the their original order.
 * It can process an arbitrary number of blocks.
 */
class seq_exec_order: public block_exec_order
{
public:
	virtual bool is_valid_size(size_t height, size_t width) const {
		return true;
	}

	virtual bool exec(block_compute_task &task,
			std::vector<const sparse_block_2d *> &blocks) const {
		BOOST_FOREACH(const sparse_block_2d *b, blocks)
			// We allow null blocks.
			if (b)
				task.run_on_block(*b);
		return true;
	}
};

/*
 * This class processes the blocks in the hilbert order.
 * The hilbert order gives us very high CPU cache hits regardless of
 * the CPU cache size. It's kind of like cache oblivious algorithms,
 * but this is not completely cache oblivious because it still relies on
 * a right block size to generate a reasonable amount of cache hits.
 */
class hilbert_exec_order: public block_exec_order
{
	struct coordinate_order
	{
		std::pair<off_t, off_t> coo;
		size_t order;
		bool operator<(const coordinate_order &o) const {
			return this->order < o.order;
		}
	};

	size_t n;
	std::vector<coordinate_order> hilbert_orders;
public:
	hilbert_exec_order(size_t n);

	virtual bool is_valid_size(size_t height, size_t width) const {
		return n == height && n == width;
	}

	virtual bool exec(block_compute_task &task,
			std::vector<const sparse_block_2d *> &blocks) const;
};

hilbert_exec_order::hilbert_exec_order(size_t n)
{
	this->n = n;
	for (size_t i = 0; i < n; i++) {
		for (size_t j = 0; j < n; j++) {
			coordinate_order o;
			o.coo.first = i;
			o.coo.second = j;
			o.order = hilbert_xy2d(n, i, j);
			hilbert_orders.push_back(o);
		}
	}
	assert(hilbert_orders.size() == n * n);
	// Order the coordinates ascendingly.
	std::sort(hilbert_orders.begin(), hilbert_orders.end());
}

/*
 * Process the blocks in the hilbert order.
 * Here I assume all blocks are in a square so there are n^2 blocks
 * in the vector. Furthermore, the blocks are organized in the row major
 * in the original matrix.
 */
bool hilbert_exec_order::exec(block_compute_task &task,
		std::vector<const sparse_block_2d *> &blocks) const
{
	if (blocks.size() != hilbert_orders.size()) {
		BOOST_LOG_TRIVIAL(error) << "The number of blocks need to be n^2";
		return false;
	}

	BOOST_FOREACH(coordinate_order o, hilbert_orders) {
		size_t row_idx = o.coo.first;
		size_t col_idx = o.coo.second;
		size_t idx = row_idx * n + col_idx;
		// We allow null blocks. There must be n^2 blocks in the vector.
		// If there are empty blocks in the square, we have to put a null
		// in the place to indicate the empty block.
		if (blocks[idx])
			task.run_on_block(*blocks[idx]);
	}
	return true;
}

bool EM_matrix_stream::filled_local_store::write(
		local_matrix_store::const_ptr portion,
		off_t global_start_row, off_t global_start_col)
{
	local_matrix_store::ptr part = data->get_portion(
			global_start_row - data->get_global_start_row(),
			global_start_col - data->get_global_start_col(),
			portion->get_num_rows(), portion->get_num_cols());
	part->copy_from(*portion);
	size_t ret = num_filled_rows.fetch_add(portion->get_num_rows());
	// I assume that no region is filled multiple times.
	return ret + portion->get_num_rows() == data->get_num_rows();
}

void EM_matrix_stream::write_async(local_matrix_store::const_ptr portion,
		off_t start_row, off_t start_col)
{
	assert(!mat->is_wide());
	assert(portion->get_num_cols() == mat->get_num_cols());
	const size_t CHUNK_SIZE = EM_matrix_store::CHUNK_SIZE;
	// If the portion is aligned with the default EM matrix portion size.
	if (!mat->is_wide() && start_row % CHUNK_SIZE == 0
			&& portion->get_num_rows() % CHUNK_SIZE == 0) {
		mat->write_portion_async(portion, start_row, start_col);
		return;
	}

	off_t EM_portion_row_start;
	EM_portion_row_start = (start_row / CHUNK_SIZE) * CHUNK_SIZE;
	// TODO we might want to a thread-safe hashtable.
	pthread_spin_lock(&lock);
	auto it = portion_bufs.find(EM_portion_row_start);
	filled_local_store::ptr buf;
	if (it == portion_bufs.end()) {
		size_t portion_num_rows = std::min(CHUNK_SIZE,
				mat->get_num_rows() - EM_portion_row_start);
		size_t num_bytes
			= portion_num_rows * portion->get_num_cols() * mat->get_type().get_size();
		// We don't want to allocate memory from the local memory buffers
		// because it's not clear which thread will destroy the raw array.
		local_raw_array arr(num_bytes, false);
		local_matrix_store::ptr tmp(
				new local_buf_row_matrix_store(arr, EM_portion_row_start,
					start_col, portion_num_rows, portion->get_num_cols(),
					mat->get_type(), -1));
		buf = filled_local_store::ptr(new filled_local_store(tmp));
		auto ret = portion_bufs.insert(std::pair<off_t, filled_local_store::ptr>(
					EM_portion_row_start, buf));
		assert(ret.second);
	}
	else
		buf = it->second;
	pthread_spin_unlock(&lock);
	assert(buf->get_global_start_row() == EM_portion_row_start);
	bool ret = buf->write(portion, start_row, start_col);
	// If we fill the buffer, we should flush it to disks.
	if (ret) {
		local_matrix_store::const_ptr data = buf->get_whole_portion();
		mat->write_portion_async(data, data->get_global_start_row(),
				data->get_global_start_col());
		pthread_spin_lock(&lock);
		portion_bufs.erase(EM_portion_row_start);
		pthread_spin_unlock(&lock);
	}
}

bool EM_matrix_stream::is_complete() const
{
	EM_matrix_stream *mutable_this = const_cast<EM_matrix_stream *>(this);
	pthread_spin_lock(&mutable_this->lock);
	bool ret = portion_bufs.empty();
	pthread_spin_unlock(&mutable_this->lock);
	return ret;
}

namespace
{
struct buf_deleter {
	void operator()(char *buf) const {
		free(buf);
	}
};
}

block_compute_task::block_compute_task(const matrix_io &_io,
		const sparse_matrix &mat, block_exec_order::ptr order): io(
			_io), block_size(mat.get_block_size())
{
	this->entry_size = mat.get_entry_size();
	size_t num_block_rows
		= ceil(((double) io.get_num_rows()) / block_size.get_num_rows());
	if (order->is_valid_size(num_block_rows, num_block_rows))
		this->exec_order = order;
	else
		this->exec_order = block_exec_order::ptr(new seq_exec_order());

	off_t orig_off = io.get_loc().get_offset();
	off = ROUND_PAGE(orig_off);
	real_io_size = ROUNDUP_PAGE(orig_off - off + io.get_size());
	buf = local_mem_buffer::get_irreg();
	// If there isn't a buffer available in the local thread or the local buffer
	// is smaller than required, we allocate a new buffer.
	// The smaller buffer will be deallocated if it exists.
	if (buf.second == NULL || buf.first < real_io_size) {
		std::shared_ptr<char> tmp((char *) valloc(real_io_size), buf_deleter());
		buf = local_mem_buffer::irreg_buf_t(real_io_size, tmp);
	}

	// The last entry in the vector indicates the end of the last block row.
	block_rows.resize(num_block_rows + 1);
	assert(io.get_top_left().get_row_idx() % block_size.get_num_rows() == 0);
	// The first block row.
	off_t block_row_idx
		= io.get_top_left().get_row_idx() / block_size.get_num_rows();
	std::vector<off_t> block_row_idxs(num_block_rows + 1);
	for (size_t i = 0; i < block_row_idxs.size(); i++)
		block_row_idxs[i] = block_row_idx + i;
	std::vector<off_t> block_row_offs;
	mat.get_block_row_offs(block_row_idxs, block_row_offs);
	assert(io.get_loc().get_offset() == block_row_offs[0]);
	for (size_t i = 0; i < num_block_rows; i++) {
		// The offset of the block row in the buffer.
		off_t local_off = block_row_offs[i] - off;
		block_rows[i] = buf.second.get() + local_off;
	}
	block_rows[num_block_rows]
		= buf.second.get() + block_row_offs[num_block_rows] - off;
}

block_compute_task::~block_compute_task()
{
	local_mem_buffer::cache_irreg(buf);
}

/*
 * A block compute task processes data in multiple block rows.
 * It's up to us in what order we should process the blocks in these block rows.
 */
void block_compute_task::run(char *buf, size_t size)
{
	off_t orig_off = io.get_loc().get_offset();
	off_t local_off = orig_off - ROUND_PAGE(orig_off);
	assert(local_off + io.get_size() <= size);
	size_t block_row_start
		= io.get_top_left().get_row_idx() / block_size.get_num_rows();
	size_t num_block_rows
		= ceil(((double) io.get_num_rows()) / block_size.get_num_rows());
	assert(io.get_top_left().get_col_idx() == 0);
	size_t block_col_start = 0;
	size_t num_block_cols
		= ceil(((double) io.get_num_cols()) / block_size.get_num_cols());

	// We access data in super blocks.
	// A super block is a set of blocks organized in a square or
	// a nearly square.
	// The last entry in the vector indicates the end of the last block row.
	size_t num_blocks = block_rows.size() - 1;
	std::vector<block_row_iterator> its;
	for (size_t i = 0; i < num_blocks; i++) {
		its.emplace_back((const sparse_block_2d *) block_rows[i],
				(const sparse_block_2d *) block_rows[i + 1]);
	}
	std::vector<const sparse_block_2d *> blocks(num_blocks * num_blocks);
	// The column index of a super block (in blocks).
	size_t sb_col_idx = 0;
	bool has_blocks;
	do {
		has_blocks = false;
		// Get a super block.
		// TODO this process might be computationally expensive itself.
		for (size_t i = 0; i < num_blocks; i++) {
			for (size_t j = 0; j < num_blocks; j++) {
				size_t idx = i * num_blocks + j;
				// If the block row doesn't have blocks left, we should fill
				// the corresponding location with NULL.
				if (!its[i].has_next()) {
					blocks[idx] = NULL;
					continue;
				}
				const sparse_block_2d &b = its[i].get_curr();
				assert(b.get_block_row_idx() >= block_row_start
						&& b.get_block_row_idx() < block_row_start + num_block_rows);
				assert(b.get_block_col_idx() >= block_col_start
						&& b.get_block_col_idx() < block_col_start + num_block_cols);
				assert(b.get_block_col_idx() >= sb_col_idx + j);
				if (b.get_block_col_idx() == sb_col_idx + j) {
					blocks[idx] = &b;
					its[i].next(entry_size);
				}
				else
					blocks[idx] = NULL;
			}
			// As long as there is a block left in a block row, we need to
			// go through the process again.
			has_blocks |= its[i].has_next();
		}
		sb_col_idx += num_blocks;
		exec_order->exec(*this, blocks);
	} while (has_blocks);
	// We have process the entire block rows.
	notify_complete();
}

block_spmm_task::block_spmm_task(row_portions::ptr in_row_portions,
		matrix_store &_output, EM_matrix_stream::ptr stream,
		const matrix_io &io, const sparse_matrix &mat,
		block_exec_order::ptr order): block_compute_task(
			io, mat, order), output(_output)
{
	this->in_row_portions = in_row_portions;
	this->output_stream = stream;
	entry_size = mat.get_entry_size();
	// We have to make sure the task processes the entire block rows.
	assert(io.get_num_cols() == mat.get_num_cols());
}

char *block_spmm_task::get_out_rows(size_t start_row, size_t num_rows)
{
	// out_part only needs to be initialized once because a task only
	// runs on certain block rows. 
	size_t block_row_start = get_io().get_top_left().get_row_idx();
	size_t block_num_rows = std::min(get_io().get_num_rows(),
			output.get_num_rows() - block_row_start);
	if (out_part == NULL) {
		// We maintain a local buffer for the corresponding part of
		// the output matrix.
		out_part = local_row_matrix_store::ptr(
				new local_buf_row_matrix_store(block_row_start, 0,
					block_num_rows, output.get_num_cols(), output.get_type(),
					// we allocate the buffer in the local node.
					-1));
		out_part->reset_data();
	}

	// Get the contiguous rows in the input and output matrices.
	size_t local_start = start_row - out_part->get_global_start_row();
	assert(local_start + num_rows <= out_part->get_num_rows());
	return out_part->get_raw_arr()
		+ local_start * out_part->get_num_cols() * out_part->get_entry_size();
}

void block_spmm_task::notify_complete()
{
	// It's possible that the entire block row is empty. In this case,
	// we didn't create out_part for the output portion. We need to reset
	// the data in the portion.
	if (out_part == NULL) {
		size_t block_row_start = get_io().get_top_left().get_row_idx();
		size_t block_num_rows = std::min(get_io().get_num_rows(),
				output.get_num_rows() - block_row_start);
		if (output.is_in_mem()) {
			local_matrix_store::ptr tmp = output.get_portion(
					block_row_start, 0, block_num_rows, output.get_num_cols());
			assert(tmp);
			tmp->reset_data();
		}
		else {
			local_matrix_store::ptr out_part(
					new local_buf_row_matrix_store(block_row_start, 0,
						block_num_rows, output.get_num_cols(), output.get_type(),
						// we allocate the buffer in the local node.
						-1));
			out_part->reset_data();
			output_stream->write_async(out_part, block_row_start, 0);
		}
	}
	else {
		if (output.is_in_mem()) {
			local_matrix_store::ptr tmp = output.get_portion(
					out_part->get_global_start_row(),
					out_part->get_global_start_col(), out_part->get_num_rows(),
					out_part->get_num_cols());
			assert(tmp);
			tmp->copy_from(*out_part);
		}
		else
			output_stream->write_async(out_part,
					out_part->get_global_start_row(),
					out_part->get_global_start_col());
	}
}

/*
 * This is shared by all threads.
 */
class spm_dispatcher: public task_dispatcher
{
	matrix_io_generator::ptr io_gen;
	EM_object::io_set::ptr ios;
	task_creator::ptr tcreator;
public:
	spm_dispatcher(matrix_io_generator::ptr io_gen,
			EM_object::io_set::ptr ios, task_creator::ptr tcreator) {
		this->io_gen = io_gen;
		this->ios = ios;
		this->tcreator = tcreator;
	}

	virtual bool issue_task();
};

bool spm_dispatcher::issue_task()
{
	matrix_io mio = io_gen->get_next_io();
	if (!mio.is_valid())
		return false;

	compute_task::ptr task = tcreator->create(mio);
	safs::io_request req = task->get_request();
	safs::io_interface &io = ios->get_curr_io();
	portion_callback &cb = static_cast<portion_callback &>(io.get_callback());
	cb.add(req, task);
	io.access(&req, 1);
	return true;
}

}

void sparse_matrix::compute(detail::task_creator::ptr creator,
		const detail::matrix_store &in) const
{
	// We might have kept the memory buffers to avoid the overhead of memory
	// allocation. We should delete them all before running SpMM.
	detail::local_mem_buffer::clear_bufs();
	detail::mem_thread_pool::ptr threads
		= detail::mem_thread_pool::get_global_mem_threads();
	int num_workers = threads->get_num_threads();
	matrix_io_generator::ptr io_gen = create_io_gen(in);
#ifdef PROFILER
	if (!matrix_conf.get_prof_file().empty())
		ProfilerStart(matrix_conf.get_prof_file().c_str());
#endif
	if (ios == NULL) {
		sparse_matrix *mutable_this = const_cast<sparse_matrix *>(this);
		mutable_this->ios = detail::EM_object::io_set::ptr(
				new detail::EM_object::io_set(get_io_factory()));
	}
	detail::spm_dispatcher::ptr dispatcher(new detail::spm_dispatcher(io_gen,
				ios, creator));
	for (int i = 0; i < num_workers; i++) {
		detail::io_worker_task *task = new detail::io_worker_task(dispatcher);
		const detail::EM_object *sp_obj = this;
		task->register_EM_obj(const_cast<detail::EM_object *>(sp_obj));
		std::vector<detail::EM_object *> em_objs = creator->get_EM_objs();
		for (size_t i = 0; i < em_objs.size(); i++)
			task->register_EM_obj(em_objs[i]);
		threads->process_task(i % threads->get_num_nodes(), task);
	}
	threads->wait4complete();
	assert(creator->is_complete());
#ifdef PROFILER
	if (!matrix_conf.get_prof_file().empty())
		ProfilerStop();
#endif
	detail::local_mem_buffer::clear_bufs();
}

///////////// The code for sparse matrix of the FlashGraph format //////////////

namespace detail
{

void fg_row_compute_task::run(char *buf, size_t size)
{
	assert(this->buf == buf);
	assert(this->buf_size == size);

	char *buf_p = buf + (io.get_loc().get_offset() - off);
	fg::ext_mem_undirected_vertex *v = (fg::ext_mem_undirected_vertex *) buf_p;
	for (size_t i = 0; i < io.get_num_rows(); i++) {
		size_t vsize = v->get_size();
		assert(buf_size >= vsize);
		buf_size -= vsize;
		buf_p += vsize;
		run_on_row(*v);
		v = (fg::ext_mem_undirected_vertex *) buf_p;
	}
}

/*
 * Sparse square symmetric matrix. It is partitioned in rows.
 */
class fg_sparse_sym_matrix: public sparse_matrix
{
	block_2d_size block_size;
	// This works like the index of the sparse matrix.
	std::vector<row_block> blocks;
	safs::file_io_factory::shared_ptr factory;

	fg_sparse_sym_matrix(safs::file_io_factory::shared_ptr factory,
			size_t nrows, const scalar_type *entry_type): sparse_matrix(
				nrows, entry_type, true) {
		this->factory = factory;
	}
public:
	static ptr create(fg::FG_graph::ptr, const scalar_type *entry_type);

	// Nothing should happen for a symmetric matrix.
	virtual sparse_matrix::ptr transpose() const {
		return sparse_matrix::ptr(new fg_sparse_sym_matrix(*this));
	}

	virtual safs::file_io_factory::shared_ptr get_io_factory() const {
		return factory;
	}

	virtual matrix_io_generator::ptr create_io_gen(
			const detail::matrix_store &in) const {
		assert(!in.is_wide());
		size_t num_rows = in.get_portion_size().first;
		size_t num_brows = std::min((size_t) matrix_conf.get_rb_io_size(),
				num_rows / matrix_conf.get_row_block_size());
		row_block_mapper mapper(blocks, num_brows);
		return matrix_io_generator::create(blocks, get_num_rows(),
				get_num_cols(), factory->get_file_id(), mapper);
	}

	virtual const block_2d_size &get_block_size() const {
		return block_size;
	}

	virtual void get_block_row_offs(const std::vector<off_t> &block_row_idxs,
			std::vector<off_t> &offs) const {
		throw unsupported_exception(
				"get_block_row_offs isn't supported in fg_sparse_sym_matrix");
	}

	virtual block_exec_order::ptr get_multiply_order(
			size_t num_block_rows, size_t num_block_cols) const {
		return block_exec_order::ptr(new seq_exec_order());
	}
};

sparse_matrix::ptr fg_sparse_sym_matrix::create(fg::FG_graph::ptr fg,
		const scalar_type *entry_type)
{
	// Initialize vertex index.
	fg::vertex_index::ptr index = fg->get_index_data();
	assert(index != NULL);
	assert(!index->get_graph_header().is_directed_graph());

	if (entry_type)
		assert(entry_type->get_size()
				== (size_t) index->get_graph_header().get_edge_data_size());
	fg::vsize_t num_vertices = index->get_num_vertices();
	fg_sparse_sym_matrix *m = new fg_sparse_sym_matrix(fg->get_graph_io_factory(
				safs::REMOTE_ACCESS), num_vertices, entry_type);

	// Generate the matrix index from the vertex index.
	if (index->is_compressed()) {
		fg::in_mem_cundirected_vertex_index::ptr uindex
			= fg::in_mem_cundirected_vertex_index::create(*index);
		for (size_t i = 0; i < num_vertices;
				i += matrix_conf.get_row_block_size()) {
			fg::vertex_offset off = uindex->get_vertex(i);
			m->blocks.emplace_back(off.get_off());
		}
		size_t graph_size = uindex->get_vertex(num_vertices - 1).get_off()
			+ uindex->get_size(num_vertices - 1);
		m->blocks.emplace_back(graph_size);
	}
	else {
		fg::undirected_vertex_index::ptr uindex
			= fg::undirected_vertex_index::cast(index);
		for (size_t i = 0; i < num_vertices;
				i += matrix_conf.get_row_block_size()) {
			fg::ext_mem_vertex_info info = uindex->get_vertex_info(i);
			m->blocks.emplace_back(info.get_off());
		}
		m->blocks.emplace_back(uindex->get_graph_size());
	}

	return sparse_matrix::ptr(m);
}

/*
 * Sparse asymmetric square matrix. It is partitioned in rows.
 */
class fg_sparse_asym_matrix: public sparse_matrix
{
	block_2d_size block_size;
	// These work like the index of the sparse matrix.
	// out_blocks index the original matrix.
	std::shared_ptr<std::vector<row_block> > out_blocks;
	// in_blocks index the transpose of the matrix.
	std::shared_ptr<std::vector<row_block> > in_blocks;
	safs::file_io_factory::shared_ptr factory;

	fg_sparse_asym_matrix(safs::file_io_factory::shared_ptr factory,
			size_t nrows, const scalar_type *entry_type): sparse_matrix(
				nrows, entry_type, false) {
		this->factory = factory;
		out_blocks = std::shared_ptr<std::vector<row_block> >(
				new std::vector<row_block>());
		in_blocks = std::shared_ptr<std::vector<row_block> >(
				new std::vector<row_block>());
	}
public:
	static ptr create(fg::FG_graph::ptr, const scalar_type *entry_type);

	virtual safs::file_io_factory::shared_ptr get_io_factory() const {
		return factory;
	}

	virtual sparse_matrix::ptr transpose() const {
		fg_sparse_asym_matrix *ret = new fg_sparse_asym_matrix(*this);
		ret->sparse_matrix::_transpose();
		ret->out_blocks = this->in_blocks;
		ret->in_blocks = this->out_blocks;
		return sparse_matrix::ptr(ret);
	}

	virtual matrix_io_generator::ptr create_io_gen(
			const detail::matrix_store &in) const {
		assert(!in.is_wide());
		size_t num_rows = in.get_portion_size().first;
		size_t num_brows = std::min((size_t) matrix_conf.get_rb_io_size(),
				num_rows / matrix_conf.get_row_block_size());
		row_block_mapper mapper(*out_blocks, num_brows);
		return matrix_io_generator::create(*out_blocks, get_num_rows(),
				get_num_cols(), factory->get_file_id(), mapper);
	}

	virtual const block_2d_size &get_block_size() const {
		return block_size;
	}

	virtual void get_block_row_offs(const std::vector<off_t> &block_row_idxs,
			std::vector<off_t> &offs) const {
		throw unsupported_exception(
				"get_block_row_offs isn't supported in fg_sparse_asym_matrix");
	}

	virtual block_exec_order::ptr get_multiply_order(
			size_t num_block_rows, size_t num_block_cols) const {
		return block_exec_order::ptr(new seq_exec_order());
	}
};

sparse_matrix::ptr fg_sparse_asym_matrix::create(fg::FG_graph::ptr fg,
		const scalar_type *entry_type)
{
	// Initialize vertex index.
	fg::vertex_index::ptr index = fg->get_index_data();
	assert(index != NULL);
	assert(index->get_graph_header().is_directed_graph());

	if (entry_type)
		assert(entry_type->get_size()
				== (size_t) index->get_graph_header().get_edge_data_size());
	fg::vsize_t num_vertices = index->get_num_vertices();
	fg_sparse_asym_matrix *m = new fg_sparse_asym_matrix(fg->get_graph_io_factory(
				safs::REMOTE_ACCESS), num_vertices, entry_type);

	if (index->is_compressed()) {
		fg::in_mem_cdirected_vertex_index::ptr dindex
			= fg::in_mem_cdirected_vertex_index::create(*index);
		for (size_t i = 0; i < num_vertices;
				i += matrix_conf.get_row_block_size()) {
			fg::directed_vertex_entry ventry = dindex->get_vertex(i);
			m->out_blocks->emplace_back(ventry.get_out_off());
			m->in_blocks->emplace_back(ventry.get_in_off());
		}
		fg::directed_vertex_entry ventry = dindex->get_vertex(num_vertices - 1);
		m->out_blocks->emplace_back(ventry.get_out_off()
				+ dindex->get_out_size(num_vertices - 1));
		m->in_blocks->emplace_back(ventry.get_in_off()
				+ dindex->get_in_size(num_vertices - 1));
	}
	else {
		fg::directed_vertex_index::ptr dindex
			= fg::directed_vertex_index::cast(index);
		// Generate the matrix index from the vertex index.
		for (size_t i = 0; i < num_vertices;
				i += matrix_conf.get_row_block_size()) {
			fg::ext_mem_vertex_info info = dindex->get_vertex_info_out(i);
			m->out_blocks->emplace_back(info.get_off());

			info = dindex->get_vertex_info_in(i);
			m->in_blocks->emplace_back(info.get_off());
		}
		fg::ext_mem_vertex_info info
			= dindex->get_vertex_info_out(num_vertices - 1);
		m->out_blocks->emplace_back(info.get_off() + info.get_size());
		info = dindex->get_vertex_info_in(num_vertices - 1);
		m->in_blocks->emplace_back(info.get_off() + info.get_size());
	}

	return sparse_matrix::ptr(m);
}

}

sparse_matrix::ptr sparse_matrix::create(fg::FG_graph::ptr fg,
		const scalar_type *entry_type)
{
	const fg::graph_header &header = fg->get_graph_header();
	if (header.is_directed_graph())
		return detail::fg_sparse_asym_matrix::create(fg, entry_type);
	else
		return detail::fg_sparse_sym_matrix::create(fg, entry_type);
}

/////////////// The code for native 2D-partitioned sparse matrix ///////////////

namespace detail
{

class block_sparse_matrix: public sparse_matrix
{
	// If the matrix is stored in the native format and is partitioned
	// in two dimensions, we need to know the block size.
	block_2d_size block_size;
	SpM_2d_index::ptr index;
	safs::file_io_factory::shared_ptr factory;
public:
	block_sparse_matrix(SpM_2d_index::ptr index,
			SpM_2d_storage::ptr mat): sparse_matrix(
				index->get_header().get_num_rows(),
				index->get_header().get_num_cols(),
				&index->get_header().get_data_type(), true), block_size(
				index->get_header().get_2d_block_size()) {
		this->index = index;
		factory = mat->create_io_factory();
	}

	block_sparse_matrix(SpM_2d_index::ptr index,
			safs::file_io_factory::shared_ptr factory): sparse_matrix(
				index->get_header().get_num_rows(),
				index->get_header().get_num_cols(),
				&index->get_header().get_data_type(), true), block_size(
				index->get_header().get_2d_block_size()) {
		this->index = index;
		this->factory = factory;
	}

	virtual safs::file_io_factory::shared_ptr get_io_factory() const {
		return factory;
	}

	// Nothing should happen for a symmetric matrix.
	virtual sparse_matrix::ptr transpose() const {
		return sparse_matrix::ptr(new block_sparse_matrix(*this));
	}

	virtual matrix_io_generator::ptr create_io_gen(
			const detail::matrix_store &in) const {
		// For 2D-partitioned sparse matrix, we access blocks in super blocks.
		// The super block size should be CPU-cache friendly as well as I/O
		// friendly. That is, the super block size should be large enough to
		// fill the rows from the dense matrices involed in the computation
		// should fill the entire CPU cache. If the output matrix is written
		// to disks, the super block should also be large enough so that
		// each write to disks is large enough to have high I/O throughput.
		size_t sblock_size = detail::cal_super_block_size(get_block_size(),
				in.get_entry_size() * in.get_num_cols());
		return matrix_io_generator::create(index, factory->get_file_id(),
				sblock_size, 1);
	}

	virtual const block_2d_size &get_block_size() const {
		return block_size;
	}

	virtual void get_block_row_offs(const std::vector<off_t> &block_row_idxs,
			std::vector<off_t> &offs) const {
		offs.resize(block_row_idxs.size());
		for (size_t i = 0; i < block_row_idxs.size(); i++)
			offs[i] = index->get_block_row_off(block_row_idxs[i]);
	}

	virtual block_exec_order::ptr get_multiply_order(
			size_t num_block_rows, size_t num_block_cols) const {
		if (num_block_rows != num_block_cols) {
			BOOST_LOG_TRIVIAL(error) << "hilbert order requires a square.";
			return block_exec_order::ptr();
		}
		double log2_nbr = log2(num_block_rows);
		if (log2_nbr != floor(log2_nbr)) {
			BOOST_LOG_TRIVIAL(error)
				<< "hilbert order requires a dimension of 2^n";
			return block_exec_order::ptr();
		}
		if (matrix_conf.use_hilbert_order())
			return block_exec_order::ptr(new hilbert_exec_order(num_block_rows));
		else
			return block_exec_order::ptr(new seq_exec_order());
	}
};

class block_sparse_asym_matrix: public sparse_matrix
{
	// If the matrix is stored in the native format and is partitioned
	// in two dimensions, we need to know the block size.
	block_2d_size block_size;
	block_sparse_matrix::ptr mat;
	block_sparse_matrix::ptr t_mat;
public:
	block_sparse_asym_matrix(SpM_2d_index::ptr index, SpM_2d_storage::ptr mat,
			SpM_2d_index::ptr t_index, SpM_2d_storage::ptr t_mat): sparse_matrix(
				index->get_header().get_num_rows(),
				index->get_header().get_num_cols(),
				&index->get_header().get_data_type(), false), block_size(
				index->get_header().get_2d_block_size()) {
		this->mat = block_sparse_matrix::ptr(new block_sparse_matrix(index, mat));
		this->t_mat = block_sparse_matrix::ptr(new block_sparse_matrix(t_index,
					t_mat));
	}

	block_sparse_asym_matrix(SpM_2d_index::ptr index,
			safs::file_io_factory::shared_ptr mat_io_fac,
			SpM_2d_index::ptr t_index,
			safs::file_io_factory::shared_ptr t_mat_io_fac): sparse_matrix(
				index->get_header().get_num_rows(),
				index->get_header().get_num_cols(),
				&index->get_header().get_data_type(), false), block_size(
				index->get_header().get_2d_block_size()) {
		this->mat = block_sparse_matrix::ptr(new block_sparse_matrix(index, mat_io_fac));
		this->t_mat = block_sparse_matrix::ptr(new block_sparse_matrix(t_index,
					t_mat_io_fac));
	}

	virtual safs::file_io_factory::shared_ptr get_io_factory() const {
		return mat->get_io_factory();
	}

	// Nothing should happen for a symmetric matrix.
	virtual sparse_matrix::ptr transpose() const {
		block_sparse_asym_matrix *ret = new block_sparse_asym_matrix(*this);
		ret->mat = this->t_mat;
		ret->t_mat = this->mat;
		ret->sparse_matrix::_transpose();
		ret->reset_ios();
		return sparse_matrix::ptr(ret);
	}

	virtual matrix_io_generator::ptr create_io_gen(
			const detail::matrix_store &in) const {
		return mat->create_io_gen(in);
	}

	virtual const block_2d_size &get_block_size() const {
		return block_size;
	}

	virtual void get_block_row_offs(const std::vector<off_t> &block_row_idxs,
			std::vector<off_t> &offs) const {
		mat->get_block_row_offs(block_row_idxs, offs);
	}

	virtual block_exec_order::ptr get_multiply_order(
			size_t num_block_rows, size_t num_block_cols) const {
		return mat->get_multiply_order(num_block_rows, num_block_cols);
	}
};

}

sparse_matrix::ptr sparse_matrix::create(SpM_2d_index::ptr index,
		SpM_2d_storage::ptr mat)
{
	return sparse_matrix::ptr(new detail::block_sparse_matrix(index, mat));
}

sparse_matrix::ptr sparse_matrix::create(SpM_2d_index::ptr index,
		SpM_2d_storage::ptr mat, SpM_2d_index::ptr t_index,
		SpM_2d_storage::ptr t_mat)
{
	return sparse_matrix::ptr(new detail::block_sparse_asym_matrix(index, mat,
				t_index, t_mat));
}

sparse_matrix::ptr sparse_matrix::create(SpM_2d_index::ptr index,
			safs::file_io_factory::shared_ptr mat_io_fac)
{
	return sparse_matrix::ptr(new detail::block_sparse_matrix(index,
				mat_io_fac));
}

sparse_matrix::ptr sparse_matrix::create(SpM_2d_index::ptr index,
			safs::file_io_factory::shared_ptr mat_io_fac,
			SpM_2d_index::ptr t_index,
			safs::file_io_factory::shared_ptr t_mat_io_fac)
{
	return sparse_matrix::ptr(new detail::block_sparse_asym_matrix(index,
				mat_io_fac, t_index, t_mat_io_fac));
}

bool sparse_matrix::multiply(detail::matrix_store::const_ptr in,
		detail::matrix_store::ptr out, detail::task_creator::ptr create) const
{
	if (in->get_num_rows() != ncols
			|| in->get_num_cols() != out->get_num_cols()
			|| out->get_num_rows() != this->get_num_rows()) {
		BOOST_LOG_TRIVIAL(error) <<
			"the input and output matrix have incompatible dimensions";
		return false;
	}

	dense_matrix::ptr in_tmp;
	if (in->store_layout() != matrix_layout_t::L_ROW) {
		in_tmp = dense_matrix::create(in);
		in_tmp = in_tmp->conv2(matrix_layout_t::L_ROW);
	}
	if (!in->is_in_mem()) {
		if (in_tmp == NULL)
			in_tmp = dense_matrix::create(in);
		in_tmp = in_tmp->conv_store(true, matrix_conf.get_num_nodes());
	}

	if (in_tmp) {
		in_tmp->materialize_self();
		in = in_tmp->get_raw_store();
	}
	bool ret = create->set_data(in, out);
	if (ret)
		compute(create, *in);
	return ret;
}

std::vector<safs::io_interface::ptr> sparse_matrix::create_ios() const
{
	std::vector<safs::io_interface::ptr> ret(1);
	assert(ios);
	ret[0] = ios->create_io();
	return ret;
}

static std::atomic<long> init_count;

void init_flash_matrix(config_map::ptr configs)
{
	size_t count = init_count.fetch_add(1);
	if (count == 0) {
		// We should initialize SAFS first.
		try {
			safs::init_io_system(configs);
		} catch (std::exception &e) {
			BOOST_LOG_TRIVIAL(warning)
				<< "FlashMatrix: fail to initialize SAFS";
		}

		if (configs)
			matrix_conf.init(configs);
		size_t num_nodes = matrix_conf.get_num_nodes();
		size_t num_threads = matrix_conf.get_num_DM_threads();
		detail::local_mem_buffer::init();
		detail::mem_thread_pool::init_global_mem_threads(num_nodes,
				num_threads / num_nodes);
		detail::init_memchunk_reserve(num_nodes);
	}
}

void destroy_flash_matrix()
{
	long count = init_count.fetch_sub(1);
	assert(count > 0);
	if (count == 1) {
		detail::destroy_memchunk_reserve();
		detail::local_mem_buffer::destroy();
		detail::mem_thread_pool::destroy();
		safs::destroy_io_system();
	}
}

}
