#include <iostream>
#include <mutex>
#include <list>
#include <sys/mman.h>
#include <sys/user.h>
#include <cassert>
#include <utility>

static constexpr size_t object_pool_alignment = 16UL;

template <typename T>
constexpr T align_up(T number, T alignment)
{
	return (number + (alignment - 1)) & -alignment;
}


template <typename T>
class memory_pool_segment;

template <typename T>
struct memory_chunk
{
	struct memory_chunk *next;
	memory_pool_segment<T> *segment;
	/* Note that this is 16-byte aligned */
} __attribute__((packed));

template <typename T>
class memory_pool_segment
{
private:
	void *mmap_segment;
	size_t size;
public:
	size_t used_objs;

	memory_pool_segment(void *mmap_segment, size_t size) : mmap_segment{mmap_segment}, size{size}, used_objs{} {}
	~memory_pool_segment()
	{
		/* If mmap_segment is null, it's an empty object(has been std::move'd) */
		if(mmap_segment != nullptr)
		{
			assert(used_objs == 0);
			//std::cout << "Freeing segment " << mmap_segment << "\n";
			munmap(mmap_segment, size);
		}
	}

	memory_pool_segment(const memory_pool_segment &rhs) = delete;
	memory_pool_segment& operator=(const memory_pool_segment &rhs) = delete;

	memory_pool_segment(memory_pool_segment&& rhs)
	{
		if(this == &rhs)
			return;
		mmap_segment = rhs.mmap_segment;
		size = rhs.size;
		used_objs = rhs.used_objs;

		rhs.mmap_segment = nullptr;
		rhs.size = SIZE_MAX;
		rhs.used_objs = 0;
	}

	memory_pool_segment& operator=(memory_pool_segment&& rhs)
	{
		if(this == &rhs)
			return *this;
		mmap_segment = rhs.mmap_segment;
		size = rhs.size;
		used_objs = rhs.used_objs;

		rhs.mmap_segment = nullptr;
		rhs.size = SIZE_MAX;
		rhs.used_objs = 0;

		return *this;
	}

	static constexpr size_t memory_pool_size()
	{
		return 2 * PAGE_SIZE;
	}

	constexpr size_t size_of_chunk()
	{
		return align_up(sizeof(T), object_pool_alignment) + sizeof(memory_chunk<T>);
	}

	constexpr size_t number_of_objects()
	{
		return memory_pool_size() / size_of_chunk();
	}

	std::pair<memory_chunk<T> *, memory_chunk<T> *> setup_chunks()
	{
		memory_chunk<T> *prev = nullptr;
		memory_chunk<T> *curr = static_cast<memory_chunk<T> *>(mmap_segment);
		auto nr_objs = number_of_objects();

		while(nr_objs--)
		{
			curr->segment = this;
			curr->next = nullptr;
			if(prev)	prev->next = curr;

			prev = curr;
			curr = reinterpret_cast<memory_chunk<T> *>(reinterpret_cast<unsigned char *>(curr) + size_of_chunk());
		}

		return std::pair<memory_chunk<T> *, memory_chunk<T> *>(static_cast<memory_chunk<T> *>(mmap_segment), prev);
	}

	bool empty()
	{
		return used_objs == 0;
	}

	bool operator==(const memory_pool_segment<T> &rhs)
	{
		return mmap_segment == rhs.mmap_segment;
	}

	void *get_mmap_segment()
	{
		return mmap_segment;
	}
};

template <typename T>
class memory_pool
{
private:
	memory_chunk<T> *free_chunk_head, *free_chunk_tail;
	std::mutex lock;
	std::list<memory_pool_segment<T> > segment_list;
	size_t nr_objects;

