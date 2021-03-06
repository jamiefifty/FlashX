/*
 * Copyright 2015 Open Connectome Project (http://openconnecto.me)
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

#include "mapply_matrix_store.h"
#include "local_matrix_store.h"
#include "vec_store.h"
#include "local_mem_buffer.h"
#include "dense_matrix.h"

namespace fm
{

namespace detail
{

namespace
{

size_t SUB_CHUNK_SIZE = 4 * 1024;

class mapply_store
{
	materialize_level mater_level;
	std::vector<local_matrix_store::const_ptr> ins;
	const portion_mapply_op &op;
	// This stores the materialized result for the whole portion.
	// If it exists, we always get data from it.
	local_matrix_store::ptr whole_res;
	// This stores the materialized results for individual sub chunks.
	std::vector<local_matrix_store::ptr> res_bufs;
	size_t num_materialized_parts;
	local_matrix_store *lstore;

	// The information of the original portion.
	bool is_wide;
	size_t orig_global_start_row;
	size_t orig_global_start_col;
	size_t orig_num_rows;
	size_t orig_num_cols;

	bool is_right_part(const local_matrix_store &part) const {
		return part.get_global_start_row() == lstore->get_global_start_row()
			&& part.get_global_start_col() == lstore->get_global_start_col()
			&& part.get_num_rows() == lstore->get_num_rows()
			&& part.get_num_cols() == lstore->get_num_cols();
	}

	off_t get_part_idx() const {
		off_t part_idx;
		if (is_wide)
			part_idx = lstore->get_local_start_col() / SUB_CHUNK_SIZE;
		else
			part_idx = lstore->get_local_start_row() / SUB_CHUNK_SIZE;
		return part_idx;
	}
public:
	mapply_store(const std::vector<local_matrix_store::const_ptr> &ins,
			const portion_mapply_op &_op, local_matrix_store *lstore,
			materialize_level mater_level): op(_op) {
		this->mater_level = mater_level;
		this->lstore = lstore;
		this->ins = ins;

		is_wide = lstore->is_wide();
		orig_global_start_row = lstore->get_global_start_row();
		orig_global_start_col = lstore->get_global_start_col();
		orig_num_rows = lstore->get_num_rows();
		orig_num_cols = lstore->get_num_cols();

		size_t num_parts;
		if (is_wide)
			num_parts = ceil(((double) orig_num_cols) / SUB_CHUNK_SIZE);
		else
			num_parts = ceil(((double) orig_num_rows) / SUB_CHUNK_SIZE);
		res_bufs.resize(num_parts);
		num_materialized_parts = 0;
	}

	const char *get_raw_arr() const {
		const char *ret = NULL;
		if (whole_res)
			ret = whole_res->get_raw_arr();
		else if (!lstore->is_whole() && res_bufs[get_part_idx()]
				&& is_right_part(*res_bufs[get_part_idx()]))
			ret = res_bufs[get_part_idx()]->get_raw_arr();
		// If the materialization level is CPU cache, the materialized data is
		// stored in the first buffer.
		else if (mater_level == materialize_level::MATER_CPU
				&& !lstore->is_whole() && res_bufs[0]
				&& is_right_part(*res_bufs[0])) {
			ret = res_bufs[0]->get_raw_arr();
		}
		assert(ret);
		return ret;
	}

	const local_matrix_store &get_materialized_res() const {
		// No matter we are in the resized subchunk or in the whole portion,
		// whole_res always has the right data if whole_res exists.
		if (whole_res)
			return *whole_res;
		else if (!lstore->is_whole() && res_bufs[get_part_idx()]
				&& is_right_part(*res_bufs[get_part_idx()]))
			return *res_bufs[get_part_idx()];
		else {
			assert(mater_level == materialize_level::MATER_CPU
					&& !lstore->is_whole() && res_bufs[0]
					&& is_right_part(*res_bufs[0]));
			return *res_bufs[0];
		}
	}

	bool is_materialized() const {
		// If the whole portion is materialized, it always returns true.
		if (whole_res
				// If the current part is materialized.
				|| (!lstore->is_whole() && res_bufs[get_part_idx()]
					&& is_right_part(*res_bufs[get_part_idx()]))
				// If we materialize the current part in CPU cache.
				|| (mater_level == materialize_level::MATER_CPU
					&& !lstore->is_whole() && res_bufs[0]
					&& is_right_part(*res_bufs[0])))
			return true;
		else
			return false;
	}
	void materialize() const;
	void materialize_whole();

	void resize(off_t local_start_row, off_t local_start_col,
			size_t local_num_rows, size_t local_num_cols);
	void reset_size();
};

void mapply_store::reset_size()
{
	for (size_t i = 0; i < ins.size(); i++)
		const_cast<local_matrix_store *>(ins[i].get())->reset_size();

	if (whole_res)
		whole_res->reset_size();
}

void mapply_store::resize(off_t local_start_row, off_t local_start_col,
		size_t local_num_rows, size_t local_num_cols)
{
	if (is_wide) {
		assert(local_start_row == 0 && local_num_rows == lstore->get_num_rows());
		for (size_t i = 0; i < ins.size(); i++)
			const_cast<local_matrix_store *>(ins[i].get())->resize(0,
					local_start_col, ins[i]->get_num_rows(), local_num_cols);
	}
	else {
		assert(local_start_col == 0 && local_num_cols == lstore->get_num_cols());
		for (size_t i = 0; i < ins.size(); i++)
			const_cast<local_matrix_store *>(ins[i].get())->resize(
					local_start_row, 0, local_num_rows, ins[i]->get_num_cols());
	}
	if (whole_res)
		whole_res->resize(local_start_row, local_start_col, local_num_rows,
				local_num_cols);
}

/*
 * This is to materialize the entire portion of a mapply store.
 * When there are a chain of mapply operations, we need to break a mapply
 * portion into smaller portions, so that data in the chain can be retained
 * in the CPU cache as much as possible.
 */
