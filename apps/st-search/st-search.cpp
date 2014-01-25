/**
 * Copyright 2013 Da Zheng
 *
 * This file is part of SA-GraphLib.
 *
 * SA-GraphLib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SA-GraphLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SA-GraphLib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <google/profiler.h>

#include "thread.h"
#include "io_interface.h"
#include "container.h"
#include "concurrency.h"

#include "vertex_index.h"
#include "graph_engine.h"
#include "graph_config.h"

class st_vertex: public compute_vertex
{
	enum {
		VISITED,
	};

	atomic_flags<int> flags;
public:
	st_vertex(): compute_vertex(-1, -1, 0) {
	}

	st_vertex(vertex_id_t id, off_t off, int size): compute_vertex(
			id, off, size) {
	}

	bool has_visited() const {
		return flags.test_flag(VISITED);
	}

	bool set_visited(bool visited) {
		if (visited)
			return flags.set_flag(VISITED);
		else
			return flags.clear_flag(VISITED);
	}

	bool run(graph_engine &graph, const page_vertex *vertex);

	bool run_on_neighbors(graph_engine &graph, const page_vertex *vertices[],
			int num) {
		return true;
	}

	virtual void run_on_messages(graph_engine &,
			const vertex_message *msgs[], int num) {
	}
};

atomic_integer num_visited_vertices;

bool st_vertex::run(graph_engine &graph, const page_vertex *vertex)
{
	vertex_id_t max_id = graph.get_max_vertex_id();
	vertex_id_t min_id = graph.get_min_vertex_id();

	// We need to add the neighbors of the vertex to the queue of
	// the next level.
	std::vector<vertex_id_t> activated_vertices;
	page_byte_array::const_iterator<vertex_id_t> end_it
		= vertex->get_neigh_end(OUT_EDGE);
	for (page_byte_array::const_iterator<vertex_id_t> it
			= vertex->get_neigh_begin(OUT_EDGE); it != end_it; ++it) {
		vertex_id_t id = *it;
		assert(id >= min_id && id <= max_id);
		st_vertex &info = (st_vertex &) graph.get_vertex(id);
		// If the vertex has been visited, we can skip it.
		if (info.has_visited())
			continue;
		if (info.set_visited(true))
			continue;
		activated_vertices.push_back(id);
	}
	if (activated_vertices.size() > 0)
		num_visited_vertices.inc(activated_vertices.size());

	graph.activate_vertices(activated_vertices.data(),
			activated_vertices.size());

	return true;
}

void int_handler(int sig_num)
{
	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	exit(0);
}

int main(int argc, char *argv[])
{
	if (argc < 6) {
		fprintf(stderr, "bfs conf_file graph_file index_file start_vertex directed\n");
		graph_conf.print_help();
		params.print_help();
		exit(-1);
	}

	std::string conf_file = argv[1];
	std::string graph_file = argv[2];
	std::string index_file = argv[3];
	vertex_id_t start_vertex = atoi(argv[4]);
	bool directed = atoi(argv[5]);

	config_map configs(conf_file);
	configs.add_options(argv + 6, argc - 6);
	graph_conf.init(configs);
	graph_conf.print();

	signal(SIGINT, int_handler);
	init_io_system(configs);

	int min_vertex_size;
	if (directed)
		min_vertex_size = sizeof(ext_mem_directed_vertex);
	else
		min_vertex_size = sizeof(ext_mem_undirected_vertex);

	graph_index *index = graph_index_impl<st_vertex>::create(index_file, min_vertex_size);
	ext_mem_vertex_interpreter *interpreter;
	if (directed)
		interpreter = new ext_mem_directed_vertex_interpreter();
	else
		interpreter = new ext_mem_undirected_vertex_interpreter();
	graph_engine *graph = graph_engine::create(graph_conf.get_num_threads(),
			params.get_num_nodes(), graph_file, index, interpreter, directed);
	printf("BFS starts\n");
	printf("prof_file: %s\n", graph_conf.get_prof_file().c_str());
	if (!graph_conf.get_prof_file().empty())
		ProfilerStart(graph_conf.get_prof_file().c_str());

	struct timeval start, end;
	gettimeofday(&start, NULL);
	graph->start(&start_vertex, 1);
	graph->wait4complete();
	gettimeofday(&end, NULL);

	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	if (graph_conf.get_print_io_stat())
		print_io_thread_stat();
	graph->cleanup();
	printf("BFS from vertex %ld visits %d vertices. It takes %f seconds\n",
			start_vertex, num_visited_vertices.get(),
			time_diff(start, end));
}