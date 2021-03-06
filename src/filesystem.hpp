#ifndef FILESYSTEM_HPP
#define FILESYSTEM_HPP

#include <bitset>
#include <array>
#include <vector>
#include <memory>
#include <cstdint>

#include "diskinterface.hpp"

using Size = uint64_t;

struct FileSystem;
struct INode;
struct INodeTable;
struct SuperBlock;

struct FileSystemException : public std::exception {
	std::string message;
	FileSystemException(const std::string &message) : message(message) { };
};

struct SuperBlock {
	Disk *disk;
    const uint64_t superblock_size_chunks = 1;
	const uint64_t disk_size_bytes;
    const uint64_t disk_size_chunks;
	const uint64_t disk_chunk_size;

	uint64_t disk_block_map_offset; // chunk in which the disk block map starts
    uint64_t disk_block_map_size_chunks; // number of chunks in disk block map
	std::unique_ptr<DiskBitMap> disk_block_map;

	uint64_t inode_table_offset; // chunk in which the inode table starts
	uint64_t inode_table_size_chunks; // number of chunks in the inode table
	std::unique_ptr<INodeTable> inode_table;

    uint64_t data_offset; //where free chunks begin

	SuperBlock(Disk *disk);

    void init(double inode_table_size_rel_to_disk);
    void load_from_disk(Disk * disk);

	std::shared_ptr<Chunk> allocate_chunk() {
		DiskBitMap::BitRange range = this->disk_block_map->find_unset_bits(1);
		if (range.bit_count != 1) {
			throw FileSystemException("FileSystem out of space -- unable to allocate a new chunk");
		}

		std::shared_ptr<Chunk> chunk = this->disk->get_chunk(range.start_idx);
		this->disk_block_map->set(range.start_idx);

		return std::move(chunk);
	}
};

struct FileSystem {
	Disk *disk;			
	std::unique_ptr<SuperBlock> superblock;

	// the file system, once constructed, takes ownership of the disk
	FileSystem(Disk *disk) : disk(disk), superblock(new SuperBlock(disk)) {
	}
};

struct INode;

struct INodeTable {
	SuperBlock *superblock;
	uint64_t inode_table_size_chunks = 0; // size of the inode table including used_inodes bitmap + ilist 
	uint64_t inode_table_offset = 0; // this actually winds up being the offset of the used_inodes bitmap
	uint64_t inode_ilist_offset = 0; // this ends up storing the calculated real offset of the inodes
	uint64_t inode_count = 0;
	uint64_t inodes_per_chunk = 0;

	SharedObjectCache<uint64_t, INode> inodecache;
	std::unique_ptr<DiskBitMap> used_inodes;
	// struct INode ilist[10]; //TODO: change the size

	// size and offset are in chunks
	INodeTable(SuperBlock *superblock, uint64_t offset_chunks, uint64_t size_chunks);

	void format_inode_table();

	// returns the size of the entire table in chunks
	uint64_t size_chunks();
	uint64_t size_inodes() {
		return inode_count;
	}
	
	INode get_inode(uint64_t idx);
	
	void set_inode(uint64_t idx, INode &node);

	void free_inode(uint64_t idx);
};

struct INode {
	static constexpr uint64_t DIRECT_ADDRESS_COUNT = 8;
	static constexpr uint64_t INDIRECT_ADDRESS_COUNT = 1;
	static constexpr uint64_t DOUBLE_INDIRECT_ADDRESS_COUNT = 1;
	static constexpr uint64_t TRIPPLE_INDIRECT_ADDRESS_COUNT = 1;
	static constexpr uint64_t ADDRESS_COUNT = DIRECT_ADDRESS_COUNT + INDIRECT_ADDRESS_COUNT + DOUBLE_INDIRECT_ADDRESS_COUNT + TRIPPLE_INDIRECT_ADDRESS_COUNT;
	static const uint64_t INDIRECT_TABLE_SIZES[4];

	struct INodeData {
		// we store the data in a subclass so that it can be serialized independently 
		// from data structures that INode needs to keep when loaded in memory
		uint64_t UID = 0; //user id
		uint64_t last_modified = 0; //last modified timestamp
		uint64_t file_size = 0; //size of file
		uint64_t reference_count = 0; //reference count to the inode
		uint64_t addresses[ADDRESS_COUNT] = {0}; //8 direct
		std::bitset<11> inode_bits; //rwxrwxrwx (ow, g, oth) dir special 
	};

	INodeData data;
	SuperBlock *superblock;	

	std::shared_ptr<Chunk> resolve_indirection(uint64_t chunk_number);

	// NOTE: read is NOT const, it will allocate chunks when reading inodes 
	// that have not been written but that ARE within the size of the file,
	// TODO: possibly be smart about this
	uint64_t read(uint64_t starting_offset, char *buf, uint64_t n);
};

#endif