void mapply_store::materialize_whole()
{
	if (whole_res)
		return;

	if (lstore->store_layout() == matrix_layout_t::L_COL) {
		local_buf_col_matrix_store *tmp = new local_buf_col_matrix_store(
				lstore->get_global_start_row(),
				lstore->get_global_start_col(), lstore->get_num_rows(),
				lstore->get_num_cols(), lstore->get_type(), -1);
		whole_res = local_matrix_store::ptr(tmp);
	}
	else {
		local_buf_row_matrix_store *tmp = new local_buf_row_matrix_store(
				lstore->get_global_start_row(),
				lstore->get_global_start_col(), lstore->get_num_rows(),
				lstore->get_num_cols(), lstore->get_type(), -1);
		whole_res = local_matrix_store::ptr(tmp);
	}

	// If all parts in the local matrix store have been materialized,
	// we can merge them.
	if (num_materialized_parts == res_bufs.size() && res_bufs[0]) {
		for (size_t i = 0; i < res_bufs.size(); i++)
			assert(res_bufs[i]);

		// Copy all the parts
		if (is_wide) {
			for (size_t local_start_col = 0; local_start_col < orig_num_cols;
					local_start_col += SUB_CHUNK_SIZE) {
				size_t local_num_cols = std::min(SUB_CHUNK_SIZE,
						orig_num_cols - local_start_col);
				whole_res->resize(0, local_start_col, whole_res->get_num_rows(),
						local_num_cols);
				off_t part_idx = local_start_col / SUB_CHUNK_SIZE;
				assert(res_bufs[part_idx]);
				whole_res->copy_from(*res_bufs[part_idx]);
			}
		}
		else {
			// If this is a tall matrix.
			for (size_t local_start_row = 0; local_start_row < orig_num_rows;
					local_start_row += SUB_CHUNK_SIZE) {
				size_t local_num_rows = std::min(SUB_CHUNK_SIZE,
						orig_num_rows - local_start_row);
				whole_res->resize(local_start_row, 0, local_num_rows,
						whole_res->get_num_cols());
				off_t part_idx = local_start_row / SUB_CHUNK_SIZE;
				assert(res_bufs[part_idx]);
				whole_res->copy_from(*res_bufs[part_idx]);
			}
		}
		whole_res->reset_size();
	}
	else {
		std::vector<local_matrix_store *> mutable_ins(ins.size());
		for (size_t i = 0; i < ins.size(); i++)
			mutable_ins[i] = const_cast<local_matrix_store *>(ins[i].get());

		if (is_wide) {
			for (size_t local_start_col = 0; local_start_col < lstore->get_num_cols();
					local_start_col += SUB_CHUNK_SIZE) {
				size_t local_num_cols = std::min(SUB_CHUNK_SIZE,
						lstore->get_num_cols() - local_start_col);
				for (size_t i = 0; i < mutable_ins.size(); i++) {
					mutable_ins[i]->resize(0, local_start_col,
							mutable_ins[i]->get_num_rows(), local_num_cols);
				}
				whole_res->resize(0, local_start_col, whole_res->get_num_rows(),
						local_num_cols);
				op.run(ins, *whole_res);
			}
		}
		else {
			// If this is a tall matrix.
			for (size_t local_start_row = 0; local_start_row < lstore->get_num_rows();
					local_start_row += SUB_CHUNK_SIZE) {
				size_t local_num_rows = std::min(SUB_CHUNK_SIZE,
						lstore->get_num_rows() - local_start_row);
				for (size_t i = 0; i < mutable_ins.size(); i++) {
					mutable_ins[i]->resize(local_start_row, 0, local_num_rows,
							mutable_ins[i]->get_num_cols());
				}
				whole_res->resize(local_start_row, 0, local_num_rows,
						whole_res->get_num_cols());
				op.run(ins, *whole_res);
			}
		}
		for (size_t i = 0; i < mutable_ins.size(); i++)
			mutable_ins[i]->reset_size();
		whole_res->reset_size();
		num_materialized_parts = res_bufs.size();
	}

	// We can clean up all the parts now if there is any. We'll always access
	// data from `whole_res' from now on.
	for (size_t i = 0; i < res_bufs.size(); i++)
		res_bufs[i] = NULL;
	// We don't need the input matrix portions any more.
	ins.clear();
}

