#include <iostream>
#include <mutex>
#include <list>
#include <sys/mman.h>
#include <sys/user.h>
#include <cassert>
#include <utility>


#define OBJECT_POOL_ALLOCATE_WARM_CACHE
#define OBJECT_CANARY				0xcacacacacacacaca
#undef OBJECT_CANARY
#undef OBJECT_POOL_DEFER_UNMAP

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
#ifdef OBJECT_CANARY
	unsigned long object_canary;
	unsigned long pad0;
#endif
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
	memory_pool_segment<T> *prev, *next;

	memory_pool_segment(void *mmap_segment, size_t size) : mmap_segment{mmap_segment}, size{size}, used_objs{},
								prev{nullptr}, next{nullptr} {}
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

	static constexpr bool is_large_object()
	{
		return sizeof(T) >= PAGE_SIZE / 8;
	}
	
	static constexpr size_t default_pool_size = 2 * PAGE_SIZE;

	static constexpr size_t size_of_chunk()
	{
		return align_up(sizeof(T), object_pool_alignment) + sizeof(memory_chunk<T>);
	}

	static constexpr size_t size_of_inline_segment()
	{
		return align_up(sizeof(memory_pool_segment<T>), object_pool_alignment);
	}

	static constexpr size_t memory_pool_size()
	{
		if(is_large_object())
		{
			return align_up(size_of_inline_segment() + size_of_chunk() * 24, PAGE_SIZE); 
		}
		else
			return default_pool_size;
	}

	constexpr size_t number_of_objects()
	{
		return (memory_pool_size() - size_of_inline_segment()) / size_of_chunk();
	}

	std::pair<memory_chunk<T> *, memory_chunk<T> *> setup_chunks()
	{
		memory_chunk<T> *prev = nullptr;
		memory_chunk<T> *curr = reinterpret_cast<memory_chunk<T> *>((unsigned char *) mmap_segment + size_of_inline_segment());
		auto first = curr;
		auto nr_objs = number_of_objects();

		while(nr_objs--)
		{
			curr->segment = this;
#ifdef OBJECT_CANARY
			curr->object_canary = OBJECT_CANARY;
#endif
			curr->next = nullptr;
			if(prev)	prev->next = curr;

			prev = curr;
			curr = reinterpret_cast<memory_chunk<T> *>(reinterpret_cast<unsigned char *>(curr) + size_of_chunk());
		}

		return std::pair<memory_chunk<T> *, memory_chunk<T> *>(first, prev);
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
	memory_pool_segment<T> *segment_head, *segment_tail;
	size_t nr_objects;

	void append_segment(memory_pool_segment<T> *seg)
	{
		if(!segment_head)
		{
			segment_head = segment_tail = seg;
		}
		else
		{
			segment_tail->next = seg;
			seg->prev = segment_tail;
			segment_tail = seg;
		}
	}

	void remove_segment(memory_pool_segment<T> *seg)
	{
		if(seg->prev)
		{
			seg->prev->next = seg->next;
		}
		else
			segment_head = seg->next;
		
		if(seg->next)
			seg->next->prev = seg->prev;
		else
			segment_tail = seg->prev;

		seg->~memory_pool_segment<T>();
	}

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
	
		memory_pool_segment<T> &mmap_seg = *static_cast<memory_pool_segment<T> *>(new_mmap_region);
		mmap_seg = std::move(seg);

		auto pair = mmap_seg.setup_chunks();
		free_chunk_head = pair.first;
		free_chunk_tail = pair.second;

		append_segment(&mmap_seg);

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

	void free_list_purge_segment_chunks(memory_pool_segment<T> *seg)
	{
		//std::cout << "Removing chunks\n";
		auto l = free_chunk_head;
		memory_chunk<T> *prev = nullptr;
		while(l)
		{
			//std::cout << "Hello " << l << "\n";
			if(l->segment == seg)
			{
				//std::cout << "Removing chunk " << l << "\n";
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

	void append_chunk_tail(memory_chunk<T> *chunk)
	{
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
	}

	void append_chunk_head(memory_chunk<T> *chunk)
	{
		if(!free_chunk_head)
		{
			free_chunk_head = free_chunk_tail = chunk;
		}
		else
		{
			auto curr_head = free_chunk_head;
			free_chunk_head = chunk;
			free_chunk_head->next = curr_head;
			assert(free_chunk_tail != nullptr);
		}
	}

	void purge_segment(memory_pool_segment<T> *segment)
	{
		if(segment->empty())
		{
			/* We can still have free objects on the free list. Remove them. */
			free_list_purge_segment_chunks(segment);
			remove_segment(segment);
		}
	}

public:
	size_t used_objects;

	memory_pool() : free_chunk_head{nullptr}, free_chunk_tail{nullptr}, lock{}, segment_head{}, segment_tail{},
			nr_objects{0}, used_objects{0} {}

	void print_segments()
	{
	}

	~memory_pool()
	{
		assert(used_objects == 0);
	}

	T *allocate()
	{
		std::scoped_lock guard{lock};

		while(!free_chunk_head)
		{
			if(!expand_pool())
			{
				//std::cout << "mmap failed\n";
				return nullptr;
			}
		}

		auto return_chunk = free_chunk_head;

		free_chunk_head = free_chunk_head->next;

		if(!free_chunk_head)	free_chunk_tail = nullptr;

		return_chunk->segment->used_objs++;
		used_objects++;

#ifdef OBJECT_CANARY
		assert(return_chunk->object_canary == OBJECT_CANARY);
#endif

		return reinterpret_cast<T *>(return_chunk + 1);
	}

	void free(T *ptr)
	{
		auto chunk = ptr_to_chunk(ptr);
		//std::cout << "Removing chunk " << chunk << "\n";
		std::scoped_lock guard{lock};

		chunk->next = nullptr;
#ifdef OBJECT_CANARY
		assert(chunk->object_canary == OBJECT_CANARY);
#endif

#ifndef OBJECT_POOL_ALLOCATE_WARM_CACHE
		append_chunk_tail(chunk);
#else
		append_chunk_head(chunk);
#endif

		used_objects--;
		chunk->segment->used_objs--;

#ifndef OBJECT_POOL_DEFER_UNMAP
		purge_segment(chunk->segment);
#endif
	}


	void purge()
	{
		auto s = segment_head;

		while(s)
		{
			auto next = s->next;
			purge_segment(s);
			s = next;
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

	pool.purge();

	//std::cout << "Used objects: " << pool.used_objects << "\n";
	return 0;
}
