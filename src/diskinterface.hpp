#ifndef DISKINTERFACE_HPP
#define DISKINTERFACE_HPP

#include <stdint.h>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <array>
#include <iostream>

#include <cstring>
#include <memory>

typedef uint8_t Byte;
typedef uint64_t Size;

class Disk;

struct DiskException : public std::exception {
	std::string message;
	DiskException(const std::string &message) : message(message) { };
};

struct Chunk {
	Disk *parent = nullptr;

	std::mutex lock;
	size_t size_bytes = 0;
	size_t chunk_idx = 0;
	std::unique_ptr<Byte[]> data = nullptr;

	~Chunk();
};


template<typename K, typename V>
class SharedObjectCache {
private:
	size_t size_next_sweep = 16;
	std::unordered_map<K, std::weak_ptr<V>> map;
public:
	void sweep(bool force) {
		if (!force && map.size() < size_next_sweep)
			return ;

		for (auto it = this->map.cbegin(); it != this->map.cend();){
			if ((*it).second.expired()) {
				this->map.erase(it++);    // or "it = m.erase(it)" since C++11
			} else {
				++it;
			}
		}

		size_next_sweep = this->map.size() < 16 ? 16 : this->map.size();
	}

	void put(const K& k, std::weak_ptr<V> v) {
		map[k] = std::move(v);
		this->sweep(false);
	}

	std::shared_ptr<V> get(const K& k) {
		auto ref = this->map.find(k);
		if (ref != this->map.end()) {
			if (std::shared_ptr<V> v = (*ref).second.lock()) {
				return std::move(v);
			}
		}

		return nullptr;
	}

	inline size_t size() {
		return this->map.size();
	}
};

/*
	acts as an interface onto the disk as well as a cache for chunks on disk
	in this way the same chunk can be accessed and modified in multiple places
	at the same time if this is desirable
*/
class Disk {
private:
	// properties of the class
	const Size _size_chunks;
	const Size _chunk_size;

	std::unique_ptr<Byte[]> data;

	// a mutex which protects access to the disk
	std::mutex lock;

	// a cache of chunks that are loaded in
	SharedObjectCache<Size, Chunk> chunk_cache;

	// loops over weak pointers, if any of them are expired, it deletes 
	// the entries from the unordered map 
	void sweep_chunk_cache(); 
public:

	Disk(Size size_chunk_ctr, Size chunk_size_ctr) 
		: _chunk_size(chunk_size_ctr), _size_chunks(size_chunk_ctr) {
		// initialize the data for the disk
		this->data = std::unique_ptr<Byte[]>(new Byte[this->size_bytes() + 1]);
		std::memset(this->data.get(), 0, this->size_bytes());
	}

	inline Size size_bytes() const {
		return _size_chunks * _chunk_size;
	}

	inline Size size_chunks() const {
		return _size_chunks;
	}

	inline Size chunk_size() const {
		return _chunk_size;
	}

	std::shared_ptr<Chunk> get_chunk(Size chunk_idx);

	void flush_chunk(const Chunk& chunk);

	void try_close();

	~Disk();
};

/*
	A utility class that implements a bitmap ontop of a range of chunks
*/
struct DiskBitMap {
	std::mutex block;

	Disk *disk;
	Size size_in_bits;
	std::vector<std::shared_ptr<Chunk>> chunks;

	DiskBitMap(Disk *disk, Size chunk_start, Size size_in_bits) {
		this->size_in_bits = size_in_bits;
		this->disk = disk;
		for (uint64_t idx = 0; idx < this->size_chunks(); ++idx) {
			auto chunk = disk->get_chunk(idx + chunk_start);
			chunk->lock.lock();
			this->chunks.push_back(std::move(chunk));
		}
	}

	~DiskBitMap() {
		for (auto &chunk : chunks) {
			chunk->lock.unlock();
		}
	}

	void clear_all() {
		std::cout << "\tIN CLEAR ALL" << std::endl;
		for (std::shared_ptr<Chunk>& chunk : chunks) {
			std::cout << "\t\ttrying to clear a chunk" << std::endl;
			std::cout << "\t\tclearing chunk: " << chunk->chunk_idx << "/" << this->chunks.size() << std::endl;
			std::cout << "\t\t\tchunk size in bytes is: " << chunk->size_bytes << std::endl;
			std::cout << "\t\t\tchunk data address is:" << (unsigned long long)chunk->data.get() << std::endl;
			std::memset(chunk->data.get(), 0, chunk->size_bytes);
		}

		std::cout << "\tDONE, NOW SETTING BITMAP VALUES" << std::endl;

		for (uint64_t idx = this->size_in_bits; idx < this->size_in_bits + 8; ++idx) {
			this->set(idx);
		}

		std::cout << "\tOUT CLEAR ALL" << std::endl;
	}

	Size size_bytes() const {
		// add an extra byte which will be used for padding
		return size_in_bits / 8 + 2;
	}

	Size size_chunks() const {
		return this->size_bytes() / disk->chunk_size() + 1;
	}

	inline Byte &get_byte_for_idx(Size idx) {
		uint64_t byte_idx = idx / 8;
		Byte *data = this->chunks[byte_idx / disk->chunk_size()]->data.get();
		return data[byte_idx % disk->chunk_size()];
	}

	inline const Byte &get_byte_for_idx(Size idx) const {
		uint64_t byte_idx = idx / 8;
		Byte *data = this->chunks[byte_idx / disk->chunk_size()]->data.get();
		return data[byte_idx % disk->chunk_size()];
	}

	inline bool get(Size idx) const {
		Byte byte = get_byte_for_idx(idx);
		return byte & (1 << (idx % 8));
	}

	inline void set(Size idx) {
		Byte& byte = get_byte_for_idx(idx);
		byte |= (1 << (idx % 8));
	}

	inline void clr(Size idx) {
		Byte& byte = get_byte_for_idx(idx);
		byte &= ~(1 << (idx % 8));
	}

	struct BitRange {
		Size start_idx = 0;
		Size bit_count = 0;

		void set_range(DiskBitMap &map) {
			for (Size idx = start_idx; idx < start_idx + bit_count; ++idx) {
				map.set(idx);
			}
		}

		void clr_range(DiskBitMap &map) {
			for (Size idx = start_idx; idx < start_idx + bit_count; ++idx) {
				map.clr(idx);
			}
		} 
	};

	static std::array<BitRange, 256> find_unset_cache;
	static uint64_t find_last_byte_idx;
	
	BitRange find_unset_bits(Size length) const;
};


#endif