void mapply_store::materialize() const
{
	mapply_store *mutable_this = const_cast<mapply_store *>(this);
	if (lstore->is_whole()) {
		mutable_this->materialize_whole();
		return;
	}

	// If we have materialized the whole portion.
	if (whole_res)
		return;

	// If we have materialized the part
	off_t part_idx = get_part_idx();
	if (res_bufs[part_idx] && is_right_part(*res_bufs[part_idx]))
		return;

	// Materialize the part.
	local_matrix_store::ptr part;
	if (lstore->store_layout() == matrix_layout_t::L_COL)
		part = local_matrix_store::ptr(
				new local_buf_col_matrix_store(lstore->get_global_start_row(),
					lstore->get_global_start_col(), lstore->get_num_rows(),
					lstore->get_num_cols(), lstore->get_type(), -1));
	else
		part = local_matrix_store::ptr(
				new local_buf_row_matrix_store(lstore->get_global_start_row(),
					lstore->get_global_start_col(), lstore->get_num_rows(),
					lstore->get_num_cols(), lstore->get_type(), -1));
	op.run(ins, *part);
	// If the materialization level is CPU cache.
	if (mater_level == materialize_level::MATER_CPU)
		mutable_this->res_bufs[0] = part;
	else {
		mutable_this->num_materialized_parts++;
		mutable_this->res_bufs[part_idx] = part;
	}

	if (num_materialized_parts == res_bufs.size())
		// We don't need the input matrix portions any more.
		mutable_this->ins.clear();
}

