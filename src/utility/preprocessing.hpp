/*
 * preprocessing.hpp
 *
 *  Created on: Mar 27, 2017
 *      Author: kai
 */

#ifndef UTILITY_PREPROCESSING_HPP_
#define UTILITY_PREPROCESSING_HPP_

#include "../common/RStreamCommon.hpp"
//#include "../core/engine.hpp"

namespace RStream {
	class Preprocessing {
		std::atomic<int> atomic_num_producers;
		std::atomic<int> atomic_chunk_id;
		std::atomic<int> atomic_partition_number;
		std::string input;
//		std::string output;
		int num_partitions;
		int num_vertices;
		int vertices_per_partition;
		int edge_unit;
		int edge_type;

	public:
		inline int getEdgeUnit(){
			return edge_unit;
		}

		inline int getEdgeType(){
			return edge_type;
		}

		inline int getNumVerPerPartition(){
			return vertices_per_partition;
		}

		Preprocessing(std::string & _input, int _num_partitions, int _num_vertices) :
			input(_input), num_partitions(_num_partitions), num_vertices(_num_vertices)
		{
			atomic_num_producers = 0;
			atomic_chunk_id = 0;
			atomic_partition_number = num_partitions;
			vertices_per_partition = num_vertices / num_partitions;

			convert_edgelist();
			dump(_input + ".binary");

			if(edge_type == 0) {
				generate_partitions<Edge>();
			}
			else if(edge_type == 1) {
				generate_partitions<WeightedEdge>();
			}

			for(int i = 0; i < num_partitions; i++) {
				std::cout << "===============Printing Partition " << i << "================" << std::endl;
				dump(input + "." + std::to_string(i));
			}
		}

		void convert_edgelist() {
			FILE * fd = fopen(input.c_str(), "r");
			assert(fd != NULL );

			char * buf = (char *)memalign(PAGE_SIZE, IO_SIZE);
			long pos = 0;

			long counter = 0;
			char s[1024];
			while(fgets(s, 1024, fd) != NULL) {
				FIXLINE(s);

				if (s[0] == '#') continue; // Comment
				if (s[0] == '%') continue; // Comment

				counter++;

				char delims[] = "\t, ";
				char * t;
				t = strtok(s, delims);
				assert(t != NULL);

				VertexId from = atoi(t) - 1;
				t = strtok(NULL, delims);
				assert(t != NULL);
				VertexId to = atoi(t) - 1;

				if(from == to) continue;

				void * data = nullptr;

				Weight val;
				/* Check if has value */
				t = strtok(NULL, delims);
				if(t != NULL) {
					val = atof(t);
					data = new WeightedEdge(from, to, val);
					edge_unit = sizeof(VertexId) * 2 + sizeof(Weight);
					std::memcpy(buf + pos, data, edge_unit);
					pos += edge_unit;
					edge_type = 1;
				} else {
					data = new Edge(from, to);
					edge_unit =  sizeof(VertexId) * 2;
					std::memcpy(buf + pos, data, edge_unit);
					pos += edge_unit;
					edge_type = 0;
				}

//				std::cout << "src: " << from << " , target: " << to  << " , weight : " << val << std::endl;
				assert(IO_SIZE % edge_unit == 0);

				if(pos >= IO_SIZE) {
					int perms = O_WRONLY | O_APPEND;
					int fout = open((input + ".binary").c_str(), perms, S_IRWXU);
					if(fout < 0){
						fout = creat((input + ".binary").c_str(), S_IRWXU);
					}
					io_manager::write_to_file(fout, buf, IO_SIZE);
					counter -= IO_SIZE / edge_unit;
					close(fout);
					pos = 0;
				}

			}

			int perms = O_WRONLY | O_APPEND;
			int fout = open((input + ".binary").c_str(), perms, S_IRWXU);
			if(fout < 0){
				fout = creat((input + ".binary").c_str(), S_IRWXU);
			}
			io_manager::write_to_file(fout, buf, counter * edge_unit);
			close(fout);

		}

		template<typename T>
		void generate_partitions() {

//			int num_threads = std::thread::hardware_concurrency();
			int num_threads = 8;
			int num_write_threads = num_threads > 2 ? 2 : 1;
			int num_exec_threads = num_threads > 2 ? num_threads - 2 : 1;

			int fd = open((input + ".binary").c_str(), O_RDONLY);
			assert(fd > 0 );

			// get file size
			long file_size = io_manager::get_filesize(fd);
			int streaming_counter = file_size / IO_SIZE + 1;
			long valid_io_size = 0;
			long offset = 0;

			concurrent_queue<std::tuple<int, long, long>> * task_queue = new concurrent_queue<std::tuple<int, long, long>>(65536);
			// <fd, offset, length>
			for(int counter = 0; counter < streaming_counter; counter++) {
				if(counter == streaming_counter - 1)
					// TODO: potential overflow?
					valid_io_size = file_size - IO_SIZE * (streaming_counter - 1);
				else
					valid_io_size = IO_SIZE;

				task_queue->push(std::make_tuple(fd, offset, valid_io_size));
				offset += valid_io_size;
			}

//			print_thread_info_locked("task queue size is: " + std::to_string(task_queue->size()) + "\n");
			const int queue_size = task_queue->size();
			// allocate global buffers for shuffling
			// TODO: Edge?
			global_buffer<T> ** buffers_for_shuffle = buffer_manager<T>::get_global_buffers(num_partitions);

			std::vector<std::thread> exec_threads;
			for(int i = 0; i < num_exec_threads; i++)
				exec_threads.push_back( std::thread([=] { this->producer<T>(buffers_for_shuffle, task_queue); } ));

//			print_thread_info_locked("In main thread ........\n");

			// write threads will flush shuffle buffer to file as long as it's full
			std::vector<std::thread> write_threads;
			for(int i = 0; i < num_write_threads; i++)
				write_threads.push_back(std::thread(&Preprocessing::consumer<T>, this, buffers_for_shuffle, queue_size));

			// join all threads
			for(auto & t : exec_threads)
				t.join();

			for(auto &t : write_threads)
				t.join();

			delete[] buffers_for_shuffle;
			delete task_queue;
			close(fd);

		}

