#include <bitset>
#include <cassert>

#include "diskinterface.hpp"

Chunk::~Chunk() {
	// whenever the last reference to a chunk is released, we flush the chunk
	// out to the disk 
	this->parent->flush_chunk(*this);
}

std::shared_ptr<Chunk> Disk::get_chunk(Size chunk_idx) {
	std::lock_guard<std::mutex> g(lock); // acquire the lock

	if (chunk_idx > this->size_chunks()) {
		throw DiskException("chunk index out of bounds");
	}
	
	if (auto chunk_ref = this->chunk_cache.get(chunk_idx)) {
		return chunk_ref;
	}

	// initialize the new chunk
	std::shared_ptr<Chunk> chunk(new Chunk);
	chunk->parent = this; 
	chunk->size_bytes = this->chunk_size();
	chunk->chunk_idx = chunk_idx;
	chunk->data = std::unique_ptr<Byte[]>(new Byte);
	std::memcpy(chunk->data.get(), this->data.get() + chunk_idx * this->chunk_size(), 
		this->chunk_size());

	// store it into the chunk cache so that it can be shared if requested again
	this->chunk_cache.put(chunk_idx, chunk); 
	return std::move(chunk);
}

void Disk::flush_chunk(const Chunk& chunk) {
	std::lock_guard<std::mutex> g(lock); // acquire the lock

	assert(chunk.size_bytes == this->chunk_size());
	assert(chunk.parent == this);

	std::memcpy(this->data.get() + chunk.chunk_idx * this->chunk_size(), 
		chunk.data.get(), this->chunk_size());
}

void Disk::try_close() {
	std::lock_guard<std::mutex> g(lock); // acquire the lock
	this->chunk_cache.sweep(true);
	if (this->chunk_cache.size() > 0) {
		throw DiskException("there are still chunks referenced in other parts of the program");
	}
}

Disk::~Disk() {
	
}

std::array<DiskBitMap::BitRange, 256> DiskBitMap::find_unset_cache;

static bool bitmap_init_cache(std::array<DiskBitMap::BitRange, 256>& cache) {
	for (Size idx = 0; idx < 256; ++idx) {
		std::bitset<8> byte(idx);

		for (Size j = 0; j < 8; ++j) {
			if (!byte[j]) {
				cache[idx].start_idx = j;
				Size k = 1;
				while (!byte[j + k] && j + k < 8) {
					k++;
				}
				cache[idx].bit_count = k;
				break;
			}
		}
	}
	return true;
}

static bool find_unset_cache_initialized = bitmap_init_cache(DiskBitMap::find_unset_cache);