/*
 * This portion compute has two functions.
 * It collects multiple portions if the mapply store is on top of multiple
 * matrix stores. It can also collect multiple user's portion computation
 * if multiple users need the portion.
 */
class collect_portion_compute: public portion_compute
{
	size_t num_EM_parts;
	std::vector<portion_compute::ptr> orig_computes;
	local_matrix_store::ptr res;
	size_t num_reads;
public:
	typedef std::shared_ptr<collect_portion_compute> ptr;

	collect_portion_compute(portion_compute::ptr orig_compute) {
		this->num_EM_parts = 0;
		this->num_reads = 0;
		this->orig_computes.push_back(orig_compute);
	}

	size_t get_num_EM_parts() const {
		return num_EM_parts;
	}

	void add_EM_part(local_matrix_store::const_ptr part) {
		num_EM_parts++;
	}

	void add_orig_compute(portion_compute::ptr compute) {
		// When the portion compute is created, the user must have provide
		// his portion compute.
		assert(orig_computes.size() > 0);
		orig_computes.push_back(compute);
	}

	void set_res_part(local_matrix_store::ptr res) {
		this->res = res;
	}

	virtual void run(char *buf, size_t size);
};

void collect_portion_compute::run(char *buf, size_t size)
{
	num_reads++;
	if (num_reads == num_EM_parts) {
		size_t num_eles = res->get_num_rows() * res->get_num_cols();
		for (size_t i = 0; i < orig_computes.size(); i++)
			orig_computes[i]->run(res->get_raw_arr(),
					num_eles * res->get_entry_size());
		// This only runs once.
		// Let's remove all user's portion compute to indicate that it has
		// been invoked.
		orig_computes.clear();
		// We don't need to reference the result matrix portion any more.
		res = NULL;
	}
}

class lmapply_col_matrix_store: public lvirtual_col_matrix_store
{
	std::weak_ptr<collect_portion_compute> collect_compute;
	mapply_store store;
public:
	lmapply_col_matrix_store(materialize_level mater_level,
			const std::vector<local_matrix_store::const_ptr> &ins,
			const portion_mapply_op &op,
			collect_portion_compute::ptr collect_compute,
			off_t global_start_row, off_t global_start_col,
			size_t nrow, size_t ncol, const scalar_type &type,
			int node_id): lvirtual_col_matrix_store(global_start_row,
				global_start_col, nrow, ncol, type, node_id),
			store(ins, op, this, mater_level) {
		this->collect_compute = collect_compute;
	}

	collect_portion_compute::ptr get_compute() const {
		return collect_compute.lock();
	}

	virtual bool resize(off_t local_start_row, off_t local_start_col,
			size_t local_num_rows, size_t local_num_cols) {
		store.resize(local_start_row, local_start_col, local_num_rows,
				local_num_cols);
		return local_matrix_store::resize(local_start_row, local_start_col,
				local_num_rows, local_num_cols);
	}
	virtual void reset_size() {
		store.reset_size();
		local_matrix_store::reset_size();
	}

	using lvirtual_col_matrix_store::get_raw_arr;
	virtual const char *get_raw_arr() const {
		if (!store.is_materialized())
			store.materialize();
		return store.get_raw_arr();
	}

	using lvirtual_col_matrix_store::transpose;
	virtual matrix_store::const_ptr transpose() const {
		if (!store.is_materialized())
			store.materialize();
		return store.get_materialized_res().transpose();
	}

	using lvirtual_col_matrix_store::get_col;
	virtual const char *get_col(size_t col) const {
		if (!store.is_materialized())
			store.materialize();
		return static_cast<const local_col_matrix_store &>(
				store.get_materialized_res()).get_col(col);
	}
};