		template<typename T>
		void producer(global_buffer<T> ** buffers_for_shuffle, concurrent_queue<std::tuple<int, long, long> > * task_queue) {
			atomic_num_producers++;

			int fd = -1;
			long offset = 0, length = 0;
			auto one_task = std::make_tuple(fd, offset, length);

			// streaming edges
			char * local_buf = (char *)memalign(PAGE_SIZE, IO_SIZE);
			VertexId src = 0, dst = 0;
			Weight weight = 0.0f;

			// pop from queue
			while(task_queue->test_pop_atomic(one_task)){
				fd = std::get<0>(one_task);
				offset = std::get<1>(one_task);
				length = std::get<2>(one_task);

				io_manager::read_from_file(fd, local_buf, length, offset);
				for(long pos = 0; pos < length; pos += edge_unit) {

					src = *(VertexId*)(local_buf + pos);
					dst = *(VertexId*)(local_buf + pos + sizeof(VertexId));
					std::cout << src << ", " << dst << std::endl;
					assert(src >= 0 && src < num_vertices && dst >= 0 && dst < num_vertices);

					// insert into shuffle buffer accordingly
					void * data = nullptr;
					if(typeid(T) == typeid(Edge)) {
						data = new Edge(src, dst);
					} else if(typeid(T) == typeid(WeightedEdge)) {
						weight = *(Weight*)(local_buf + pos + sizeof(VertexId) * 2);
						data = new WeightedEdge(src, dst, weight);
					}

					int index = get_global_buffer_index(src);

					global_buffer<T>* global_buf = buffer_manager<T>::get_global_buffer(buffers_for_shuffle, num_partitions, index);
					global_buf->insert((T*)data, index);
				}
			}

			delete[] local_buf;
			atomic_num_producers--;
		}

		template<typename T>
		void consumer(global_buffer<T> ** buffers_for_shuffle, const int num_chunks) {
			while(atomic_num_producers != 0) {
				int i = (atomic_chunk_id++) % num_chunks ;

				//debugging info
//				print_thread_info_locked("as a consumer dealing with buffer[" + std::to_string(i) + "]\n");

				const char * file_name = (input + "." + std::to_string(i)).c_str();
				global_buffer<T>* g_buf = buffer_manager<T>::get_global_buffer(buffers_for_shuffle, num_partitions, i);
				g_buf->flush(file_name, i);
			}

			//the last run - deal with all remaining content in buffers
			while(true){
				int i = --atomic_partition_number;
				if(i >= 0){
//					//debugging info
//					print_thread_info("as a consumer dealing with buffer[" + std::to_string(i) + "]\n");

					const char * file_name = (input + "." + std::to_string(i)).c_str();
					global_buffer<T>* g_buf = buffer_manager<T>::get_global_buffer(buffers_for_shuffle, num_partitions, i);
					g_buf->flush_end(file_name, i);

					delete g_buf;
				}
				else{
					break;
				}
			}
		}

		int get_global_buffer_index(int src) {

			int partition_id = src/ vertices_per_partition;
			return partition_id < (num_partitions - 1) ? partition_id : (num_partitions - 1);
		}

		// Removes \n from the end of line
		void FIXLINE(char * s) {
			int len = (int) strlen(s)-1;
			if(s[len] == '\n') s[len] = 0;
		}

		void dump(std::string input) {
			int fd = open(input.c_str(), O_RDONLY);
			assert(fd > 0 );

			// get file size
			long file_size = io_manager::get_filesize(fd);
			char * buf = (char *)malloc(file_size);
			io_manager::read_from_file(fd, buf, file_size, 0);

			VertexId src = -1, dst = -1;
			Weight weight = 0.0f;
			for(long pos = 0; pos < file_size; pos += edge_unit) {
				src = *(VertexId*)(buf + pos);
				dst = *(VertexId*)(buf + pos + sizeof(VertexId));

				if(edge_type == 1)
					weight = *(Weight*)(buf + pos + sizeof(VertexId) * 2);

				assert(src >= 0 && dst >= 0);

				if(edge_type == 0)
					std::cout << std::to_string(src) << " " <<  std::to_string(dst) << std::endl;
				else if(edge_type == 1)
					std::cout << std::to_string(src) << " " <<  std::to_string(dst) << " " << std::to_string(weight) << std::endl;
				else
					assert(false);
			}

			close(fd);
		}
	};

}



#endif /* UTILITY_PREPROCESSING_HPP_ */
