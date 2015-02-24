#ifndef __FM_VECTOR_H__
#define __FM_VECTOR_H__

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
#include <memory>

namespace fm
{

class bulk_operate;

class vector
{
	size_t length;
	size_t entry_size;
	bool in_mem;

protected:
	vector(size_t length, size_t entry_size, bool in_mem) {
		this->length = length;
		this->entry_size = entry_size;
		this->in_mem = in_mem;
	}
public:
	typedef std::shared_ptr<vector> ptr;
	typedef std::shared_ptr<const vector> const_ptr;

	virtual ~vector() {
	}

	bool is_in_mem() const {
		return in_mem;
	}

	size_t get_length() const {
		return length;
	}

	size_t get_type() const {
		// TODO
		return -1;
	}

	template<class T>
	bool is_type() const {
		// TODO
		return sizeof(T) == entry_size;
	}

	size_t get_entry_size() const {
		return entry_size;
	}

	virtual bool resize(size_t new_length) {
		this->length = new_length;
		return true;
	}

	virtual bool set_sub_vec(off_t start, const vector &vec) = 0;
	virtual vector::const_ptr get_sub_vec(off_t start, size_t length) const = 0;
	virtual bool append(std::vector<vector::ptr>::const_iterator vec_it,
			std::vector<vector::ptr>::const_iterator vec_end) = 0;

	virtual void sort() = 0;
	virtual bool is_sorted() const = 0;

	/**
	 * This is a deep copy. It copies all members of the vector object
	 * as well as the data in the vector;
	 */
	virtual vector::ptr clone() const = 0;
	/**
	 * This only copies all members of the vector object.
	 */
	virtual vector::ptr shallow_copy() = 0;
	virtual vector::const_ptr shallow_copy() const = 0;
};

}

#endif