class lmapply_row_matrix_store: public lvirtual_row_matrix_store
{
	std::weak_ptr<collect_portion_compute> collect_compute;
	mapply_store store;
public:
	lmapply_row_matrix_store(materialize_level mater_level,
			const std::vector<local_matrix_store::const_ptr> &ins,
			const portion_mapply_op &op,
			collect_portion_compute::ptr collect_compute,
			off_t global_start_row, off_t global_start_col,
			size_t nrow, size_t ncol, const scalar_type &type,
			int node_id): lvirtual_row_matrix_store(global_start_row,
				global_start_col, nrow, ncol, type, node_id),
			store(ins, op, this, mater_level) {
		this->collect_compute = collect_compute;
	}

	collect_portion_compute::ptr get_compute() const {
		return collect_compute.lock();
	}

	virtual bool resize(off_t local_start_row, off_t local_start_col,
			size_t local_num_rows, size_t local_num_cols) {
		store.resize(local_start_row, local_start_col, local_num_rows,
				local_num_cols);
		return local_matrix_store::resize(local_start_row, local_start_col,
				local_num_rows, local_num_cols);
	}
	virtual void reset_size() {
		store.reset_size();
		local_matrix_store::reset_size();
	}

	using lvirtual_row_matrix_store::get_raw_arr;
	virtual const char *get_raw_arr() const {
		if (!store.is_materialized())
			store.materialize();
		return store.get_raw_arr();
	}

	using lvirtual_row_matrix_store::transpose;
	virtual matrix_store::const_ptr transpose() const {
		if (!store.is_materialized())
			store.materialize();
		return store.get_materialized_res().transpose();
	}

	using lvirtual_row_matrix_store::get_row;
	virtual const char *get_row(size_t row) const {
		if (!store.is_materialized())
			store.materialize();
		return static_cast<const local_row_matrix_store &>(
				store.get_materialized_res()).get_row(row);
	}

	using lvirtual_row_matrix_store::get_rows;
	virtual const char *get_rows(size_t row_start, size_t row_end) const {
		// TODO
		assert(0);
		return NULL;
	}
};

}

static inline bool is_all_in_mem(
		const std::vector<matrix_store::const_ptr> &in_mats)
{
	for (size_t i = 0; i < in_mats.size(); i++)
		if (!in_mats[i]->is_in_mem())
			return false;
	return true;
}

mapply_matrix_store::mapply_matrix_store(
		const std::vector<matrix_store::const_ptr> &_in_mats,
		portion_mapply_op::const_ptr op, matrix_layout_t layout,
		size_t nrow, size_t ncol, size_t _data_id): virtual_matrix_store(nrow,
			ncol, is_all_in_mem(_in_mats), op->get_output_type()),
		data_id(_data_id), in_mats(_in_mats)
{
	this->par_access = true;
	this->cache_portion = true;
	this->layout = layout;
	this->op = op;
}

void mapply_matrix_store::materialize_self() const
{
	// Materialize the matrix store if it hasn't.
	if (res)
		return;

	mapply_matrix_store *mutable_this = const_cast<mapply_matrix_store *>(this);
	mutable_this->res = materialize(is_in_mem(), get_num_nodes());
}

matrix_store::const_ptr mapply_matrix_store::materialize(bool in_mem,
			int num_nodes) const
{
	if (res) {
		if (res->is_in_mem() == in_mem && res->get_num_nodes() == num_nodes)
			return res;
		else {
			BOOST_LOG_TRIVIAL(error)
				<< "Can't materialize the mapply matrix to the specified storage";
			return matrix_store::const_ptr();
		}
	}
	else
		return __mapply_portion(in_mats, op, layout, in_mem, num_nodes,
				par_access);
}

vec_store::const_ptr mapply_matrix_store::get_col_vec(off_t idx) const
{
	if (res == NULL)
		materialize_self();
	return res->get_col_vec(idx);
}

vec_store::const_ptr mapply_matrix_store::get_row_vec(off_t idx) const
{
	if (res == NULL)
		materialize_self();
	return res->get_row_vec(idx);
}