	bool expand_pool()
	{
		//std::cout << "Expanding pool.\n";
		auto allocation_size = memory_pool_segment<T>::memory_pool_size();
		void *new_mmap_region = mmap(nullptr, allocation_size, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if(!new_mmap_region)
			return false;

		memory_pool_segment<T> seg{new_mmap_region, allocation_size};

		nr_objects += seg.number_of_objects();

		//std::cout << "Added " << new_mmap_region << " size " << allocation_size << "\n";
	
		segment_list.push_back(std::move(seg));

		auto pair = segment_list.back().setup_chunks();
		free_chunk_head = pair.first;
		free_chunk_tail = pair.second;

		/* TODO: Test for failure */
		return true;
	}

	inline memory_chunk<T> *ptr_to_chunk(T *ptr)
	{
		/* Memory is layed out like this:
		 * ----------------------------------
		 * memory_chunk<T>
		 * ..................................
		 * T data
		 * ..................................
		 * Possible padding in between chunks
		 * ----------------------------------*/

		memory_chunk<T> *c = reinterpret_cast<memory_chunk<T> *>(ptr) - 1;
		return c;
	}

	void free_list_remove_chunks(memory_pool_segment<T> *seg)
	{
		//std::cout << "Removing chunks\n";
		auto l = free_chunk_head;
		memory_chunk<T> *prev = nullptr;
		while(l)
		{
			//std::cout << "Hello " << l << "\n";
			if(l->segment == seg)
			{
				//std::cout << "Removing daldmchunk " << l << "\n";
				if(prev)
					prev->next = l->next;
				else
					free_chunk_head = l->next;
				
				if(!l->next)
					free_chunk_tail = prev;
			}

			l = l->next;
		}
	}
public:
	size_t used_objects;

	memory_pool() : free_chunk_head{nullptr}, free_chunk_tail{nullptr}, lock{}, segment_list{}, nr_objects{0}, used_objects{0} {}

	void print_segments()
	{
		//std::cout << "List size: " << segment_list.size() << "\n";
		for(auto &s : segment_list)
		{
			//std::cout << "Segment: " << s.get_mmap_segment() << " Used objs: " << s.used_objs << "\n";
		}
	}

	~memory_pool()
	{
		print_segments();
		assert(used_objects == 0);
	}

	T *allocate()
	{
		std::scoped_lock guard{lock};

		while(!free_chunk_head)
		{
			if(!expand_pool())
			{
				//std::cout << "mmap failed after " << segment_list.size() << "\n";
				return nullptr;
			}
		}

		auto return_chunk = free_chunk_head;

		free_chunk_head = free_chunk_head->next;

		if(!free_chunk_head)	free_chunk_tail = nullptr;

		return_chunk->segment->used_objs++;
		used_objects++;

		return reinterpret_cast<T *>(return_chunk + 1);
	}

	void free(T *ptr)
	{
		auto chunk = ptr_to_chunk(ptr);
		//std::cout << "Removing chunk " << chunk << "\n";
		std::scoped_lock guard{lock};

		chunk->next = nullptr;

		if(!free_chunk_tail)
		{
			free_chunk_head = free_chunk_tail = chunk;
		}
		else
		{
			free_chunk_tail->next = chunk;
			free_chunk_tail = chunk;
			assert(free_chunk_head != nullptr);
		}

		used_objects--;
		chunk->segment->used_objs--;

		if(chunk->segment->empty())
		{
			/* We can still have free objects on the free list. Remove them. */
			free_list_remove_chunks(chunk->segment);
			auto &seg = *chunk->segment;
			segment_list.remove(seg);
		}
	}
};

class object
{
private:
	unsigned long a;
	unsigned long b;
	unsigned long c;
};

#include <vector>

int main(int argc, char **)
{
	memory_pool<object> pool;
	/* TODO: Why is pool going out of scope */
	/* TODO: Add support for buffers bigger than 2 * PAGE_SIZE and adjust segment size dynamically */
	std::vector<object *> vec;

	for(int i = 0; i < 10000; i++)
	{
		auto p = pool.allocate();
		vec.push_back(p);
		//std::cout << "Allocated " << p << "\n";
	}

	for(auto &p : vec)
	{
		pool.free(p);
		p = nullptr;
	}

	//std::cout << "Used objects: " << pool.used_objects << "\n";
	return 0;
}