matrix_store::const_ptr mapply_matrix_store::get_cols(
		const std::vector<off_t> &idxs) const
{
	if (res == NULL)
		materialize_self();
	return res->get_cols(idxs);
}

matrix_store::const_ptr mapply_matrix_store::get_rows(
		const std::vector<off_t> &idxs) const
{
	if (res == NULL)
		materialize_self();
	return res->get_rows(idxs);
}

local_matrix_store::const_ptr mapply_matrix_store::get_portion(
			size_t start_row, size_t start_col, size_t num_rows,
			size_t num_cols) const
{
	// We should try to get the portion from the local thread memory buffer
	// first.
	local_matrix_store::const_ptr ret = local_mem_buffer::get_mat_portion(
			data_id);
	// If it's in the same portion.
	if (ret && (((size_t) ret->get_global_start_row() == start_row
					&& (size_t) ret->get_global_start_col() == start_col
					&& ret->get_num_rows() == num_rows
					&& ret->get_num_cols() == num_cols)
				// If it's in the corresponding portion in the transposed matrix.
				|| ((size_t) ret->get_global_start_row() == start_col
					&& (size_t) ret->get_global_start_col() == start_row
					&& ret->get_num_rows() == num_cols
					&& ret->get_num_cols() == num_rows))) {
		assert(ret->get_local_start_row() == 0);
		assert(ret->get_local_start_col() == 0);
		return ret;
	}

	// If the virtual matrix store has been materialized, we should return
	// the portion from the materialized store directly.
	// If the materialized matrix store is external memory, it should cache
	// the portion itself.
	if (res)
		return res->get_portion(start_row, start_col, num_rows, num_cols);

	std::vector<local_matrix_store::const_ptr> parts(in_mats.size());
	if (is_wide()) {
		assert(start_row == 0);
		assert(num_rows == get_num_rows());
		for (size_t i = 0; i < in_mats.size(); i++)
			parts[i] = in_mats[i]->get_portion(start_row, start_col,
					in_mats[i]->get_num_rows(), num_cols);
	}
	else {
		assert(start_col == 0);
		assert(num_cols == get_num_cols());
		for (size_t i = 0; i < in_mats.size(); i++)
			parts[i] = in_mats[i]->get_portion(start_row, start_col,
					num_rows, in_mats[i]->get_num_cols());
	}

	if (store_layout() == matrix_layout_t::L_ROW)
		ret = local_matrix_store::const_ptr(new lmapply_row_matrix_store(
					get_materialize_level(), parts, *op, NULL, start_row,
					start_col, num_rows, num_cols, get_type(),
					parts.front()->get_node_id()));
	else
		ret = local_matrix_store::const_ptr(new lmapply_col_matrix_store(
					get_materialize_level(), parts, *op, NULL, start_row,
					start_col, num_rows, num_cols, get_type(),
					parts.front()->get_node_id()));
	if (cache_portion)
		local_mem_buffer::cache_portion(data_id, ret);
	return ret;
}

local_matrix_store::const_ptr mapply_matrix_store::get_portion(
			size_t id) const
{
	size_t start_row;
	size_t start_col;
	size_t num_rows;
	size_t num_cols;
	std::pair<size_t, size_t> chunk_size = get_portion_size();
	if (is_wide()) {
		start_row = 0;
		start_col = chunk_size.second * id;
		num_rows = get_num_rows();
		num_cols = std::min(chunk_size.second, get_num_cols() - start_col);
	}
	else {
		start_row = chunk_size.first * id;
		start_col = 0;
		num_rows = std::min(chunk_size.first, get_num_rows() - start_row);
		num_cols = get_num_cols();
	}
	return get_portion(start_row, start_col, num_rows, num_cols);
}

int mapply_matrix_store::get_portion_node_id(size_t id) const
{
	int node_id = -1;
	for (size_t i = 0; i < in_mats.size(); i++) {
		if (node_id < 0)
			node_id = in_mats[i]->get_portion_node_id(id);
		// We assume that all portions are either in the same node
		// or aren't associated with a node.
		// It might be expensive to compute if mapply matrix has
		// a deep hierarchy, so depth-first search can reduce the cost.
		if (node_id >= 0)
			break;
	}
	return node_id;
}

async_cres_t mapply_matrix_store::get_portion_async(
		size_t start_row, size_t start_col, size_t num_rows,
		size_t num_cols, portion_compute::ptr orig_compute) const
{
	// If the virtual matrix store has been materialized, we should return
	// the portion from the materialized store directly.
	// If the materialized matrix store is external memory, it should cache
	// the portion itself.
	if (res)
		return res->get_portion_async(start_row, start_col, num_rows, num_cols,
				orig_compute);

	// We should try to get the portion from the local thread memory buffer
	// first.
	local_matrix_store::const_ptr ret1 = local_mem_buffer::get_mat_portion(
			data_id);
	// If it's in the same portion.
	if (ret1 && (((size_t) ret1->get_global_start_row() == start_row
					&& (size_t) ret1->get_global_start_col() == start_col
					&& ret1->get_num_rows() == num_rows
					&& ret1->get_num_cols() == num_cols)
				// If it's in the corresponding portion in the transposed matrix.
				|| ((size_t) ret1->get_global_start_row() == start_col
					&& (size_t) ret1->get_global_start_col() == start_row
					&& ret1->get_num_rows() == num_cols
					&& ret1->get_num_cols() == num_rows))) {
		assert(ret1->get_local_start_row() == 0);
		assert(ret1->get_local_start_col() == 0);
		// In the asynchronous version, data in the portion isn't ready when
		// the method is called. We should add the user's portion computation
		// to the queue. When the data is ready, all user's portion computations
		// will be invoked.
		local_matrix_store *tmp = const_cast<local_matrix_store *>(ret1.get());
		collect_portion_compute::ptr collect_compute;
		if (ret1->store_layout() == matrix_layout_t::L_COL) {
			lmapply_col_matrix_store *store
				= dynamic_cast<lmapply_col_matrix_store *>(tmp);
			assert(store);
			collect_compute = store->get_compute();
		}
		else {
			lmapply_row_matrix_store *store
				= dynamic_cast<lmapply_row_matrix_store *>(tmp);
			assert(store);
			collect_compute = store->get_compute();
		}
		// If the collect compute doesn't exist, it mean the data in the local
		// matrix store may already by ready.
		if (collect_compute) {
			collect_compute->add_orig_compute(orig_compute);
			return async_cres_t(false, ret1);
		}
		else
			return async_cres_t(true, ret1);
	}

	std::shared_ptr<collect_portion_compute> collect_compute
		= std::shared_ptr<collect_portion_compute>(
			new collect_portion_compute(orig_compute));

	std::vector<local_matrix_store::const_ptr> parts(in_mats.size());
	if (is_wide()) {
		assert(start_row == 0);
		assert(num_rows == get_num_rows());
		for (size_t i = 0; i < in_mats.size(); i++) {
			async_cres_t res = in_mats[i]->get_portion_async(start_row, start_col,
					in_mats[i]->get_num_rows(), num_cols, collect_compute);
			parts[i] = res.second;
			// If the data in the request portion is invalid.
			if (!res.first && collect_compute)
				collect_compute->add_EM_part(parts[i]);
		}
	}
	else {
		assert(start_col == 0);
		assert(num_cols == get_num_cols());
		for (size_t i = 0; i < in_mats.size(); i++) {
			async_cres_t res = in_mats[i]->get_portion_async(start_row, start_col,
					num_rows, in_mats[i]->get_num_cols(), collect_compute);
			parts[i] = res.second;
			// If the data in the request portion is invalid.
			if (!res.first && collect_compute)
				collect_compute->add_EM_part(parts[i]);
		}
	}

	local_matrix_store::ptr ret;
	if (store_layout() == matrix_layout_t::L_ROW)
		ret = local_matrix_store::ptr(new lmapply_row_matrix_store(
					get_materialize_level(), parts, *op, collect_compute,
					start_row, start_col, num_rows, num_cols, get_type(),
					parts.front()->get_node_id()));
	else
		ret = local_matrix_store::ptr(new lmapply_col_matrix_store(
					get_materialize_level(), parts, *op, collect_compute,
					start_row, start_col, num_rows, num_cols, get_type(),
					parts.front()->get_node_id()));
	if (collect_compute)
		collect_compute->set_res_part(ret);
	if (cache_portion)
		local_mem_buffer::cache_portion(data_id, ret);
	// If all parts are from the in-mem matrix store or have been cached by
	// the underlying matrices, the data in the returned portion is immediately
	// accessible.
	return async_cres_t(collect_compute->get_num_EM_parts() == 0, ret);
}

std::pair<size_t, size_t> mapply_matrix_store::get_portion_size() const
{
	// I should use a relatively small chunk size here. Otherwise,
	// the aggregated memory size for buffering a portion of each matrix
	// will be too large.
	if (is_wide())
		return std::pair<size_t, size_t>(get_num_rows(),
				mem_matrix_store::CHUNK_SIZE);
	else
		return std::pair<size_t, size_t>(mem_matrix_store::CHUNK_SIZE,
				get_num_cols());
}

matrix_store::const_ptr mapply_matrix_store::transpose() const
{
	std::vector<matrix_store::const_ptr> t_in_mats(in_mats.size());
	for (size_t i = 0; i < in_mats.size(); i++)
		t_in_mats[i] = in_mats[i]->transpose();
	matrix_layout_t t_layout;
	if (layout == matrix_layout_t::L_COL)
		t_layout = matrix_layout_t::L_ROW;
	else
		t_layout = matrix_layout_t::L_COL;
	mapply_matrix_store *ret = new mapply_matrix_store(t_in_mats,
			op->transpose(), t_layout, get_num_cols(), get_num_rows(), data_id);
	if (this->res)
		ret->res = this->res->transpose();
	return matrix_store::const_ptr(ret);
}

std::vector<safs::io_interface::ptr> mapply_matrix_store::create_ios() const
{
	std::vector<safs::io_interface::ptr> ret;
	for (size_t i = 0; i < in_mats.size(); i++) {
		if (!in_mats[i]->is_in_mem()) {
			const EM_object *obj = dynamic_cast<const EM_object *>(in_mats[i].get());
			std::vector<safs::io_interface::ptr> tmp = obj->create_ios();
			ret.insert(ret.end(), tmp.begin(), tmp.end());
		}
	}
	return ret;
}

std::string mapply_matrix_store::get_name() const
{
	return (boost::format("vmat-%1%=") % data_id).str()
		+ op->to_string(in_mats);
}

std::unordered_map<size_t, size_t> mapply_matrix_store::get_underlying_mats() const
{
	if (res)
		return res->get_underlying_mats();

	std::unordered_map<size_t, size_t> final_res;
	for (size_t i = 0; i < in_mats.size(); i++) {
		std::unordered_map<size_t, size_t> res = in_mats[i]->get_underlying_mats();
		/*
		 * When we merge, we only need to collect the matrices with different
		 * data IDs.
		 */
		for (auto it = res.begin(); it != res.end(); it++) {
			auto to_it = final_res.find(it->first);
			if (to_it == final_res.end())
				final_res.insert(std::pair<size_t, size_t>(it->first, it->second));
		}
	}
	return final_res;
}

void mapply_matrix_store::set_cache_portion(bool cache_portion)
{
	this->cache_portion = cache_portion;
	for (size_t i = 0; i < in_mats.size(); i++)
		const_cast<matrix_store &>(*in_mats[i]).set_cache_portion(cache_portion);
}

}

}
